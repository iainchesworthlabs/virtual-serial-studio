//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/listen_transport.hpp>
#include <vss/ports/rfc2217.hpp>
#include <vss/ports/serial_transport.hpp>

#include "test_framework.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <initializer_list>
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

constexpr unsigned char iac = 255;
constexpr unsigned char do_option = 253;
constexpr unsigned char will = 251;
constexpr unsigned char sb = 250;
constexpr unsigned char se = 240;
constexpr unsigned char com_port = 44;

std::string bytes(std::initializer_list<unsigned int> values)
{
    std::string out;
    for (const unsigned int value : values)
        out.push_back(static_cast<char>(value));
    return out;
}

/// IAC SB COM-PORT <code> <value> IAC SE
std::string command(unsigned char code, const std::string& value)
{
    return bytes({iac, sb, com_port, code}) + value + bytes({iac, se});
}

/// A session that has finished the opening negotiation.
rfc2217::session ready_session()
{
    rfc2217::session session;
    static_cast<void>(session.greeting());
    static_cast<void>(session.feed(bytes({iac, do_option, com_port})));
    return session;
}

struct loop
{
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{
        io.get_executor()};
    std::thread thread{[this] { io.run(); }};

    ~loop()
    {
        guard.reset();
        try
        {
            io.stop();
        }
        catch (...)
        {
            // best effort: a destructor must not throw, and nothing can act on a failed stop
        }
        thread.join();
    }
};

struct read_result
{
    boost::system::error_code error;
    std::string text;
};

read_result read_from(listen_transport& transport,
                      std::chrono::milliseconds wait = 3000ms)
{
    auto promise = std::make_shared<std::promise<read_result>>();
    auto buffer = std::make_shared<std::array<char, 256>>();
    transport.async_read_some(
        asio::buffer(*buffer),
        [promise, buffer](boost::system::error_code ec, std::size_t count)
        { promise->set_value({ec, std::string(buffer->data(), count)}); });
    auto future = promise->get_future();
    if (future.wait_for(wait) != std::future_status::ready)
    {
        transport.cancel();
        future.wait();
        return {asio::error::timed_out, {}};
    }
    return future.get();
}

/// Reads from the client socket until `count` bytes have arrived, or three
/// seconds pass (then the text is shorter and the comparison fails).
std::string receive(asio::ip::tcp::socket& socket, std::size_t count)
{
    socket.non_blocking(true);
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (text.size() < count && std::chrono::steady_clock::now() < deadline)
    {
        std::array<char, 256> chunk{};
        boost::system::error_code ec;
        const std::size_t got = socket.read_some(
            asio::buffer(chunk.data(), std::min(chunk.size(), count - text.size())),
            ec);
        if (ec == asio::error::would_block)
        {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (ec)
            break;
        text.append(chunk.data(), got);
    }
    return text;
}

} // namespace

TEST_CASE("rfc2217: the greeting offers the options and an answer is not echoed")
{
    rfc2217::session session;
    const std::string greeting = session.greeting();
    REQUIRE_EQ(greeting,
               bytes({iac, will, com_port, iac, will, 0, iac, do_option, 0, iac,
                      will, 3, iac, do_option, 3}));

    // The client agrees to what was offered; nothing more is said.
    const auto agreed = session.feed(bytes({iac, do_option, com_port, iac, do_option, 0,
                                            iac, will, 0, iac, do_option, 3, iac,
                                            will, 3}));
    REQUIRE(agreed.reply.empty());
    REQUIRE(agreed.data.empty());
    REQUIRE(session.com_port_active());

    // An option the server does not know is refused, once.
    const auto refused = session.feed(bytes({iac, do_option, 24}));
    REQUIRE_EQ(refused.reply, bytes({iac, 252, 24}));
    const auto offered = session.feed(bytes({iac, will, 31}));
    REQUIRE_EQ(offered.reply, bytes({iac, 254, 31}));
}

TEST_CASE("rfc2217: a client that speaks first is answered")
{
    rfc2217::session session;
    const auto opened = session.feed(bytes({iac, will, com_port}));
    REQUIRE_EQ(opened.reply, bytes({iac, do_option, com_port}));
    REQUIRE(session.com_port_active());
}

