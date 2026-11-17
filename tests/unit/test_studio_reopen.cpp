//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reopening a port from what the studio kept of it, the retry of a lost
// connection, and the ports of the Windows driver that a restart takes away.
//

#include "support/scripted_transport.hpp"
#include "support/test_support.hpp"

#include <vss/studio/studio.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;

namespace
{
using namespace std::chrono_literals;
namespace asio = boost::asio;

/// A TCP server on loopback that can drop its client, stop listening and start
/// again on the same port.
class tcp_server
{
public:
    tcp_server()
    {
        listen(0);
        thread_ = std::thread([this] { io_.run(); });
    }

    ~tcp_server()
    {
        try
        {
            io_.stop();
        }
        catch (...)
        {
            // best effort: a destructor must not throw, and nothing can act on a failed stop
        }
        if (thread_.joinable())
            thread_.join();
    }

    std::uint16_t port() const { return port_; }

    std::size_t accepted() const { return accepted_.load(); }

    std::string received() const
    {
        std::lock_guard lock(mutex_);
        return received_;
    }

    /// Hangs up on the client, as a server that restarted would.
    void drop()
    {
        asio::post(io_, [this] {
            boost::system::error_code ignored;
            if (socket_)
                socket_->close(ignored);
            socket_.reset();
        });
    }

    /// Refuses new connections until #resume.
    void pause()
    {
        std::promise<void> done;
        asio::post(io_, [this, &done] {
            boost::system::error_code ignored;
            acceptor_->close(ignored);
            done.set_value();
        });
        done.get_future().wait();
    }

    void resume()
    {
        std::promise<void> done;
        asio::post(io_, [this, &done] {
            listen(port_);
            done.set_value();
        });
        done.get_future().wait();
    }

    void send(const std::string& text)
    {
        std::promise<void> done;
        asio::post(io_, [this, &text, &done] {
            boost::system::error_code ignored;
            if (socket_)
                asio::write(*socket_, asio::buffer(text), ignored);
            done.set_value();
        });
        done.get_future().wait();
    }

private:
    void listen(std::uint16_t port)
    {
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_);
        boost::system::error_code error;
        acceptor_->open(asio::ip::tcp::v4(), error);
        REQUIRE(!error);
        acceptor_->set_option(asio::socket_base::reuse_address(true), error);
        // Loopback only: a bind to every address makes Windows Firewall ask.
        acceptor_->bind(asio::ip::tcp::endpoint(
                            asio::ip::address_v4::loopback(), port),
                        error);
        REQUIRE(!error);
        port_ = acceptor_->local_endpoint().port();
        acceptor_->listen(asio::socket_base::max_listen_connections, error);
        REQUIRE(!error);
        accept();
    }

    void accept()
    {
        acceptor_->async_accept(
            [this](boost::system::error_code error, asio::ip::tcp::socket socket) {
                if (error)
                    return;
                socket_ = std::move(socket);
                accepted_.fetch_add(1);
                read();
                accept();
            });
    }

    void read()
    {
        if (!socket_)
            return;
        socket_->async_read_some(
            asio::buffer(buffer_),
            [this](boost::system::error_code error, std::size_t count) {
                if (error)
                    return;
                {
                    std::lock_guard lock(mutex_);
                    received_.append(buffer_.data(), count);
                }
                read();
            });
    }

    mutable std::mutex mutex_;
    asio::io_context io_;
    // Keeps run() going while no accept or read is pending.
    asio::executor_work_guard<asio::io_context::executor_type> guard_{
        io_.get_executor()};
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::thread thread_;
    std::array<char, 4096> buffer_{};
    std::uint16_t port_ = 0;
    std::atomic<std::size_t> accepted_{0};
    std::string received_;
};

std::string uri_of(const tcp_server& server)
{
    return "tcp://127.0.0.1:" + std::to_string(server.port());
}

bool wait_for_state(const std::shared_ptr<endpoint>& port, endpoint_state state,
                    std::chrono::milliseconds limit = 5000ms)
{
    return wait_until([&] { return port->state() == state; }, limit);
}

