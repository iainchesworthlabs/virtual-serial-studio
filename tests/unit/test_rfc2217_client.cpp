//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RFC 2217 client: the protocol session on its own, then the transport
// against a server played by the test.
//

#include <vss/ports/rfc2217.hpp>
#include <vss/ports/rfc2217_client.hpp>

#include "test_framework.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace
{

constexpr char iac = static_cast<char>(255);
constexpr char dont = static_cast<char>(254);
constexpr char do_ = static_cast<char>(253);
constexpr char wont = static_cast<char>(252);
constexpr char will = static_cast<char>(251);
constexpr char sb = static_cast<char>(250);
constexpr char se = static_cast<char>(240);
constexpr char com_port = 44;

std::string command(char verb, char option)
{
    return std::string{iac, verb, option};
}

/// A server notice: IAC SB COM-PORT code value IAC SE.
std::string notice(char code, const std::string& value)
{
    std::string out{iac, sb, com_port, code};
    for (const char byte : value)
    {
        out.push_back(byte);
        if (byte == iac)
            out.push_back(byte);
    }
    out += std::string{iac, se};
    return out;
}

constexpr char notify_linestate = 106;
constexpr char notify_modemstate = 107;

/// An io_context that runs on its own thread, like the hub's.
struct loop
{
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{
        io.get_executor()};
    std::thread thread{[this] { io.run(); }};

    ~loop()
    {
        guard.reset();
        io.stop();
        thread.join();
    }
};

/// The remote computer: a socket the test reads and writes by hand.
struct server
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{
        io, {asio::ip::make_address("127.0.0.1"), 0}};
    asio::ip::tcp::socket socket{io};
    std::string got;

    std::uint16_t port() const { return acceptor.local_endpoint().port(); }
    std::string uri(const std::string& query = {}) const
    {
        return "rfc2217://127.0.0.1:" + std::to_string(port()) + query;
    }

    void accept() { acceptor.accept(socket); }

    void send(const std::string& bytes)
    {
        asio::write(socket, asio::buffer(bytes));
    }

    /// Reads until what came contains `needle`.
    bool wait_for(const std::string& needle,
                  std::chrono::milliseconds wait = 3000ms)
    {
        const auto until = std::chrono::steady_clock::now() + wait;
        std::array<char, 1024> buffer{};
        while (got.find(needle) == std::string::npos)
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            boost::system::error_code ec;
            const std::size_t ready = socket.available(ec);
            if (ready == 0)
            {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const std::size_t count =
                socket.read_some(asio::buffer(buffer), ec);
            if (ec)
                return false;
            got.append(buffer.data(), count);
        }
        return true;
    }

    /// The server agrees to the COM-PORT option.
    void agree()
    {
        send(command(do_, com_port) + command(will, com_port));
    }
};

struct read_result
{
    boost::system::error_code error;
    std::string text;
};

read_result read_from(transport& port, std::chrono::milliseconds wait = 3000ms)
{
    auto promise = std::make_shared<std::promise<read_result>>();
    auto buffer = std::make_shared<std::array<char, 256>>();
    port.async_read_some(
        asio::buffer(*buffer),
        [promise, buffer](boost::system::error_code ec, std::size_t count)
        { promise->set_value({ec, std::string(buffer->data(), count)}); });
    auto future = promise->get_future();
    if (future.wait_for(wait) != std::future_status::ready)
    {
        port.cancel();
        future.wait();
        return {asio::error::timed_out, {}};
    }
    return future.get();
}

boost::system::error_code write_to(transport& port, const std::string& text)
{
    auto promise =
        std::make_shared<std::promise<boost::system::error_code>>();
    port.async_write_some(asio::buffer(text),
                          [promise](boost::system::error_code ec, std::size_t)
                          { promise->set_value(ec); });
    auto future = promise->get_future();
    REQUIRE(future.wait_for(3000ms) == std::future_status::ready);
    return future.get();
}

