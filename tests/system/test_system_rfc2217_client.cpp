//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RFC 2217 client against Studio's own RFC 2217 server: two studios, as
// two computers, and the network between them is a loopback address.
//

#include "support/test_support.hpp"

#include <vss/ports/listen_transport.hpp>
#include <vss/ports/rfc2217.hpp>
#include <vss/studio/studio.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <boost/asio/post.hpp>

#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

std::uint16_t bound_port(studio& app, const endpoint& port)
{
    const auto address = parse_listen_uri(app.advertised_path(port.id()));
    REQUIRE(address.has_value());
    REQUIRE(address->port != 0);
    return address->port;
}

/// What an endpoint read, in order: bytes as they are, a break as `[break]`.
std::string read_by(const event_recorder& events, endpoint_id id)
{
    std::string out;
    for (const auto& event : events.snapshot())
    {
        if (event.endpoint != id || event.direction != data_direction::rx)
            continue;
        if (event.kind == event_kind::data && event.data)
            out += to_string(bytes_view(*event.data));
        else if (event.kind == event_kind::line_break)
            out += "[break]";
    }
    return out;
}

/// Every event of `app`'s hub, for the test to read back.
struct watch
{
    event_recorder events;
    subscription_guard guard;

    explicit watch(studio& app)
        : guard(app.engine(), event_filter{},
                [recorder = events](const endpoint_event& event)
                { recorder.record(event); })
    {
    }
};

/// A port whose program has set a format, as a driver port reports it: the
/// transport says what the program chose and ends a read to say it changed.
class program_port final : public transport
{
public:
    explicit program_port(const executor_type& executor) : executor_(executor)
    {
    }

    std::string describe() const override { return "program"; }
    bool is_open() const override { return true; }
    bool supports_control_lines() const noexcept override { return true; }

    void async_read_some(boost::asio::mutable_buffer, io_handler handler) override
    {
        std::lock_guard lock(mutex_);
        parked_ = std::move(handler);
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        const std::size_t size = buffer.size();
        boost::asio::post(executor_, [handler = std::move(handler), size]
                          { handler({}, size); });
    }

    void close() override { cancel(); }

    void cancel() override
    {
        io_handler taken;
        {
            std::lock_guard lock(mutex_);
            taken = std::move(parked_);
            parked_ = nullptr;
        }
        if (taken)
            boost::asio::post(executor_,
                              [taken = std::move(taken)]
                              { taken(boost::asio::error::operation_aborted, 0); });
    }

    std::optional<line_format> peer_format() const override
    {
        std::lock_guard lock(mutex_);
        return format_;
    }

    /// The program chose `format`.
    void announce(const line_format& format)
    {
        io_handler taken;
        {
            std::lock_guard lock(mutex_);
            format_ = format;
            taken = std::move(parked_);
            parked_ = nullptr;
        }
        if (taken)
            boost::asio::post(executor_,
                              [taken = std::move(taken)] { taken({}, 0); });
    }

private:
    executor_type executor_;
    mutable std::mutex mutex_;
    std::optional<line_format> format_;
    io_handler parked_;
};

} // namespace