/// The log messages an endpoint has published, as text.
struct log_watch
{
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::string>> lines =
        std::make_shared<std::vector<std::string>>();
    studio& app;
    subscriber_id id = 0;

    explicit log_watch(studio& studio_app) : app(studio_app)
    {
        id = app.engine().subscribe(
            event_filter{std::nullopt, event_kind::log_message},
            [mutex = mutex, lines = lines](const endpoint_event& event) {
                std::lock_guard lock(*mutex);
                lines->push_back(event.message);
            });
    }

    ~log_watch() { app.engine().unsubscribe(id); }

    bool saw(const std::string& fragment) const
    {
        std::lock_guard lock(*mutex);
        for (const auto& line : *lines)
        {
            if (line.find(fragment) != std::string::npos)
                return true;
        }
        return false;
    }
};

/// Stands in for opening an address, so that a test decides when a connection
/// comes up.  Every call makes a scripted transport.  After #hold_next the
/// next call waits inside the factory until #release, as a connection that
/// takes a while does.
class held_opener
{
public:
    held_opener() = default;
    held_opener(const held_opener&) = delete;
    held_opener& operator=(const held_opener&) = delete;

    /// A test that fails must not leave a thread waiting here.
    ~held_opener() { release(); }

    studio_options options()
    {
        studio_options made;
        made.make_transport = [this](const transport::executor_type& executor,
                                     const std::string&,
                                     const serial_settings*, error_code&)
        { return open(executor); };
        return made;
    }

    void hold_next()
    {
        std::lock_guard lock(mutex_);
        hold_ = true;
        entered_ = false;
        released_ = false;
    }

    bool wait_entered(std::chrono::milliseconds limit = 10000ms)
    {
        return wait_until(
            [this]
            {
                std::lock_guard lock(mutex_);
                return entered_;
            },
            limit);
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

    /// How many transports were made.
    std::size_t opened() const
    {
        std::lock_guard lock(mutex_);
        return made_.size();
    }

    std::shared_ptr<vss_test_support::scripted_transport>
    transport_at(std::size_t index) const
    {
        std::lock_guard lock(mutex_);
        return made_.at(index);
    }

    /// How many owners transport `index` has, this opener being one: 1 means
    /// nothing else holds it.
    long owners_of(std::size_t index) const
    {
        std::lock_guard lock(mutex_);
        return made_.at(index).use_count();
    }

private:
    std::shared_ptr<transport> open(const transport::executor_type& executor)
    {
        auto made =
            std::make_shared<vss_test_support::scripted_transport>(executor);
        std::unique_lock lock(mutex_);
        made_.push_back(made);
        if (hold_)
        {
            hold_ = false;
            entered_ = true;
            // The limit is only there so that a broken test cannot hang.
            changed_.wait_for(lock, 60s, [this] { return released_; });
        }
        return made;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::shared_ptr<vss_test_support::scripted_transport>> made_;
    bool hold_ = false;
    bool entered_ = false;
    bool released_ = false;
};

config::port_entry client_entry(const std::string& name)
{
    config::port_entry port;
    port.name = name;
    port.kind = "tcp";
    port.uri = "tcp://127.0.0.1:4001";
    return port;
}

/// True when transport `index` of the opener was closed, never had a read
/// armed, and is held by nothing but the opener.
bool unused_and_released(const held_opener& opener, std::size_t index)
{
    const auto made = opener.transport_at(index);
    // `made` is an owner too.
    return wait_until(
               [&] { return !made->is_open() && opener.owners_of(index) == 2; },
               10000ms) &&
           made->reads_armed() == 0;
}

reconnect_policy quick_policy(unsigned max_attempts = 0)
{
    reconnect_policy policy;
    policy.enabled = true;
    policy.first_delay = 40ms;
    policy.max_delay = 160ms;
    policy.max_attempts = max_attempts;
    return policy;
}

} // namespace

TEST_CASE("reconnect: the delay doubles up to the longest")
{
    reconnect_policy policy;
    policy.first_delay = 1000ms;
    policy.max_delay = 5000ms;
    REQUIRE_EQ(backoff_delay(policy, 0).count(), 1000);
    REQUIRE_EQ(backoff_delay(policy, 1).count(), 2000);
    REQUIRE_EQ(backoff_delay(policy, 2).count(), 4000);
    REQUIRE_EQ(backoff_delay(policy, 3).count(), 5000);
    REQUIRE_EQ(backoff_delay(policy, 40).count(), 5000);

    // Odd settings still give a wait the thread can use.
    policy.first_delay = 0ms;
    policy.max_delay = 0ms;
    REQUIRE_EQ(backoff_delay(policy, 0).count(), 1);
    REQUIRE_EQ(backoff_delay(policy, 9).count(), 1);
    policy.first_delay = 9000ms;
    policy.max_delay = 2000ms;
    REQUIRE_EQ(backoff_delay(policy, 0).count(), 2000);
}

TEST_CASE("reopen: a TCP client whose server hung up is opened again")
{
    tcp_server server;
    studio app;
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.drop();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));

    REQUIRE(app.reopen((*port)->id()));
    REQUIRE((*port)->is_open());
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));

    // The same endpoint carries bytes both ways again.
    const auto written = (*port)->write_sync(as_bytes("again"), 3000ms);
    REQUIRE(written);
    REQUIRE(wait_until([&] { return server.received() == "again"; }, 5000ms));
    server.send("back");
    REQUIRE(wait_for_rx(**port, 4, 5000ms));
}