TEST_CASE("rfc2217: data passes through with the commands taken out")
{
    rfc2217::session session = ready_session();

    const auto plain = session.feed("abc");
    REQUIRE_EQ(plain.data, std::string("abc"));

    // A doubled 0xFF is one data byte, and it may arrive in separate chunks.
    const auto first = session.feed(bytes({'a', iac}));
    REQUIRE_EQ(first.data, std::string("a"));
    const auto second = session.feed(bytes({iac, 'b'}));
    REQUIRE_EQ(second.data, bytes({iac, 'b'}));

    // Commands in the middle of data are removed.
    const auto mixed =
        session.feed("x" + command(2, bytes({7})) + "y" + bytes({iac, 241}));
    REQUIRE_EQ(mixed.data, std::string("xy"));

    REQUIRE_EQ(rfc2217::session::encode(bytes({'a', iac, 'b', iac, iac})),
               bytes({'a', iac, iac, 'b', iac, iac, iac, iac}));
}

TEST_CASE("rfc2217: baud rate, framing and flow control are accepted")
{
    rfc2217::session session = ready_session();
    REQUIRE_EQ(session.settings().baud_rate, 9600U);

    auto result = session.feed(command(1, bytes({0, 1, 0xC2, 0})));
    REQUIRE_EQ(session.settings().baud_rate, 115200U);
    REQUIRE_EQ(result.reply, command(101, bytes({0, 1, 0xC2, 0})));

    // Zero asks for the current rate and changes nothing.
    result = session.feed(command(1, bytes({0, 0, 0, 0})));
    REQUIRE_EQ(session.settings().baud_rate, 115200U);
    REQUIRE_EQ(result.reply, command(101, bytes({0, 1, 0xC2, 0})));

    session.feed(command(2, bytes({7})));
    session.feed(command(3, bytes({3})));
    session.feed(command(4, bytes({2})));
    session.feed(command(5, bytes({3})));
    REQUIRE_EQ(session.settings().data_bits, 7U);
    REQUIRE(session.settings().parity == parity_mode::even);
    REQUIRE(session.settings().stop_bits == stop_bits_mode::two);
    REQUIRE(session.settings().flow_control == flow_control_mode::hardware);

    // A value out of range is refused by answering with the one in force.
    result = session.feed(command(2, bytes({9})));
    REQUIRE_EQ(session.settings().data_bits, 7U);
    REQUIRE_EQ(result.reply, command(102, bytes({7})));
    result = session.feed(command(3, bytes({0})));
    REQUIRE_EQ(result.reply, command(103, bytes({3})));

    // A 0xFF in a value is doubled in the answer.
    result = session.feed(command(1, bytes({0, 0, 0, iac, iac})));
    REQUIRE_EQ(session.settings().baud_rate, 255U);
    REQUIRE_EQ(result.reply, command(101, bytes({0, 0, 0, iac, iac})));
}

TEST_CASE("rfc2217: DTR, RTS and break are the outputs the client drives")
{
    rfc2217::session session = ready_session();
    REQUIRE_EQ(session.outputs().bits, 0U);

    auto result = session.feed(command(5, bytes({8})));
    REQUIRE(result.outputs_changed);
    REQUIRE_EQ(result.reply, command(105, bytes({8})));
    REQUIRE(session.outputs().dtr());

    result = session.feed(command(5, bytes({11})));
    REQUIRE(result.outputs_changed);
    REQUIRE(session.outputs().rts());
    result = session.feed(command(5, bytes({5})));
    REQUIRE(session.outputs().break_active());

    // Asking is not changing.
    result = session.feed(command(5, bytes({7})));
    REQUIRE(!result.outputs_changed);
    REQUIRE_EQ(result.reply, command(105, bytes({8})));
    result = session.feed(command(5, bytes({10})));
    REQUIRE_EQ(result.reply, command(105, bytes({11})));
    result = session.feed(command(5, bytes({4})));
    REQUIRE_EQ(result.reply, command(105, bytes({5})));

    result = session.feed(command(5, bytes({9})));
    REQUIRE(!session.outputs().dtr());
    result = session.feed(command(5, bytes({12})));
    session.feed(command(5, bytes({6})));
    REQUIRE_EQ(session.outputs().bits, 0U);
}