TEST_CASE("system: the speed and framing a program sets reach the server, "
          "when the link is made and when they change")
{
    // The server is played by the test, which reads what the client sends.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, {boost::asio::ip::make_address("127.0.0.1"), 0});

    studio local;
    const auto client = local.attach_serial(
        "client", "rfc2217://127.0.0.1:" +
                      std::to_string(acceptor.local_endpoint().port()));
    REQUIRE(client);
    boost::asio::ip::tcp::socket server(io);
    acceptor.accept(server);

    auto transport = std::make_shared<program_port>(local.engine().executor());
    const auto program = local.engine().add_endpoint(
        "program", endpoint_kind::virtual_port, transport);
    REQUIRE(program);

    // The program chose a format before the link was made.
    line_format first;
    first.baud_rate = 250000;
    first.data_bits = 7;
    first.parity = parity_mode::even;
    first.stop_bits = stop_bits_mode::two;
    transport->announce(first);
    REQUIRE(local.engine().connect((*program)->id(), (*client)->id()));

    const char iac = static_cast<char>(255);
    boost::asio::write(server,
                       boost::asio::buffer(std::string{
                           iac, static_cast<char>(253), 44, iac,
                           static_cast<char>(251), 44}));

    std::string got;
    const auto saw = [&](const std::string& needle)
    {
        const auto until = std::chrono::steady_clock::now() + 5s;
        std::array<char, 512> buffer{};
        while (got.find(needle) == std::string::npos)
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            boost::system::error_code ec;
            if (server.available(ec) == 0)
            {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            got.append(buffer.data(), server.read_some(boost::asio::buffer(buffer), ec));
        }
        return true;
    };

    rfc2217::client_session expect;
    REQUIRE(saw(expect.set_baud_rate(250000)));
    REQUIRE(saw(expect.set_data_size(7)));
    REQUIRE(saw(expect.set_parity(parity_mode::even)));
    REQUIRE(saw(expect.set_stop_size(stop_bits_mode::two)));

    // And it changes its mind: only what changed is sent.
    got.clear();
    line_format second = first;
    second.baud_rate = 31250;
    second.data_bits = 8;
    transport->announce(second);
    REQUIRE(saw(expect.set_baud_rate(31250)));
    REQUIRE(saw(expect.set_data_size(8)));
    REQUIRE(got.find(expect.set_parity(parity_mode::even)) ==
            std::string::npos);
}

TEST_CASE("system: an RFC 2217 client and Studio's server carry bytes both "
          "ways, with 0xFF in them")
{
    studio remote;
    const auto server =
        remote.attach_serial("server", "rfc2217-listen://127.0.0.1:0");
    const auto echo =
        remote.create_virtual_port("echo", virtual_port_kind::loopback);
    REQUIRE(server);
    REQUIRE(echo);
    REQUIRE(remote.connect("echo", "server"));

    studio local;
    const auto client = local.attach_serial(
        "client", "rfc2217://127.0.0.1:" +
                      std::to_string(bound_port(remote, **server)) +
                      "?baud=9600");
    REQUIRE(client);
    REQUIRE((*client)->is_open());
    REQUIRE_EQ((*client)->kind(), endpoint_kind::rfc2217);

    std::string payload = "ping";
    payload.push_back(static_cast<char>(0xFF));
    payload += "end";
    const auto written = (*client)->write_sync(as_bytes(payload), 3000ms);
    REQUIRE(written);
    REQUIRE(wait_for_rx(**client, payload.size(), 5000ms));
}

TEST_CASE("system: the server sees the DTR and RTS of the client, and the "
          "speed and framing of the address")
{
    studio remote;
    const auto server =
        remote.attach_serial("server", "rfc2217-listen://127.0.0.1:0");
    REQUIRE(server);

    studio local;
    const auto client = local.attach_serial(
        "client", "rfc2217://127.0.0.1:" +
                      std::to_string(bound_port(remote, **server)) +
                      "?baud=19200&data=7&parity=even&stop=2");
    REQUIRE(client);

    // The server's port takes DTR and RTS as its outputs; the client starts
    // both high.
    REQUIRE(wait_until([&] { return (*server)->lines().dtr(); }, 5000ms));
    REQUIRE((*server)->lines().rts());

    REQUIRE(!(*client)->set_line(control_line::dtr, false));
    REQUIRE(wait_until([&] { return !(*server)->lines().dtr(); }, 5000ms));
    REQUIRE((*server)->lines().rts());
    REQUIRE(!(*client)->set_line(control_line::rts, false));
    REQUIRE(wait_until([&] { return !(*server)->lines().rts(); }, 5000ms));
}