TEST_CASE("reopen: the links of the port stay")
{
    tcp_server server;
    studio app;
    const auto remote = app.attach_serial("remote", uri_of(server));
    REQUIRE(remote);
    const auto local = app.create_virtual_port("local", virtual_port_kind::loopback);
    REQUIRE(local);
    const auto link = app.connect("remote", "local");
    REQUIRE(link);
    // The server must hold the connection before it can hang up on it.
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.drop();
    REQUIRE(wait_for_state(*remote, endpoint_state::closed));
    REQUIRE(app.reopen((*remote)->id()));
    // The client side connects first: until the server has accepted the new
    // connection, what it sends goes nowhere.
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));

    // A link made before the loss still joins the two.
    REQUIRE_EQ(app.link_infos().size(), std::size_t{1});
    server.send("through");
    REQUIRE(wait_until(
        [&] { return (*local)->stats().bytes_tx >= 7; }, 5000ms));
}

TEST_CASE("reopen: a server that is still down reports why and the port stays down")
{
    tcp_server server;
    studio app;
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);

    server.pause();
    server.drop();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));

    const auto failed = app.reopen((*port)->id());
    REQUIRE(!failed);
    REQUIRE((*port)->state() == endpoint_state::closed);

    server.resume();
    REQUIRE(app.reopen((*port)->id()));
    REQUIRE((*port)->is_open());
}

TEST_CASE("reopen: what cannot be made again says so")
{
    studio app;
    REQUIRE_EQ(app.reopen(9999), make_error_code(errc::not_found));

    const auto loopback =
        app.create_virtual_port("loop", virtual_port_kind::loopback);
    REQUIRE(loopback);
    REQUIRE_EQ(app.reopen((*loopback)->id()),
               make_error_code(errc::unsupported));

    const auto fixture = app.create_mock_port("fixture");
    REQUIRE(fixture);
    REQUIRE_EQ(app.reopen((*fixture)->id()), make_error_code(errc::unsupported));
}

TEST_CASE("reopen: a listener binds its address again")
{
    studio app;
    const auto listener = app.attach_serial("wait", "tcp-listen://127.0.0.1:0");
    REQUIRE(listener);
    REQUIRE((*listener)->is_open());

    // Restarting a listener on port 0 gives it a fresh port; it stays open.
    REQUIRE(app.reopen((*listener)->id()));
    REQUIRE((*listener)->is_open());
}