TEST_CASE("rfc2217: modem state is sent for the lines the client watches")
{
    rfc2217::session session = ready_session();

    control_lines inputs;
    inputs.set(control_line::cts, true);
    // The default mask watches every line, so a change is reported with its
    // state and its delta bits.
    REQUIRE_EQ(session.modem_state(inputs),
               command(107, bytes({0x10 | 0x01})));
    // The same state again is silent.
    REQUIRE(session.modem_state(inputs).empty());

    inputs.set(control_line::dsr, true);
    inputs.set(control_line::dcd, true);
    REQUIRE_EQ(session.modem_state(inputs),
               command(107, bytes({0x10 | 0x20 | 0x80 | 0x02 | 0x08})));

    // RI is reported on its trailing edge only.
    inputs.set(control_line::ri, true);
    REQUIRE(session.modem_state(inputs).empty());
    inputs.set(control_line::ri, false);
    const std::string trailing = session.modem_state(inputs);
    REQUIRE_EQ(trailing, command(107, bytes({0x10 | 0x20 | 0x80 | 0x04})));

    // A mask of CTS alone: a DSR change stays quiet, a CTS change does not.
    const auto masked = session.feed(command(11, bytes({0x10})));
    REQUIRE(masked.modem_state_requested);
    REQUIRE_EQ(masked.reply, command(111, bytes({0x10})));
    inputs.set(control_line::dsr, false);
    REQUIRE(session.modem_state(inputs).empty());
    inputs.set(control_line::cts, false);
    REQUIRE(!session.modem_state(inputs).empty());

    // A forced notification reports the state whatever changed.
    REQUIRE_EQ(session.modem_state(inputs, true),
               command(107, bytes({0x80})));
}

TEST_CASE("rfc2217: signature, purge and masks are answered")
{
    rfc2217::session session = ready_session();
    const auto signature = session.feed(command(0, {}));
    REQUIRE_EQ(signature.reply, command(100, "Virtual Serial Studio"));

    REQUIRE_EQ(session.feed(command(12, bytes({3}))).reply,
               command(112, bytes({3})));
    REQUIRE_EQ(session.feed(command(10, bytes({0x0F}))).reply,
               command(110, bytes({0x0F})));

    // Malformed input is ignored: too short, wrong option, and an unterminated
    // subnegotiation that is then interrupted.
    REQUIRE(session.feed(bytes({iac, sb, com_port, iac, se})).reply.empty());
    REQUIRE(session.feed(bytes({iac, sb, 99, 1, iac, se})).reply.empty());
    const auto after = session.feed("ok");
    REQUIRE_EQ(after.data, std::string("ok"));
}

TEST_CASE("rfc2217: the listen URI names the role")
{
    const auto parsed = parse_listen_uri("rfc2217-listen://127.0.0.1:2217");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->protocol == listen_protocol::rfc2217);
    REQUIRE_EQ(make_listen_uri(*parsed),
               std::string("rfc2217-listen://127.0.0.1:2217"));
    REQUIRE(is_listen_uri("rfc2217-listen://x"));
    REQUIRE(!is_listen_uri("rfc2217://127.0.0.1:2217"));
    const auto kind = classify_uri("rfc2217-listen://127.0.0.1:2217");
    REQUIRE(kind.has_value());
    REQUIRE(*kind == endpoint_kind::rfc2217);
}