boost::system::error_code write_break(transport& port, const line_break& item)
{
    auto promise =
        std::make_shared<std::promise<boost::system::error_code>>();
    port.async_write_break(item,
                           [promise](boost::system::error_code ec, std::size_t)
                           { promise->set_value(ec); });
    auto future = promise->get_future();
    REQUIRE(future.wait_for(3000ms) == std::future_status::ready);
    return future.get();
}

/// What the transport has told the log, and the breaks it has reported.
struct recorder
{
    std::mutex mutex;
    std::vector<std::string> notices;
    std::vector<line_break> breaks;

    void attach(transport& port)
    {
        port.set_notice_sink(
            [this](std::string text)
            {
                std::lock_guard lock(mutex);
                notices.push_back(std::move(text));
            });
        port.set_break_sink(
            [this](const line_break& item)
            {
                std::lock_guard lock(mutex);
                breaks.push_back(item);
            });
    }

    std::size_t notice_count()
    {
        std::lock_guard lock(mutex);
        return notices.size();
    }

    std::size_t break_count()
    {
        std::lock_guard lock(mutex);
        return breaks.size();
    }
};

bool wait_for_count(const std::function<std::size_t()>& count,
                    std::size_t wanted)
{
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        if (count() >= wanted)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

// -- the protocol ------------------------------------------------------------

TEST_CASE("rfc2217 client: the greeting offers COM-PORT, BINARY and SGA")
{
    rfc2217::client_session session;
    REQUIRE_EQ(session.greeting(),
               command(will, 44) + command(do_, 44) + command(will, 0) +
                   command(do_, 0) + command(will, 3) + command(do_, 3));
}

TEST_CASE("rfc2217 client: the server's answers make COM-PORT active and "
          "are not answered again")
{
    rfc2217::client_session session;
    (void)session.greeting();
    REQUIRE(!session.com_port_active());

    const auto fed = session.feed(command(do_, 44) + command(will, 44) +
                                  command(do_, 0) + command(will, 0));
    REQUIRE(fed.reply.empty());
    REQUIRE(fed.com_port_changed);
    REQUIRE(session.com_port_active());
    REQUIRE(!session.com_port_refused());
}

TEST_CASE("rfc2217 client: a server that wants an option we did not offer is "
          "refused it, and one that refuses COM-PORT is noticed")
{
    rfc2217::client_session session;
    (void)session.greeting();
    const auto fed = session.feed(command(do_, 1) + command(will, 1));
    REQUIRE_EQ(fed.reply, command(wont, 1) + command(dont, 1));

    const auto refused = session.feed(command(dont, 44) + command(wont, 44));
    REQUIRE(refused.com_port_changed);
    REQUIRE(session.com_port_refused());
    REQUIRE(!session.com_port_active());
}

TEST_CASE("rfc2217 client: the requests are the bytes of RFC 2217")
{
    rfc2217::client_session session;
    const std::string head{iac, sb, 44};
    const std::string tail{iac, se};
    REQUIRE_EQ(session.set_baud_rate(115200),
               head + std::string("\x01\x00\x01\xC2\x00", 5) + tail);
    REQUIRE_EQ(session.set_data_size(7), head + std::string("\x02\x07", 2) + tail);
    REQUIRE_EQ(session.set_parity(parity_mode::even),
               head + std::string("\x03\x03", 2) + tail);
    REQUIRE_EQ(session.set_stop_size(stop_bits_mode::two),
               head + std::string("\x04\x02", 2) + tail);
    REQUIRE_EQ(session.set_dtr(true), head + std::string("\x05\x08", 2) + tail);
    REQUIRE_EQ(session.set_dtr(false), head + std::string("\x05\x09", 2) + tail);
    REQUIRE_EQ(session.set_rts(true), head + std::string("\x05\x0B", 2) + tail);
    REQUIRE_EQ(session.set_rts(false), head + std::string("\x05\x0C", 2) + tail);
    REQUIRE_EQ(session.set_break(true), head + std::string("\x05\x05", 2) + tail);
    REQUIRE_EQ(session.set_break(false), head + std::string("\x05\x06", 2) + tail);
    REQUIRE_EQ(session.set_line_state_mask(0x1E),
               head + std::string("\x0A\x1E", 2) + tail);
    const std::string doubled(2, iac);
    REQUIRE_EQ(session.set_modem_state_mask(0xFF),
               head + std::string("\x0B", 1) + doubled + tail);
}

TEST_CASE("rfc2217 client: a rate with a 0xFF byte is escaped, and a server "
          "can read it back")
{
    rfc2217::client_session client;
    rfc2217::session server;
    (void)server.greeting();
    const auto fed = server.feed(client.set_baud_rate(0x0001FFFF));
    REQUIRE_EQ(server.settings().baud_rate, 0x0001FFFFU);
    REQUIRE(!fed.reply.empty());
}

TEST_CASE("rfc2217 client: line and modem state notices are read, with the "
          "place of a break among the bytes")
{
    rfc2217::client_session session;
    const auto fed = session.feed(
        std::string("ab") + notice(notify_linestate, std::string("\x10", 1)) +
        "cd" + notice(notify_modemstate, std::string("\x30", 1)));
    REQUIRE_EQ(fed.data, std::string("abcd"));
    REQUIRE_EQ(fed.line_states.size(), std::size_t{1});
    REQUIRE_EQ(fed.line_states[0].at, std::size_t{2});
    REQUIRE_EQ(static_cast<int>(fed.line_states[0].state), 0x10);
    REQUIRE(fed.modem_state.has_value());
    REQUIRE_EQ(static_cast<int>(*fed.modem_state), 0x30);
}

TEST_CASE("rfc2217 client: data with doubled 0xFF and a split command")
{
    rfc2217::client_session session;
    const std::string first = std::string("a") + iac;
    const std::string second = std::string{iac} + "b";
    auto fed = session.feed(first);
    REQUIRE_EQ(fed.data, std::string("a"));
    fed = session.feed(second);
    const std::string expected = std::string(1, iac) + "b";
    REQUIRE_EQ(fed.data, expected);
}

// -- the transport -----------------------------------------------------------

TEST_CASE("rfc2217 client: a closed port is a connect error")
{
    loop running;
    std::uint16_t port = 0;
    {
        server gone;
        port = gone.port();
    }
    error_code ec;
    rfc2217_client_transport port_client(
        running.io.get_executor(),
        "rfc2217://127.0.0.1:" + std::to_string(port), ec);
    REQUIRE(ec);
}

TEST_CASE("rfc2217 client: a URI of another kind or a bad setting is refused")
{
    loop running;
    error_code ec;
    rfc2217_client_transport other(running.io.get_executor(),
                                   "tcp://127.0.0.1:1", ec);
    REQUIRE(ec);
    ec.clear();
    rfc2217_client_transport bad(running.io.get_executor(),
                                 "rfc2217://127.0.0.1:1?baud=fast", ec);
    REQUIRE(ec);
}

TEST_CASE("rfc2217 client: the settings of the URI go to the server once it "
          "agrees to COM-PORT, and not before")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(
        running.io.get_executor(),
        remote.uri("?baud=19200&data=7&parity=even&stop=2"), ec);
    REQUIRE(!ec);
    remote.accept();

    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.greeting()));
    REQUIRE(remote.got.find(expect.set_baud_rate(19200)) == std::string::npos);

    remote.agree();
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    REQUIRE(remote.got.find(expect.set_baud_rate(19200)) != std::string::npos);
    REQUIRE(remote.got.find(expect.set_data_size(7)) != std::string::npos);
    REQUIRE(remote.got.find(expect.set_parity(parity_mode::even)) !=
            std::string::npos);
    REQUIRE(remote.got.find(expect.set_stop_size(stop_bits_mode::two)) !=
            std::string::npos);
    REQUIRE(remote.got.find(expect.set_dtr(true)) != std::string::npos);
    REQUIRE(remote.got.find(expect.set_rts(true)) != std::string::npos);
    REQUIRE(remote.got.find(expect.set_line_state_mask(0x1E)) !=
            std::string::npos);
}