TEST_CASE("reconnect: it is off unless asked for")
{
    REQUIRE(!reconnect_policy{}.enabled);

    tcp_server server;
    studio app;
    REQUIRE(!app.current_reconnect_policy().enabled);
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.drop();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));
    std::this_thread::sleep_for(400ms);
    REQUIRE((*port)->state() == endpoint_state::closed);
    REQUIRE_EQ(server.accepted(), std::size_t{1});
}

TEST_CASE("reconnect: a lost connection is opened again, and it says so")
{
    tcp_server server;
    studio app;
    log_watch watch(app);
    app.set_reconnect_policy(quick_policy());
    REQUIRE(app.current_reconnect_policy() == quick_policy());

    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.drop();
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));
    REQUIRE((*port)->is_open());
    REQUIRE(wait_until([&] { return watch.saw("connection lost"); }, 5000ms));
    REQUIRE(wait_until([&] { return watch.saw("reconnected"); }, 5000ms));

    // And again: every loss starts a fresh sequence.
    server.drop();
    REQUIRE(wait_until([&] { return server.accepted() == 3; }, 5000ms));
    const auto written = (*port)->write_sync(as_bytes("alive"), 3000ms);
    REQUIRE(written);
    REQUIRE(wait_until([&] { return server.received() == "alive"; }, 5000ms));
}

TEST_CASE("reconnect: a client made while the server refuses connects later")
{
    tcp_server server;
    server.pause();
    studio app;
    log_watch watch(app);
    app.set_reconnect_policy(quick_policy());

    // The ordinary way is refused while the server is down.
    REQUIRE(!app.attach_serial("remote", uri_of(server)));
    // The waiting way makes the port and keeps trying.
    const auto port = app.attach_waiting("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{1});
    REQUIRE(
        wait_until([&] { return watch.saw("could not reconnect"); }, 5000ms));

    server.resume();
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));

    // An address that does not connect out has nothing to wait for.
    REQUIRE_EQ(app.attach_waiting("net", "tcp-listen://127.0.0.1:0").error(),
               make_error_code(errc::invalid_uri));
}

TEST_CASE("stop_port: a stopped client stays closed and is not retried")
{
    tcp_server server;
    studio app;
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));
    REQUIRE(app.port_stopped((*port)->id()).value_or(true) == false);
    const auto status = app.connection((*port)->id());
    REQUIRE(status);
    REQUIRE(!status->retrying);

    REQUIRE(app.stop_port((*port)->id()));
    REQUIRE(wait_for_state(*port, endpoint_state::closed));
    REQUIRE(app.port_stopped((*port)->id()).value_or(false) == true);
    // The port and its record are kept, and nothing opens it again even
    // though the retry policy is on.
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{1});
    std::this_thread::sleep_for(500ms);
    REQUIRE_EQ(server.accepted(), 1);
    REQUIRE(!app.connection((*port)->id())->retrying);

    REQUIRE(app.start_port((*port)->id()));
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));
    REQUIRE(app.port_stopped((*port)->id()).value_or(true) == false);
}

TEST_CASE("stop_port: a port with no address cannot be stopped")
{
    studio app;
    const auto port = app.create_virtual_port("plain",
                                              virtual_port_kind::loopback);
    REQUIRE(port);
    REQUIRE(!app.port_stopped((*port)->id()).has_value());
    REQUIRE_EQ(app.stop_port((*port)->id()).error(),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(app.stop_port(9999).error(), make_error_code(errc::not_found));
}

TEST_CASE("stop_port: starting a client whose server refuses keeps trying")
{
    tcp_server server;
    studio app;
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));
    REQUIRE(app.stop_port((*port)->id()));

    server.pause();
    REQUIRE(app.start_port((*port)->id()));
    REQUIRE(wait_until(
        [&] { return app.connection((*port)->id())->retrying; }, 5000ms));
    server.resume();
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));
}