TEST_CASE("rfc2217: a client opens the endpoint like a serial port")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::rfc2217;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);
    REQUIRE(transport.supports_control_lines());

    asio::io_context client_io;
    asio::ip::tcp::socket client(client_io);
    client.connect({asio::ip::make_address("127.0.0.1"),
                    transport.local_port()});

    // The server speaks first.
    rfc2217::session reference;
    const std::string greeting = reference.greeting();
    REQUIRE_EQ(receive(client, greeting.size()), greeting);

    // Agree, set the framing, and send data that holds a 0xFF byte.
    asio::write(client,
                asio::buffer(bytes({iac, do_option, com_port}) +
                             command(1, bytes({0, 0, 0x25, 0x80})) +
                             std::string("a") + bytes({iac, iac}) + "b"));
    const std::string baud_answer = command(101, bytes({0, 0, 0x25, 0x80}));
    REQUIRE_EQ(receive(client, baud_answer.size()), baud_answer);
    const auto got = read_from(transport);
    REQUIRE(!got.error);
    REQUIRE_EQ(got.text, bytes({'a', iac, 'b'}));

    // What the hub writes has its 0xFF doubled.
    auto promise = std::make_shared<std::promise<std::size_t>>();
    const std::string out = bytes({'x', iac, 'y'});
    transport.async_write_some(
        asio::buffer(out),
        [promise](boost::system::error_code, std::size_t count)
        { promise->set_value(count); });
    auto written = promise->get_future();
    REQUIRE(written.wait_for(3000ms) == std::future_status::ready);
    REQUIRE_EQ(written.get(), std::size_t{3});
    REQUIRE_EQ(receive(client, 4), bytes({'x', iac, iac, 'y'}));

    // The client sets DTR; the endpoint learns of it through a read of no
    // bytes and sees the line.
    asio::write(client, asio::buffer(command(5, bytes({8}))));
    const std::string dtr_answer = command(105, bytes({8}));
    REQUIRE_EQ(receive(client, dtr_answer.size()), dtr_answer);
    const auto lines_only = read_from(transport);
    REQUIRE(!lines_only.error);
    REQUIRE(lines_only.text.empty());
    REQUIRE(transport.lines().dtr());
    REQUIRE(!transport.lines().rts());

    // A peer line that reaches the endpoint is sent to the client as modem
    // state once the client asks for it.
    asio::write(client, asio::buffer(command(11, bytes({iac, iac}))));
    const std::string mask_answer = command(111, bytes({iac, iac})) +
                                    command(107, bytes({0x00}));
    REQUIRE_EQ(receive(client, mask_answer.size()), mask_answer);
    transport.set_control_line(control_line::cts, true);
    const std::string notified = command(107, bytes({0x10 | 0x01}));
    REQUIRE_EQ(receive(client, notified.size()), notified);
    REQUIRE(transport.lines().cts());
}

TEST_CASE("rfc2217: a plain TCP listener has no control lines")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::tcp;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);
    REQUIRE(!transport.supports_control_lines());
    transport.set_control_line(control_line::cts, true);
    REQUIRE_EQ(transport.lines().bits, 0U);
}