TEST_CASE("rfc2217 client: a new format is sent once, and only what changed")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(),
                                    remote.uri("?baud=9600"), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    remote.got.clear();

    line_format format;
    format.baud_rate = 250000;
    format.data_bits = 8;
    client.apply_peer_format(format);
    REQUIRE(remote.wait_for(expect.set_baud_rate(250000)));
    // Nothing else changed, so nothing else was sent.
    std::this_thread::sleep_for(50ms);
    remote.wait_for("never", 10ms);
    REQUIRE_EQ(remote.got, expect.set_baud_rate(250000));
}

TEST_CASE("rfc2217 client: DTR and RTS reach the server, and the modem lines "
          "come back as inputs")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    remote.got.clear();

    client.set_control_line(control_line::dtr, false);
    REQUIRE(remote.wait_for(expect.set_dtr(false)));
    client.set_control_line(control_line::rts, false);
    REQUIRE(remote.wait_for(expect.set_rts(false)));
    REQUIRE(!client.lines().dtr());
    REQUIRE(!client.lines().rts());

    // CTS and DCD up, with their change bits.
    remote.send(notice(notify_modemstate, std::string("\x99", 1)));
    const auto got = read_from(client, 200ms);
    // A read with no bytes says that only the lines changed.
    REQUIRE(!got.error || got.error == asio::error::timed_out);
    REQUIRE(wait_for_count([&] { return client.lines().cts() ? 1U : 0U; }, 1));
    REQUIRE(client.lines().dcd());
    REQUIRE(!client.lines().dsr());
}