TEST_CASE("pause_port: a paused client stays closed, is saved and resumes")
{
    tcp_server server;
    studio app;
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));
    REQUIRE(app.port_paused((*port)->id()).value_or(true) == false);

    REQUIRE(app.pause_port((*port)->id()));
    REQUIRE(wait_for_state(*port, endpoint_state::closed));
    REQUIRE(app.port_paused((*port)->id()).value_or(false) == true);
    // A pause is not a stop.
    REQUIRE(app.port_stopped((*port)->id()).value_or(true) == false);
    std::this_thread::sleep_for(500ms);
    REQUIRE_EQ(server.accepted(), 1);
    REQUIRE(!app.connection((*port)->id())->retrying);

    // The setup carries the pause; a stop is not carried.
    const auto saved = app.snapshot();
    REQUIRE_EQ(saved.ports.size(), std::size_t{1});
    REQUIRE(saved.ports.front().paused);

    REQUIRE(app.resume_port((*port)->id()));
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));
    REQUIRE(app.port_paused((*port)->id()).value_or(true) == false);
    REQUIRE(!app.snapshot().ports.front().paused);

    REQUIRE(app.stop_port((*port)->id()));
    REQUIRE(!app.snapshot().ports.front().paused);
    REQUIRE(app.port_paused((*port)->id()).value_or(true) == false);
}

TEST_CASE("pause_port: a setup with a paused port opens it paused")
{
    tcp_server server;
    studio first;
    REQUIRE(first.attach_serial("remote", uri_of(server)));
    auto document = first.snapshot();
    REQUIRE_EQ(document.ports.size(), std::size_t{1});
    document.ports.front().paused = true;

    studio second;
    REQUIRE(second.apply(document));
    const auto port = second.find("remote");
    REQUIRE(port != nullptr);
    REQUIRE(second.port_paused(port->id()).value_or(false));
    REQUIRE(second.snapshot().ports.front().paused);
    REQUIRE(second.resume_port(port->id()));
    REQUIRE(wait_for_state(port, endpoint_state::open));
}

TEST_CASE("pause_port: a port with no address cannot be paused")
{
    studio app;
    const auto port = app.create_virtual_port("plain",
                                              virtual_port_kind::loopback);
    REQUIRE(port);
    REQUIRE(!app.port_paused((*port)->id()).has_value());
    REQUIRE_EQ(app.pause_port((*port)->id()).error(),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(app.pause_port(9999).error(), make_error_code(errc::not_found));
}

TEST_CASE("reconnect: it keeps trying while the server is down")
{
    tcp_server server;
    studio app;
    log_watch watch(app);
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.pause();
    server.drop();
    REQUIRE(wait_until([&] { return watch.saw("could not reconnect"); }, 5000ms));
    REQUIRE((*port)->state() == endpoint_state::closed);

    server.resume();
    REQUIRE(wait_until([&] { return server.accepted() == 2; }, 5000ms));
    REQUIRE(wait_for_state(*port, endpoint_state::open));
}

TEST_CASE("reconnect: it gives up after the most tries asked for")
{
    tcp_server server;
    studio app;
    log_watch watch(app);
    app.set_reconnect_policy(quick_policy(2));
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.pause();
    server.drop();
    REQUIRE(wait_until([&] { return watch.saw("giving up after 2 tries"); },
                       5000ms));

    // Gave up: the server coming back changes nothing until it is asked.
    server.resume();
    std::this_thread::sleep_for(400ms);
    REQUIRE((*port)->state() == endpoint_state::closed);
    REQUIRE(app.reopen((*port)->id()));
}

TEST_CASE("reconnect: removing the port ends its tries")
{
    tcp_server server;
    studio app;
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.pause();
    server.drop();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));
    REQUIRE(app.remove_port((*port)->id()));

    server.resume();
    std::this_thread::sleep_for(400ms);
    REQUIRE_EQ(server.accepted(), std::size_t{1});
    REQUIRE(app.endpoint_infos().empty());
}