TEST_CASE("rfc2217: every parity, stop size and flow control value is mapped")
{
    rfc2217::session session = ready_session();

    const struct
    {
        unsigned code = 0;
        parity_mode parity{};
    } parities[] = {{1, parity_mode::none}, {2, parity_mode::odd},
                    {3, parity_mode::even}, {4, parity_mode::mark},
                    {5, parity_mode::space}};
    for (const auto& entry : parities)
    {
        const auto result = session.feed(command(3, bytes({entry.code})));
        REQUIRE(session.settings().parity == entry.parity);
        REQUIRE_EQ(result.reply, command(103, bytes({entry.code})));
    }

    const struct
    {
        unsigned code = 0;
        stop_bits_mode stop{};
    } stops[] = {{1, stop_bits_mode::one},
                 {2, stop_bits_mode::two},
                 {3, stop_bits_mode::one_point_five}};
    for (const auto& entry : stops)
    {
        const auto result = session.feed(command(4, bytes({entry.code})));
        REQUIRE(session.settings().stop_bits == entry.stop);
        REQUIRE_EQ(result.reply, command(104, bytes({entry.code})));
    }
    // An unknown value changes nothing and reports the one in force.
    REQUIRE_EQ(session.feed(command(4, bytes({9}))).reply,
               command(104, bytes({3})));

    const struct
    {
        unsigned code = 0;
        flow_control_mode flow{};
    } flows[] = {{1, flow_control_mode::none},
                 {2, flow_control_mode::software},
                 {3, flow_control_mode::hardware}};
    for (const auto& entry : flows)
    {
        const auto result = session.feed(command(5, bytes({entry.code})));
        REQUIRE(session.settings().flow_control == entry.flow);
        REQUIRE_EQ(result.reply, command(105, bytes({entry.code})));
        REQUIRE(!result.outputs_changed);
        // Asking for the setting reports the one in force.
        REQUIRE_EQ(session.feed(command(5, bytes({0}))).reply,
                   command(105, bytes({entry.code})));
    }

    // Inbound flow control is not supported: asking says none, and the other
    // requests are acknowledged as they were sent.
    REQUIRE_EQ(session.feed(command(5, bytes({13}))).reply,
               command(105, bytes({14})));
    REQUIRE_EQ(session.feed(command(5, bytes({15}))).reply,
               command(105, bytes({15})));
    REQUIRE_EQ(session.feed(command(5, bytes({19}))).reply,
               command(105, bytes({19})));

    // An unknown command is ignored, and so is flow control suspend.
    REQUIRE(session.feed(command(77, {})).reply.empty());
    REQUIRE(session.feed(command(8, {})).reply.empty());
    REQUIRE(session.feed(command(9, {})).reply.empty());
}

TEST_CASE("rfc2217: negotiation can be refused and withdrawn")
{
    rfc2217::session session = ready_session();

    // Binary was offered and the client has not agreed yet; refusing it is
    // silent.
    REQUIRE(session.feed(bytes({iac, 254, 0})).reply.empty());
    // Agreeing now is answered with the offer, and withdrawing it is answered.
    REQUIRE_EQ(session.feed(bytes({iac, do_option, 0})).reply,
               bytes({iac, will, 0}));
    REQUIRE_EQ(session.feed(bytes({iac, 254, 0})).reply,
               bytes({iac, 252, 0}));
    REQUIRE(session.feed(bytes({iac, 254, 0})).reply.empty());

    // The client stops doing something it was not doing, then does and stops.
    REQUIRE(session.feed(bytes({iac, 252, 3})).reply.empty());
    REQUIRE_EQ(session.feed(bytes({iac, will, 3})).reply,
               bytes({iac, do_option, 3}));
    REQUIRE_EQ(session.feed(bytes({iac, 252, 3})).reply,
               bytes({iac, 254, 3}));

    // An option the server does not know is never agreed to.
    REQUIRE(session.feed(bytes({iac, 252, 99})).reply.empty());
    REQUIRE(session.feed(bytes({iac, 254, 99})).reply.empty());

    // Asking again for what is already on is silent.
    REQUIRE(session.feed(bytes({iac, do_option, com_port})).reply.empty());
    REQUIRE_EQ(session.feed(bytes({iac, will, com_port})).reply,
               bytes({iac, do_option, com_port}));
    REQUIRE(session.feed(bytes({iac, will, com_port})).reply.empty());
}

TEST_CASE("rfc2217: damaged subnegotiations are dropped without harm")
{
    rfc2217::session session = ready_session();

    // A subnegotiation that is too long is cut, not stored without limit.
    std::string huge = bytes({iac, sb, com_port, 1});
    huge.append(1000, 'z');
    huge += bytes({iac, se});
    const auto overlong = session.feed(huge + "tail");
    REQUIRE_EQ(overlong.data, std::string("tail"));
    REQUIRE_EQ(session.settings().baud_rate, 9600U);

    // A command that interrupts a subnegotiation ends it and is not an SE.
    const auto interrupted =
        session.feed(bytes({iac, sb, com_port, 2, 7, iac, 241}) + "after");
    REQUIRE(interrupted.reply.empty());
    REQUIRE_EQ(interrupted.data, std::string("after"));

    // Doubled IAC bytes keep a long subnegotiation inside its limit too.
    std::string doubled = bytes({iac, sb, com_port, 1});
    for (int index = 0; index < 300; ++index)
        doubled += bytes({iac, iac});
    doubled += bytes({iac, se});
    session.feed(doubled);
    REQUIRE_EQ(session.settings().baud_rate, 9600U);
}