TEST_CASE("rfc2217 client: bytes both ways, and 0xFF is doubled")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();

    const std::string payload = std::string("x") + iac + "y";
    REQUIRE(!write_to(client, payload));
    REQUIRE(remote.wait_for(std::string("x") + iac + iac + "y"));

    remote.send(std::string("hi") + iac + iac + "!");
    std::string seen;
    for (int attempt = 0; attempt < 20 && seen.size() < 4; ++attempt)
    {
        const auto got = read_from(client);
        REQUIRE(!got.error);
        seen += got.text;
    }
    REQUIRE_EQ(seen, std::string("hi") + iac + "!");
}

TEST_CASE("rfc2217 client: a line-state break comes as one unmeasured break "
          "between the bytes before it and after it")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(),
                                    remote.uri("?baud=1200"), ec);
    REQUIRE(!ec);
    recorder seen;
    seen.attach(client);
    remote.accept();
    remote.agree();

    remote.send(std::string("ab") +
             notice(notify_linestate, std::string("\x10", 1)) + "cd");

    // The read stops at the break: "ab", then the break, then "cd".
    const auto first = read_from(client);
    REQUIRE(!first.error);
    REQUIRE_EQ(first.text, std::string("ab"));
    REQUIRE(wait_for_count([&] { return seen.break_count(); }, 1));
    const auto second = read_from(client);
    REQUIRE_EQ(second.text, std::string("cd"));

    std::lock_guard lock(seen.mutex);
    REQUIRE_EQ(seen.breaks.size(), std::size_t{1});
    REQUIRE(seen.breaks[0].phase == break_phase::complete);
    REQUIRE(!seen.breaks[0].measured);
    // One character time at 1200 baud, 8N1: ten bits, 8.3 ms.
    REQUIRE(seen.breaks[0].duration > 8ms);
    REQUIRE(seen.breaks[0].duration < 9ms);
}

TEST_CASE("rfc2217 client: a framing notice is a fault of the line, in order "
          "with the bytes, and the read carries on after it")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();

    remote.send(std::string("ab") +
                notice(notify_linestate, std::string("", 1)) + "cd");
    const auto first = read_from(client);
    REQUIRE_EQ(first.text, std::string("ab"));
    const auto fault = read_from(client);
    REQUIRE(fault.error);
    REQUIRE(client.is_line_error(fault.error));
    REQUIRE(fault.text.empty());
    const auto last = read_from(client);
    REQUIRE(!last.error);
    REQUIRE_EQ(last.text, std::string("cd"));
}