TEST_CASE("reconnect: only a client that connects out is retried")
{
    studio app;
    log_watch watch(app);
    app.set_reconnect_policy(quick_policy());

    // A listener and an in-process port that stop are not retried.
    const auto listener = app.attach_serial("wait", "tcp-listen://127.0.0.1:0");
    REQUIRE(listener);
    const auto loopback =
        app.create_virtual_port("loop", virtual_port_kind::loopback);
    REQUIRE(loopback);
    (*listener)->close();
    (*loopback)->close();
    REQUIRE(wait_for_state(*listener, endpoint_state::closed));
    std::this_thread::sleep_for(300ms);
    REQUIRE(!watch.saw("connection lost"));
    REQUIRE((*listener)->state() == endpoint_state::closed);
}

TEST_CASE("reconnect: turning the policy off stops the tries that are waiting")
{
    tcp_server server;
    studio app;
    reconnect_policy slow = quick_policy();
    slow.first_delay = 300ms;
    slow.max_delay = 300ms;
    app.set_reconnect_policy(slow);
    const auto port = app.attach_serial("remote", uri_of(server));
    REQUIRE(port);
    REQUIRE(wait_until([&] { return server.accepted() == 1; }, 5000ms));

    server.drop();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));
    app.set_reconnect_policy(reconnect_policy{});
    std::this_thread::sleep_for(700ms);
    REQUIRE_EQ(server.accepted(), std::size_t{1});
    REQUIRE((*port)->state() == endpoint_state::closed);
}

TEST_CASE("lost ports: a studio without the Windows driver has none")
{
    studio app;
    const auto loopback =
        app.create_virtual_port("loop", virtual_port_kind::loopback);
    REQUIRE(loopback);
    REQUIRE(app.lost_ports().empty());
    const auto report = app.recreate_lost_ports();
    REQUIRE(report.recreated.empty());
    REQUIRE(report.failed.empty());
}

TEST_CASE("reopen: a port removed while its new connection is made stays "
          "removed")
{
    held_opener opener;
    studio app(opener.options());
    const auto port = app.attach_serial("remote", "tcp://127.0.0.1:4001");
    REQUIRE(port);
    const endpoint_id id = (*port)->id();
    (*port)->close();
    REQUIRE(wait_for_state(*port, endpoint_state::closed));

    // The connection takes its time, and the port is removed meanwhile.
    opener.hold_next();
    auto outcome = std::async(std::launch::async, [&app, id]
                              { return app.reopen(id); });
    REQUIRE(opener.wait_entered());
    REQUIRE(app.remove_port(id));
    opener.release();

    const auto reopened = outcome.get();
    REQUIRE(!reopened);
    REQUIRE_EQ(reopened.error(), make_error_code(errc::not_found));
    REQUIRE_EQ(opener.opened(), std::size_t{2});
    // The new connection was closed and is held by nothing.
    REQUIRE(unused_and_released(opener, 1));
    REQUIRE(app.engine().find_endpoint(id) == nullptr);
    REQUIRE(app.endpoint_infos().empty());
}

TEST_CASE("reopen: a port that File > Open replaces while its new connection "
          "is made stays removed")
{
    held_opener opener;
    studio app(opener.options());
    config::document first;
    first.name = "first";
    first.ports = {client_entry("remote")};
    REQUIRE(app.apply(first));
    const auto old_port = app.find("remote");
    REQUIRE(old_port);
    const endpoint_id id = old_port->id();
    old_port->close();
    REQUIRE(wait_for_state(old_port, endpoint_state::closed));

    opener.hold_next();
    auto outcome = std::async(std::launch::async, [&app, id]
                              { return app.reopen(id); });
    REQUIRE(opener.wait_entered());

    // A setup in which the port differs: it is removed and made again.
    config::port_entry changed = client_entry("remote");
    changed.capture = false;
    config::document second;
    second.name = "second";
    second.ports = {changed};
    REQUIRE(app.replace(second));
    opener.release();

    const auto reopened = outcome.get();
    REQUIRE(!reopened);
    REQUIRE_EQ(reopened.error(), make_error_code(errc::not_found));
    REQUIRE_EQ(opener.opened(), std::size_t{3});
    REQUIRE(unused_and_released(opener, 1));
    REQUIRE(app.engine().find_endpoint(id) == nullptr);

    // What replace made is the only port, and it runs.
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{1});
    const auto made = app.find("remote");
    REQUIRE(made);
    REQUIRE(made->id() != id);
    const auto kept = opener.transport_at(2);
    REQUIRE(wait_until(
        [&] { return kept->is_open() && kept->reads_armed() > 0; }, 10000ms));
    REQUIRE(made->is_open());
}