TEST_CASE("system: a break at the client reaches the server as a break of "
          "about its length")
{
    studio remote;
    const auto server =
        remote.attach_serial("server", "rfc2217-listen://127.0.0.1:0");
    REQUIRE(server);
    watch remote_events(remote);

    studio local;
    const auto client = local.attach_serial(
        "client", "rfc2217://127.0.0.1:" +
                      std::to_string(bound_port(remote, **server)));
    const auto source =
        local.create_virtual_port("source", virtual_port_kind::loopback);
    REQUIRE(client);
    REQUIRE(source);
    REQUIRE(local.connect("source", "client"));
    // Give the negotiation the moment it needs.
    REQUIRE(wait_until([&] { return (*server)->lines().dtr(); }, 5000ms));

    make_break(**source, 50ms);

    REQUIRE(wait_until([&] { return (*server)->stats().breaks_rx == 1; },
                       5000ms));
    REQUIRE_EQ((*client)->stats().breaks_tx, std::uint64_t{1});

    for (const auto& event : remote_events.events.snapshot())
    {
        if (event.kind != event_kind::line_break || event.endpoint !=
                                                       (*server)->id())
            continue;
        if (event.phase == break_phase::begin)
            continue;
        // Between the two commands: the length asked for, plus what the
        // network and the timers add.
        REQUIRE(event.duration >= 40ms);
        REQUIRE(event.duration < 2s);
    }
}

TEST_CASE("system: a break the server reports is a break between the bytes "
          "around it")
{
    // The server is played by the test: two bytes, a NOTIFY-LINESTATE with
    // the break bit, two bytes.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, {boost::asio::ip::make_address("127.0.0.1"), 0});

    studio local;
    watch near_events(local);
    const auto client = local.attach_serial(
        "client",
        "rfc2217://127.0.0.1:" +
            std::to_string(acceptor.local_endpoint().port()) + "?baud=1200");
    REQUIRE(client);

    boost::asio::ip::tcp::socket server(io);
    acceptor.accept(server);
    const char iac = static_cast<char>(255);
    const std::string agree = std::string{iac, static_cast<char>(253), 44} +
                              std::string{iac, static_cast<char>(251), 44};
    std::string notice{iac, static_cast<char>(250), 44, 106, 0x10, iac,
                       static_cast<char>(240)};
    boost::asio::write(server, boost::asio::buffer(agree));
    std::this_thread::sleep_for(100ms);
    boost::asio::write(server, boost::asio::buffer("ab" + notice + "cd"));

    REQUIRE(wait_until([&] { return (*client)->stats().breaks_rx == 1; },
                       5000ms));
    REQUIRE(wait_for_rx(**client, 4, 5000ms));
    REQUIRE_EQ(read_by(near_events.events, (*client)->id()),
               std::string("ab[break]cd"));

    for (const auto& event : near_events.events.snapshot())
    {
        if (event.kind != event_kind::line_break)
            continue;
        REQUIRE(!event.measured);
        // One character time at 1200 baud.
        REQUIRE(event.duration > 8ms);
        REQUIRE(event.duration < 9ms);
    }
}

TEST_CASE("system: an address that nobody listens on is refused when the port "
          "is made, and a server that goes away stops the port")
{
    studio local;
    std::uint16_t port = 0;
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor(
            io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        port = acceptor.local_endpoint().port();
    }
    REQUIRE(!local.attach_serial(
        "client", "rfc2217://127.0.0.1:" + std::to_string(port)));

    studio remote;
    const auto server =
        remote.attach_serial("server", "rfc2217-listen://127.0.0.1:0");
    REQUIRE(server);
    const auto client = local.attach_serial(
        "client", "rfc2217://127.0.0.1:" +
                      std::to_string(bound_port(remote, **server)));
    REQUIRE(client);
    REQUIRE((*client)->is_open());

    (*server)->close();
    REQUIRE(wait_until([&] { return !(*client)->is_open(); }, 5000ms));
}
