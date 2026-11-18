//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A network client linked two ways to an RFC 2217 server: the proxy of a
// polled serial bus (a pool controller behind a network serial server, and the
// application that talks to it). The frames of both directions must arrive at
// the pace they were sent, not batched and not late.
//

#include "support/test_support.hpp"

#include <vss/ports/listen_transport.hpp>
#include <vss/studio/studio.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;

namespace {
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

struct timing
{
    double median = 0;
    double p99 = 0;
    double worst = 0;
};

timing summarise(std::vector<double> milliseconds)
{
    std::sort(milliseconds.begin(), milliseconds.end());
    return {milliseconds[milliseconds.size() / 2],
            milliseconds[milliseconds.size() * 99 / 100], milliseconds.back()};
}

void report(const char* what, const timing& result)
{
    std::cout << what << ": median " << result.median << " ms, p99 "
              << result.p99 << " ms, worst " << result.worst << " ms\n";
}

double elapsed_ms(clock_type::time_point from, clock_type::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

/// A frame of `length` bytes (at least 6) that carries its number: the open
/// character, four digits, the close character, then padding.
std::string numbered_frame(char open, char close, int number,
                           std::size_t length)
{
    std::string frame;
    frame += open;
    frame += std::to_string(10000 + number).substr(1);
    frame += close;
    if (length > frame.size())
        frame.append(length - frame.size(), 'x');
    return frame;
}

/// Finds the numbered frames in a stream, whatever way it was cut up.
class frame_scanner
{
public:
    /// Calls seen(open, number) for each complete frame in what has come.
    template <class Seen>
    void feed(const char* data, std::size_t count, Seen&& seen)
    {
        pending_.append(data, count);
        for (;;)
        {
            const auto open = pending_.find_first_of("<[");
            if (open == std::string::npos)
            {
                pending_.clear();
                return;
            }
            if (pending_.size() < open + 6)
            {
                pending_.erase(0, open);
                return;
            }
            const char close = pending_[open] == '<' ? '>' : ']';
            if (pending_[open + 5] == close)
                seen(pending_[open], std::stoi(pending_.substr(open + 1, 4)));
            pending_.erase(0, open + 1);
        }
    }

private:
    std::string pending_;
};

} // namespace

TEST_CASE("system: a client linked to an RFC 2217 server carries the frames of "
          "a polled bus both ways at the pace they were sent")
{
    // The device is a plain TCP server (a network serial server on a raw port)
    // that sends a small frame of its own twenty times a second and answers
    // each poll at once. The application is a TCP client of the RFC 2217
    // server and sends a poll with each frame.
    constexpr std::size_t frames = 200;
    constexpr auto frame_gap = 20ms;

    boost::asio::io_context device_io;
    boost::asio::ip::tcp::acceptor acceptor(
        device_io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto device_port = acceptor.local_endpoint().port();

    studio app;
    const auto client = app.attach_serial(
        "device", "tcp://127.0.0.1:" + std::to_string(device_port));
    REQUIRE(client);
    const auto server =
        app.attach_serial("net-in", "rfc2217-listen://127.0.0.1:0");
    REQUIRE(server);
    REQUIRE(app.connect("device", "net-in"));

    boost::asio::ip::tcp::socket device(device_io);
    acceptor.accept(device);
    device.set_option(boost::asio::ip::tcp::no_delay(true));

    const auto address = parse_listen_uri(app.advertised_path((*server)->id()));
    REQUIRE(address.has_value());
    boost::asio::io_context application_io;
    boost::asio::ip::tcp::socket application(application_io);
    application.connect(
        {boost::asio::ip::make_address("127.0.0.1"), address->port});
    application.set_option(boost::asio::ip::tcp::no_delay(true));
    std::this_thread::sleep_for(300ms);

    std::vector<clock_type::time_point> device_sent(frames);
    std::vector<clock_type::time_point> frame_got(frames);
    std::vector<clock_type::time_point> poll_sent(frames);
    std::vector<clock_type::time_point> poll_got(frames);
    std::vector<clock_type::time_point> answer_got(frames);
    std::atomic<std::size_t> frames_seen{0};
    std::atomic<std::size_t> answers_seen{0};
    std::atomic<std::size_t> polls_seen{0};

    // The device answers each poll as soon as it has it.
    std::thread device_reader(
        [&]
        {
            frame_scanner scanner;
            std::array<char, 512> buffer{};
            boost::system::error_code ec;
            while (polls_seen < frames)
            {
                const std::size_t n =
                    device.read_some(boost::asio::buffer(buffer), ec);
                if (ec)
                    return;
                const auto at = clock_type::now();
                scanner.feed(buffer.data(), n,
                             [&](char, int number)
                             {
                                 if (number < 0 ||
                                     static_cast<std::size_t>(number) >= frames)
                                     return;
                                 poll_got[static_cast<std::size_t>(number)] =
                                     at;
                                 boost::asio::write(
                                     device, boost::asio::buffer(numbered_frame(
                                                 '[', ']', number, 6)));
                                 ++polls_seen;
                             });
            }
        });

    // The application reads what the server sends: frames and answers.
    std::thread application_reader(
        [&]
        {
            frame_scanner scanner;
            std::array<char, 1024> buffer{};
            boost::system::error_code ec;
            while (frames_seen < frames || answers_seen < frames)
            {
                const std::size_t n =
                    application.read_some(boost::asio::buffer(buffer), ec);
                if (ec)
                    return;
                const auto at = clock_type::now();
                scanner.feed(
                    buffer.data(), n,
                    [&](char open, int number)
                    {
                        if (number < 0 ||
                            static_cast<std::size_t>(number) >= frames)
                            return;
                        const auto index = static_cast<std::size_t>(number);
                        auto& slot = open == '<' ? frame_got[index]
                                                 : answer_got[index];
                        if (slot != clock_type::time_point{})
                            return;
                        slot = at;
                        ++(open == '<' ? frames_seen : answers_seen);
                    });
            }
        });

    const auto begin = clock_type::now();
    for (std::size_t i = 0; i < frames; ++i)
    {
        std::this_thread::sleep_until(
            begin + frame_gap * static_cast<int>(i));
        const int number = static_cast<int>(i);
        device_sent[i] = clock_type::now();
        boost::asio::write(
            device,
            boost::asio::buffer(numbered_frame(
                '<', '>', number, 6 + (i % 15))));
        poll_sent[i] = clock_type::now();
        boost::asio::write(
            application,
            boost::asio::buffer(numbered_frame(
                '<', '>', number, 6 + ((i * 7) % 15))));
    }

    const auto deadline = clock_type::now() + 5s;
    while ((frames_seen < frames || answers_seen < frames) &&
           clock_type::now() < deadline)
        std::this_thread::sleep_for(10ms);

    // Unblock the readers if frames were lost.
    boost::system::error_code ignored;
    device.close(ignored);
    application.close(ignored);
    device_reader.join();
    application_reader.join();

    REQUIRE_EQ(frames_seen.load(), frames);
    REQUIRE_EQ(answers_seen.load(), frames);

    std::vector<double> forward, poll_forward, round_trip, gaps;
    for (std::size_t i = 0; i < frames; ++i)
    {
        forward.push_back(elapsed_ms(device_sent[i], frame_got[i]));
        poll_forward.push_back(elapsed_ms(poll_sent[i], poll_got[i]));
        round_trip.push_back(elapsed_ms(poll_sent[i], answer_got[i]));
        // How far the gap at the application is from the gap the frames were
        // sent with, so a sender that was held up and then caught up is not
        // taken for batching by the link.
        if (i > 0)
            gaps.push_back(std::abs(
                elapsed_ms(frame_got[i - 1], frame_got[i]) -
                elapsed_ms(device_sent[i - 1], device_sent[i])));
    }
    const timing device_to_application = summarise(forward);
    const timing application_to_device = summarise(poll_forward);
    const timing poll_round = summarise(round_trip);
    const timing spacing = summarise(gaps);
    report("device to application, one frame", device_to_application);
    report("application to device, one poll", application_to_device);
    report("poll to its answer, at the application", poll_round);
    report("gap between frames at the application, less the gap sent", spacing);

    REQUIRE(device_to_application.median < 20.0);
    REQUIRE(device_to_application.p99 < 100.0);
    REQUIRE(application_to_device.median < 20.0);
    REQUIRE(application_to_device.p99 < 100.0);
    REQUIRE(poll_round.median < 40.0);
    REQUIRE(poll_round.p99 < 150.0);
    // Batching shows as a gap that is longer than the one sent, followed by
    // one that is shorter.
    REQUIRE(spacing.p99 < 80.0);
}

TEST_CASE("system: a network client keeps its connection when the device "
          "reports a framing error")
{
    // A network serial server tells an RFC 2217 client of a fault on its line
    // with a line state notice. The client asked for them; the fault is a
    // byte that was received badly, not the end of the connection. It must be
    // counted and reported, and the bytes after it must still arrive.
    boost::asio::io_context device_io;
    boost::asio::ip::tcp::acceptor acceptor(
        device_io, {boost::asio::ip::make_address("127.0.0.1"), 0});

    studio app;
    const auto client = app.attach_serial(
        "device",
        "rfc2217://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()));
    REQUIRE(client);

    boost::asio::ip::tcp::socket device(device_io);
    acceptor.accept(device);
    device.set_option(boost::asio::ip::tcp::no_delay(true));

    // IAC SB COM-PORT NOTIFY-LINESTATE(server) <framing> IAC SE
    const std::string framing_notice = {
        '\xFF', '\xFA', '\x2C', '\x6A', '\x08', '\xFF', '\xF0'};
    std::size_t expected = 0;
    for (int round = 0; round < 5; ++round)
    {
        boost::asio::write(device, boost::asio::buffer(framing_notice));
        boost::asio::write(device, boost::asio::buffer(std::string("hello")));
        expected += 5;
        REQUIRE(wait_for_rx(**client, expected, 3000ms));
    }
    REQUIRE_EQ((*client)->state(), endpoint_state::open);
    REQUIRE(wait_until([&] { return (*client)->stats().errors >= 5; }, 3000ms));
    REQUIRE_EQ((*client)->state(), endpoint_state::open);
}


namespace {

/// A fake RFC 2217 server's side of one connection: it agrees to COM-PORT,
/// as a real one does, and keeps what the client sends.
class fake_server
{
public:
    explicit fake_server(boost::asio::ip::tcp::socket& socket)
        : socket_(socket)
    {
        socket_.non_blocking(true);
    }

    void agree()
    {
        // IAC DO COM-PORT, IAC WILL COM-PORT
        write({'\xFF', '\xFD', '\x2C', '\xFF', '\xFB', '\x2C'});
    }

    void write(const std::string& bytes)
    {
        boost::asio::write(socket_, boost::asio::buffer(bytes));
    }

    /// Reads for `time` and returns everything the client sent.
    const std::string& read_for(std::chrono::milliseconds time)
    {
        const auto until = clock_type::now() + time;
        std::array<char, 512> buffer{};
        while (clock_type::now() < until)
        {
            boost::system::error_code ec;
            const std::size_t n =
                socket_.read_some(boost::asio::buffer(buffer), ec);
            if (!ec)
                received_.append(buffer.data(), n);
            else
                std::this_thread::sleep_for(5ms);
        }
        return received_;
    }

private:
    boost::asio::ip::tcp::socket& socket_;
    std::string received_;
};

/// IAC SB COM-PORT <code> <value...> IAC SE
std::string com_port_command(char code, const std::string& value)
{
    return std::string{'\xFF', '\xFA', '\x2C', code} + value +
           std::string{'\xFF', '\xF0'};
}

} // namespace

TEST_CASE("system: a network client asks the device for its serial settings "
          "and does not set them")
{
    // The port belongs to the server. A client that sets 115200 8N1 on a
    // device that runs a 9600 bus makes every byte of the bus a framing error.
    boost::asio::io_context device_io;
    boost::asio::ip::tcp::acceptor acceptor(
        device_io, {boost::asio::ip::make_address("127.0.0.1"), 0});

    studio app;
    const auto client = app.attach_serial(
        "device",
        "rfc2217://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()),
        serial_settings{});
    REQUIRE(client);

    boost::asio::ip::tcp::socket device(device_io);
    acceptor.accept(device);
    fake_server server(device);
    server.agree();
    const std::string received = server.read_for(1s);

    // Every setting is the request form: the value zero.
    const std::string zero4(4, '\0');
    REQUIRE(received.find(com_port_command('\x01', zero4)) !=
            std::string::npos);
    for (const char code : {'\x02', '\x03', '\x04', '\x05'})
        REQUIRE(received.find(com_port_command(code, std::string(1, '\0'))) !=
                std::string::npos);
    // And no setting: not the 115200 of the defaults, nor any other rate.
    REQUIRE(received.find(com_port_command('\x01', std::string{
                '\x00', '\x01', '\xC2', '\x00'})) == std::string::npos);

    // The device answers with what its port is, which the client takes as the
    // format of the line.
    server.write(com_port_command('\x65', std::string{
                     '\x00', '\x00', '\x25', '\x80'}));
    server.write(com_port_command('\x66', std::string(1, '\x08')));
    server.write(com_port_command('\x67', std::string(1, '\x01')));
    server.write(com_port_command('\x68', std::string(1, '\x01')));
    REQUIRE(wait_until(
        [&]
        {
            const auto format = (*client)->peer_format();
            return format && format->baud_rate == 9600 &&
                   format->data_bits == 8 &&
                   format->parity == parity_mode::none &&
                   format->stop_bits == stop_bits_mode::one;
        },
        3000ms));
    REQUIRE_EQ((*client)->state(), endpoint_state::open);
}

TEST_CASE("system: a network client whose address says settings=own sets "
          "them")
{
    boost::asio::io_context device_io;
    boost::asio::ip::tcp::acceptor acceptor(
        device_io, {boost::asio::ip::make_address("127.0.0.1"), 0});

    studio app;
    const auto client = app.attach_serial(
        "device",
        "rfc2217://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) +
            "?settings=own",
        serial_settings{});
    REQUIRE(client);

    boost::asio::ip::tcp::socket device(device_io);
    acceptor.accept(device);
    fake_server server(device);
    server.agree();
    const std::string received = server.read_for(1s);

    // 115200 is the Studio default: 0x0001C200.
    REQUIRE(received.find(com_port_command('\x01', std::string{
                '\x00', '\x01', '\xC2', '\x00'})) != std::string::npos);
    REQUIRE(received.find(com_port_command('\x01', std::string(4, '\0'))) ==
            std::string::npos);
}