TEST_CASE("reconnect: a try that is under way when the port is removed does "
          "not bring it back")
{
    held_opener opener;
    studio app(opener.options());
    app.set_reconnect_policy(quick_policy());
    const auto port = app.attach_serial("remote", "tcp://127.0.0.1:4001");
    REQUIRE(port);
    const endpoint_id id = (*port)->id();

    // The connection is lost, and the reconnector is inside its first try.
    opener.hold_next();
    (*port)->close();
    REQUIRE(opener.wait_entered());
    REQUIRE(app.remove_port(id));
    opener.release();

    REQUIRE(unused_and_released(opener, 1));
    REQUIRE(app.engine().find_endpoint(id) == nullptr);
    REQUIRE(app.endpoint_infos().empty());

    // And it does not try again for a port that is gone.
    std::this_thread::sleep_for(500ms);
    REQUIRE_EQ(opener.opened(), std::size_t{2});
}

TEST_CASE("reopen: a removal that races the hand-over leaves nothing running")
{
    // Whether the removal comes before the hand-over, during it or after it
    // depends on the threads; each is allowed to happen, and each must end
    // with the port gone and the new transport closed and released.
    held_opener opener;
    studio app(opener.options());
    for (std::size_t round = 0; round < 25; ++round)
    {
        // The test keeps no handle on the endpoint, which would hold the
        // transport it was given until the end of the round.
        endpoint_id id = 0;
        {
            const auto port =
                app.attach_serial("remote", "tcp://127.0.0.1:4001");
            REQUIRE(port);
            id = (*port)->id();
            (*port)->close();
            REQUIRE(wait_for_state(*port, endpoint_state::closed));
        }

        opener.hold_next();
        auto outcome = std::async(std::launch::async, [&app, id]
                                  { return app.reopen(id); });
        REQUIRE(opener.wait_entered());
        opener.release();
        REQUIRE(app.remove_port(id));
        static_cast<void>(outcome.get());

        const std::size_t fresh = opener.opened() - 1;
        const auto made = opener.transport_at(fresh);
        REQUIRE(wait_until([&] { return !made->is_open(); }, 10000ms));
        // `made` is an owner too.
        REQUIRE(wait_until([&] { return opener.owners_of(fresh) == 2; },
                           10000ms));
        REQUIRE(app.engine().find_endpoint(id) == nullptr);
        REQUIRE(app.endpoint_infos().empty());
    }
}

TEST_CASE("reconfigure: a port removed while its new connection is made stays "
          "removed")
{
    held_opener opener;
    studio app(opener.options());
    const auto port = app.attach_serial("device", "serial://not-a-device");
    REQUIRE(port);
    const endpoint_id id = (*port)->id();

    opener.hold_next();
    serial_settings settings;
    settings.baud_rate = 9600;
    auto outcome = std::async(std::launch::async, [&app, id, settings]
                              { return app.reconfigure_port(id, settings); });
    REQUIRE(opener.wait_entered());
    REQUIRE(app.remove_port(id));
    opener.release();

    const auto reconfigured = outcome.get();
    REQUIRE(!reconfigured);
    REQUIRE_EQ(reconfigured.error(), make_error_code(errc::not_found));
    REQUIRE(unused_and_released(opener, 1));
    REQUIRE(app.engine().find_endpoint(id) == nullptr);
}