TEST_CASE("rfc2217 client: a break at the start of the stream, and two in a "
          "row, are each reported")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    recorder seen;
    seen.attach(client);
    remote.accept();
    remote.agree();

    const std::string mark = notice(notify_linestate, std::string("\x10", 1));
    remote.send(mark + mark + "z");
    REQUIRE(read_from(client).text.empty());
    const auto last = read_from(client);
    REQUIRE_EQ(last.text, std::string("z"));
    REQUIRE(wait_for_count([&] { return seen.break_count(); }, 2));
}

TEST_CASE("rfc2217 client: a break item is break on, a wait of its length, "
          "break off, and the log says the length is the network's")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    recorder seen;
    seen.attach(client);
    remote.accept();
    remote.agree();
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    remote.got.clear();

    line_break item;
    item.phase = break_phase::complete;
    item.start = clock_type::now();
    item.duration = 60ms;
    item.measured = true;

    const auto began = std::chrono::steady_clock::now();
    REQUIRE(!write_break(client, item));
    const auto took = std::chrono::steady_clock::now() - began;
    REQUIRE(took >= 55ms);

    REQUIRE(remote.wait_for(expect.set_break(true) + expect.set_break(false)));
    REQUIRE_EQ(remote.got, expect.set_break(true) + expect.set_break(false));

    REQUIRE_EQ(seen.notice_count(), std::size_t{1});
    {
        std::lock_guard lock(seen.mutex);
        REQUIRE(seen.notices[0].find("accurate to the network") !=
                std::string::npos);
    }

    // The log is told once for the connection, not once per break.
    REQUIRE(!write_break(client, item));
    REQUIRE_EQ(seen.notice_count(), std::size_t{1});
}

TEST_CASE("rfc2217 client: the halves of a break are break on and break off, "
          "and an unmeasured break waits one character time")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(),
                                    remote.uri("?baud=300"), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    remote.got.clear();

    line_break begin;
    begin.phase = break_phase::begin;
    begin.start = clock_type::now();
    REQUIRE(!write_break(client, begin));
    REQUIRE(remote.wait_for(expect.set_break(true)));
    REQUIRE_EQ(remote.got, expect.set_break(true));

    line_break end;
    end.phase = break_phase::end;
    end.start = begin.start;
    end.duration = 5ms;
    REQUIRE(!write_break(client, end));
    REQUIRE(remote.wait_for(expect.set_break(true) + expect.set_break(false)));

    // At 300 baud a character is 33 ms, and that is the least a break waits.
    remote.got.clear();
    line_break quick;
    quick.phase = break_phase::complete;
    quick.start = clock_type::now();
    quick.duration = 1ms;
    quick.measured = false;
    const auto began = std::chrono::steady_clock::now();
    REQUIRE(!write_break(client, quick));
    REQUIRE(std::chrono::steady_clock::now() - began >= 30ms);
}

TEST_CASE("rfc2217 client: a server that refuses COM-PORT is a byte stream, "
          "the log says so, and a break is refused")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    recorder seen;
    seen.attach(client);
    remote.accept();
    remote.send(command(dont, 44) + command(wont, 44));
    REQUIRE(wait_for_count([&] { return seen.notice_count(); }, 1));

    REQUIRE(!write_to(client, "plain"));
    REQUIRE(remote.wait_for("plain"));
    remote.send("echo");
    REQUIRE_EQ(read_from(client).text, std::string("echo"));

    line_break item;
    item.phase = break_phase::complete;
    item.duration = 1ms;
    REQUIRE(write_break(client, item));
    REQUIRE(remote.got.find(std::string{iac, sb, 44, 5, 5}) == std::string::npos);
}