TEST_CASE("rfc2217: a mask of zero keeps the modem state quiet")
{
    rfc2217::session session = ready_session();
    session.feed(command(11, bytes({0})));

    control_lines inputs;
    inputs.set(control_line::cts, true);
    inputs.set(control_line::dcd, true);
    REQUIRE(session.modem_state(inputs).empty());
    // A forced notification still reports the state.
    REQUIRE_EQ(session.modem_state(inputs, true),
               command(107, bytes({0x10 | 0x80})));
}

TEST_CASE("rfc2217: a client that never negotiates still gets its data")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::rfc2217;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);
    REQUIRE(transport.describe().find("RFC 2217") != std::string::npos);
    REQUIRE(transport.public_path().find("rfc2217-listen://") == 0);

    // With no client, lines still move.
    transport.set_control_line(control_line::dsr, true);
    transport.set_control_line(control_line::dtr, true);
    REQUIRE(transport.lines().dsr());
    REQUIRE(transport.lines().dtr());

    asio::io_context client_io;
    asio::ip::tcp::socket client(client_io);
    client.connect({asio::ip::make_address("127.0.0.1"),
                    transport.local_port()});
    rfc2217::session reference;
    const std::string greeting = reference.greeting();
    REQUIRE_EQ(receive(client, greeting.size()), greeting);

    // This client never agrees to COM-PORT, so modem state is not sent to it.
    transport.set_control_line(control_line::cts, true);
    transport.set_control_line(control_line::cts, false);
    asio::write(client, asio::buffer(std::string("plain")));
    const auto got = read_from(transport);
    REQUIRE_EQ(got.text, std::string("plain"));
    REQUIRE(receive(client, 1).empty());

    // The client goes away; the endpoint notices and a write still completes.
    client.close();
    for (int attempt = 0; attempt < 300 && transport.peer_count() != 0;
         ++attempt)
        std::this_thread::sleep_for(10ms);
    REQUIRE_EQ(transport.peer_count(), std::size_t{0});
    auto promise = std::make_shared<std::promise<std::size_t>>();
    const std::string out = "gone";
    transport.async_write_some(
        asio::buffer(out),
        [promise](boost::system::error_code, std::size_t count)
        { promise->set_value(count); });
    auto written = promise->get_future();
    REQUIRE(written.wait_for(3000ms) == std::future_status::ready);
    REQUIRE_EQ(written.get(), std::size_t{4});
}

TEST_CASE("rfc2217: a break the client turns on and off keeps its place among the bytes")
{
    rfc2217::session session = ready_session();
    const auto result = session.feed(std::string("AB") + command(5, bytes({5})) +
                                     "CD" + command(5, bytes({6})) + "E");
    REQUIRE_EQ(result.data, std::string("ABCDE"));
    REQUIRE_EQ(result.breaks.size(), std::size_t{2});
    REQUIRE_EQ(result.breaks[0].at, std::size_t{2});
    REQUIRE(result.breaks[0].on);
    REQUIRE_EQ(result.breaks[1].at, std::size_t{4});
    REQUIRE(!result.breaks[1].on);

    // Asking for the state is not a break.
    const auto asked = session.feed(command(5, bytes({4})));
    REQUIRE(asked.breaks.empty());
}

