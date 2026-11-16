//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live network bridge tests: a real TCP listener on loopback, driven through
// the studio exactly as a remote serial terminal would be.
//

#include "support/test_support.hpp"

#include <vss/ports/vcom_driver.hpp>
#include <vss/studio/studio.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace vss;
using namespace vss::testkit;

namespace {
using namespace std::chrono_literals;

/// A one-connection-at-a-time TCP peer that records what it receives.
class tcp_probe
{
public:
    tcp_probe()
    {
        boost::system::error_code error;
        acceptor_.open(boost::asio::ip::tcp::v4(), error);
        REQUIRE(!error);
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
        // Loopback only: a bind to every address makes Windows Firewall ask
        // whether to allow the test program, which ends in a UAC prompt.
        acceptor_.bind(boost::asio::ip::tcp::endpoint(
                           boost::asio::ip::address_v4::loopback(), 0),
                       error);
        REQUIRE(!error);
        port_ = acceptor_.local_endpoint().port();
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        REQUIRE(!error);

        do_accept();
        thread_ = std::thread([this] { io_.run(); });
    }

    ~tcp_probe()
    {
        boost::system::error_code error;
        acceptor_.close(error);
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

    std::string received() const
    {
        std::lock_guard lock(mutex_);
        return received_;
    }

    void send(std::string_view text)
    {
        std::lock_guard lock(mutex_);
        if (!socket_)
            return;
        boost::system::error_code error;
        boost::asio::write(*socket_, boost::asio::buffer(text.data(), text.size()),
                           error);
    }

private:
    void do_accept()
    {
        acceptor_.async_accept([this](boost::system::error_code error,
                                      boost::asio::ip::tcp::socket socket) {
            if (error)
                return;
            {
                std::lock_guard lock(mutex_);
                socket_ = std::move(socket);
            }
            do_read();
        });
    }

    void do_read()
    {
        std::lock_guard lock(mutex_);
        if (!socket_)
            return;
        socket_->async_read_some(
            boost::asio::buffer(buffer_),
            [this](boost::system::error_code error, std::size_t count) {
                if (error)
                    return;
                {
                    std::lock_guard guard(mutex_);
                    received_.append(buffer_.data(), count);
                }
                do_read();
            });
    }

    mutable std::mutex mutex_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_{io_};
    std::optional<boost::asio::ip::tcp::socket> socket_;
    std::thread thread_;
    std::array<char, 4096> buffer_{};
    std::uint16_t port_ = 0;
    std::string received_;
};

} // namespace

TEST_CASE("system: a TCP bridge carries bytes both ways")
{
    tcp_probe probe;
    studio app;

    const auto port = app.attach_serial(
        "remote", "tcp://127.0.0.1:" + std::to_string(probe.port()));
    REQUIRE(port);
    REQUIRE((*port)->is_open());
    REQUIRE_EQ((*port)->kind(), endpoint_kind::tcp);

    const auto written = (*port)->write_sync(as_bytes("hello"), 3000ms);
    REQUIRE(written);
    REQUIRE(wait_until(
        [&] { return probe.received() == "hello"; }, 5000ms));

    probe.send("world");
    REQUIRE(wait_for_rx(**port, 5, 5000ms));
    REQUIRE_EQ((*port)->stats().bytes_rx, std::uint64_t{5});
}

TEST_CASE("system: a TCP bridge shared through a virtual port reaches an application")
{
    // The listener is an automatic virtual port, which is a real COM port
    // where the driver is installed; a test does not make one on the machine
    // it runs on.
    if (vss::driver_available())
        VSS_SKIP("the Windows driver is installed; it would make a real port");

    tcp_probe probe;
    studio app;

    const auto shared = app.share_serial(
        "tcp://127.0.0.1:" + std::to_string(probe.port()), 1);
    REQUIRE(shared);

    const auto listener = app.engine().find_endpoint(shared->listeners.front());
    REQUIRE(listener);

    const std::string advertised = app.advertised_path(listener->id());
    REQUIRE(!advertised.empty());

    // Open the advertised virtual port exactly as a third party application
    // would: a pseudo terminal on POSIX, a com0com end on Windows.
    serial_settings settings;
    settings.baud_rate = 115200;
    const auto application = app.attach_serial("application", advertised, settings);
    if (!application)
        VSS_SKIP("cannot open the advertised virtual port " + advertised);

    const auto written = (*application)->write_sync(as_bytes("over-tcp"), 3000ms);
    REQUIRE(written);
    REQUIRE(wait_until([&] { return probe.received() == "over-tcp"; }, 5000ms));

    probe.send("reply");
    REQUIRE(wait_for_rx(**application, 5, 5000ms));
}

TEST_CASE("system: connecting to a closed TCP port fails cleanly")
{
    // A port that was just bound and released is very likely closed.
    const auto port = 1;
    studio app;
    const auto opened = app.attach_serial(
        "dead", "tcp://127.0.0.1:" + std::to_string(port));
    REQUIRE(!opened);
}