TEST_CASE("rfc2217 client: a lost connection ends the read after the bytes "
          "that came before it, and the transport is closed")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();
    // Read what the client sent, so that closing is an orderly end and not a
    // reset that throws away what is on its way.
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));
    remote.send("bye");
    remote.socket.close();

    std::string seen;
    boost::system::error_code last;
    for (int attempt = 0; attempt < 10 && !last; ++attempt)
    {
        const auto got = read_from(client);
        seen += got.text;
        last = got.error;
    }
    REQUIRE_EQ(seen, std::string("bye"));
    REQUIRE(last);
    REQUIRE(last != asio::error::timed_out);
    REQUIRE(wait_for_count([&] { return client.is_open() ? 0U : 1U; }, 1));
    REQUIRE(write_to(client, "late"));
}

TEST_CASE("rfc2217 client: close completes a waiting read and a break wait")
{
    loop running;
    server remote;
    error_code ec;
    rfc2217_client_transport client(running.io.get_executor(), remote.uri(), ec);
    REQUIRE(!ec);
    remote.accept();
    remote.agree();
    rfc2217::client_session expect;
    REQUIRE(remote.wait_for(expect.set_modem_state_mask(0xFF)));

    auto reading = std::make_shared<std::promise<boost::system::error_code>>();
    auto buffer = std::make_shared<std::array<char, 16>>();
    client.async_read_some(
        asio::buffer(*buffer),
        [reading, buffer](boost::system::error_code e, std::size_t)
        { reading->set_value(e); });

    auto breaking = std::make_shared<std::promise<boost::system::error_code>>();
    line_break item;
    item.phase = break_phase::complete;
    item.duration = 5s;
    item.measured = true;
    client.async_write_break(
        item, [breaking](boost::system::error_code e, std::size_t)
        { breaking->set_value(e); });
    std::this_thread::sleep_for(50ms);

    client.close();
    REQUIRE(reading->get_future().wait_for(3000ms) ==
            std::future_status::ready);
    REQUIRE(breaking->get_future().wait_for(3000ms) ==
            std::future_status::ready);
    REQUIRE(!client.is_open());
}

TEST_CASE("rfc2217 client: an address is told apart from the others")
{
    REQUIRE(is_rfc2217_client_uri("rfc2217://lab:7001"));
    REQUIRE(!is_rfc2217_client_uri("rfc2217-listen://0.0.0.0:7001"));
    REQUIRE(!is_rfc2217_client_uri("tcp://lab:7001"));
}

TEST_CASE("rfc2217 client: the request forms ask and the answers are read")
{
    rfc2217::client_session session;
    const std::string head{iac, sb, 44};
    const std::string tail{iac, se};
    REQUIRE_EQ(session.ask_baud_rate(),
               head + std::string("\x01\x00\x00\x00\x00", 5) + tail);
    REQUIRE_EQ(session.ask_data_size(), head + std::string("\x02\x00", 2) + tail);
    REQUIRE_EQ(session.ask_parity(), head + std::string("\x03\x00", 2) + tail);
    REQUIRE_EQ(session.ask_stop_size(), head + std::string("\x04\x00", 2) + tail);
    REQUIRE_EQ(session.ask_flow_control(),
               head + std::string("\x05\x00", 2) + tail);

    const auto fed = session.feed(
        head + std::string("\x65\x00\x00\x25\x80", 5) + tail +
        head + std::string("\x66\x07", 2) + tail +
        head + std::string("\x67\x03", 2) + tail +
        head + std::string("\x68\x02", 2) + tail);
    REQUIRE(fed.answers.baud_rate.has_value());
    REQUIRE_EQ(*fed.answers.baud_rate, 9600U);
    REQUIRE_EQ(*fed.answers.data_bits, 7U);
    REQUIRE(*fed.answers.parity == parity_mode::even);
    REQUIRE(*fed.answers.stop_bits == stop_bits_mode::two);

    // A zero or an unknown code says nothing.
    const auto none = session.feed(
        head + std::string("\x65\x00\x00\x00\x00", 5) + tail +
        head + std::string("\x67\x09", 2) + tail);
    REQUIRE(!none.answers.baud_rate.has_value());
    REQUIRE(!none.answers.parity.has_value());
}