TEST_CASE("rfc2217: the listener reports a client's break as a begin and an end between its bytes")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::rfc2217;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);

    // What the endpoint is told, in the order it is told it.
    struct log
    {
        std::mutex mutex;
        std::string order;
        std::vector<line_break> breaks;
    };
    auto seen = std::make_shared<log>();
    transport.set_break_sink(
        [seen](const line_break& item)
        {
            std::lock_guard lock(seen->mutex);
            seen->order += item.phase == break_phase::begin ? "[begin]" : "[end]";
            seen->breaks.push_back(item);
        });

    asio::io_context client_io;
    asio::ip::tcp::socket client(client_io);
    client.connect({asio::ip::make_address("127.0.0.1"),
                    transport.local_port()});
    rfc2217::session reference;
    const std::string greeting = reference.greeting();
    REQUIRE_EQ(receive(client, greeting.size()), greeting);
    asio::write(client, asio::buffer(bytes({iac, do_option, com_port})));

    const auto read_one = [&]
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto buffer = std::make_shared<std::array<char, 256>>();
        transport.async_read_some(
            asio::buffer(*buffer),
            [promise, buffer, seen](boost::system::error_code,
                                    std::size_t count)
            {
                {
                    std::lock_guard lock(seen->mutex);
                    if (count != 0)
                        seen->order += std::string(buffer->data(), count);
                }
                promise->set_value();
            });
        auto done = promise->get_future();
        REQUIRE(done.wait_for(3000ms) == std::future_status::ready);
    };

    // One write from the client holds bytes, the break on, bytes, the break
    // off and bytes.
    asio::write(client, asio::buffer(std::string("ab") + command(5, bytes({5})) +
                                     "cd" + command(5, bytes({6})) + "e"));
    for (int index = 0; index < 8; ++index)
    {
        read_one();
        std::lock_guard lock(seen->mutex);
        if (seen->order == "ab[begin]cd[end]e")
            break;
    }
    // The next read may be the one that carries the break: keep reading until
    // the whole sequence is in.
    {
        std::lock_guard lock(seen->mutex);
        REQUIRE_EQ(seen->order, std::string("ab[begin]cd[end]e"));
        REQUIRE_EQ(seen->breaks.size(), std::size_t{2});
        REQUIRE(seen->breaks[0].measured);
        REQUIRE_EQ(seen->breaks[1].phase, break_phase::end);
        REQUIRE(seen->breaks[1].duration >= std::chrono::nanoseconds(0));
        REQUIRE(seen->breaks[1].start == seen->breaks[0].start);
    }
}

TEST_CASE("rfc2217: a client that goes with its break on ends it")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::rfc2217;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);

    auto phases = std::make_shared<std::string>();
    auto mutex = std::make_shared<std::mutex>();
    transport.set_break_sink(
        [phases, mutex](const line_break& item)
        {
            std::lock_guard lock(*mutex);
            *phases += item.phase == break_phase::begin ? "[begin]" : "[end]";
        });

    {
        asio::io_context client_io;
        asio::ip::tcp::socket client(client_io);
        client.connect({asio::ip::make_address("127.0.0.1"),
                        transport.local_port()});
        rfc2217::session reference;
        const std::string greeting = reference.greeting();
        REQUIRE_EQ(receive(client, greeting.size()), greeting);
        asio::write(client, asio::buffer(bytes({iac, do_option, com_port}) +
                                         command(5, bytes({5}))));
        // Reads until the begin has been told.
        for (int index = 0; index < 8; ++index)
        {
            auto promise = std::make_shared<std::promise<void>>();
            auto buffer = std::make_shared<std::array<char, 64>>();
            transport.async_read_some(
                asio::buffer(*buffer),
                [promise, buffer](boost::system::error_code, std::size_t)
                { promise->set_value(); });
            auto done = promise->get_future();
            if (done.wait_for(500ms) != std::future_status::ready)
            {
                transport.cancel();
                done.wait();
            }
            std::lock_guard lock(*mutex);
            if (*phases == "[begin]")
                break;
        }
        // The client goes away with the break still on.
    }

    // The end comes with the next read.
    for (int index = 0; index < 20; ++index)
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto buffer = std::make_shared<std::array<char, 64>>();
        transport.async_read_some(
            asio::buffer(*buffer),
            [promise, buffer](boost::system::error_code, std::size_t)
            { promise->set_value(); });
        auto done = promise->get_future();
        if (done.wait_for(250ms) != std::future_status::ready)
        {
            transport.cancel();
            done.wait();
        }
        std::lock_guard lock(*mutex);
        if (*phases == "[begin][end]")
            break;
    }
    std::lock_guard lock(*mutex);
    REQUIRE_EQ(*phases, std::string("[begin][end]"));
}
