//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/com0com.hpp>
#include <vss/ports/native_lines.hpp>
#include <vss/ports/peer_open.hpp>
#include <vss/ports/process_control.hpp>
#include <vss/ports/virtual_port.hpp>

#include <vss/testkit/harness.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

extern char** environ;

using namespace vss;

TEST_CASE("native lines: peer driven inputs report the OS error")
{
    // POSIX can drive CTS/DSR/DCD/RI on a PTY, so an invalid handle reports
    // the OS error instead of an unsupported-feature error.
    for (const auto line : {control_line::cts, control_line::dsr,
                            control_line::dcd, control_line::ri})
    {
        const auto result =
            native::write_line(static_cast<std::intptr_t>(1234), line, true);
        REQUIRE(!result);
    }
}

TEST_CASE("com0com: every operation is unsupported on POSIX")
{
    // com0com is a Windows driver.
    com0com_installation fake;
    fake.found = true;
    fake.setupc_path = "does-not-exist";

    REQUIRE_EQ(create_com0com_pair(fake, "", "COM11", false),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(create_com0com_pair(fake, "COM10", "COM11", false),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(remove_com0com_pair(fake, 0, false),
               make_error_code(errc::unsupported));
}

TEST_CASE("virtual ports: pseudo terminals are supported on POSIX")
{
    REQUIRE(supports(virtual_port_kind::pty));
}

TEST_CASE("virtual ports: a pty is a real device")
{
    boost::asio::io_context io;
    const auto created =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "pty");
    REQUIRE(created);
    REQUIRE_EQ(created->kind, virtual_port_kind::pty);
    REQUIRE(created->path.find("/dev/") == 0);
    REQUIRE(created->port->is_open());
    REQUIRE(created->port->supports_control_lines());
    REQUIRE(!created->port->describe().empty());

    created->port->set_control_line(control_line::dtr, true);
    created->port->refresh_lines();
    created->port->close();
    REQUIRE(!created->port->is_open());
}

namespace
{

/// The descriptors this process has open, up to a limit well above what a
/// test run uses.
std::vector<int> open_descriptors()
{
    std::vector<int> out;
    for (int descriptor = 0; descriptor < 4096; ++descriptor)
    {
        if (::fcntl(descriptor, F_GETFD) >= 0)
            out.push_back(descriptor);
    }
    return out;
}

} // namespace

TEST_CASE("virtual ports: a program Studio starts does not inherit a pty")
{
    boost::asio::io_context io;
    // The first port of an io_context also makes the reactor's own
    // descriptors.  Make them now, so that only the port's are new below.
    const auto first =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "first");
    REQUIRE(first);

    const auto before = open_descriptors();
    const auto created =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "kept");
    REQUIRE(created);

    // The master and the slave Studio holds open are both new, and both are
    // closed in a program started with exec.
    int added = 0;
    for (const int descriptor : open_descriptors())
    {
        if (std::find(before.begin(), before.end(), descriptor) != before.end())
            continue;
        ++added;
        const int flags = ::fcntl(descriptor, F_GETFD);
        REQUIRE(flags >= 0);
        REQUIRE((flags & FD_CLOEXEC) != 0);
    }
    REQUIRE_EQ(added, 2);
}

TEST_CASE("virtual ports: a pty carries data in both directions")
{
    boost::asio::io_context io;
    const auto created =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "io");
    REQUIRE(created);

    const int slave = ::open(created->path.c_str(), O_RDWR | O_NOCTTY);
    REQUIRE(slave >= 0);

    // master -> slave
    std::size_t written = 0;
    created->port->async_write_some(
        boost::asio::buffer("hello", 5),
        [&](boost::system::error_code ec, std::size_t count)
        {
            if (!ec)
                written = count;
        });
    io.run();
    REQUIRE_EQ(written, std::size_t{5});

    char buffer[16]{};
    pollfd descriptor{slave, POLLIN, 0};
    REQUIRE(::poll(&descriptor, 1, 2000) > 0);
    REQUIRE_EQ(::read(slave, buffer, 5), ssize_t{5});
    REQUIRE_EQ(std::string(buffer, 5), std::string("hello"));

    // slave -> master
    REQUIRE_EQ(::write(slave, "world", 5), ssize_t{5});
    std::size_t received = 0;
    created->port->async_read_some(
        boost::asio::buffer(buffer),
        [&](boost::system::error_code ec, std::size_t count)
        {
            if (!ec)
                received = count;
        });
    io.restart();
    io.run();
    REQUIRE_EQ(received, std::size_t{5});
    REQUIRE_EQ(std::string(buffer, 5), std::string("world"));

    created->port->set_control_line(control_line::dtr, true);
    created->port->refresh_lines();
    REQUIRE(created->port->lines().dtr());

    ::close(slave);
    created->port->close();
    REQUIRE(!created->port->is_open());
}

TEST_CASE("peer open: another process holding a pty open is noticed")
{
    using namespace std::chrono_literals;

    boost::asio::io_context io;
    const auto created =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "peer");
    REQUIRE(created);
    const std::vector<std::string> paths{created->path};
    REQUIRE(peer_open({}).empty());

    if (!peer_open_supported())
    {
        // A platform that cannot tell must say so for every path.
        const auto states = peer_open(paths);
        REQUIRE_EQ(states.size(), std::size_t{1});
        REQUIRE(states[0] == peer_open_state::unknown);
        return;
    }

    // This process holds the master, and the slave is not open anywhere.
    REQUIRE(peer_open(paths)[0] == peer_open_state::not_open);

    // A shell runs sleep with the pseudo terminal as its standard input.
    std::string shell = "/bin/sh";
    std::string flag = "-c";
    std::string script = "exec sleep 30 < \"$0\"";
    std::string device = created->path;
    char* const arguments[] = {shell.data(), flag.data(), script.data(),
                               device.data(), nullptr};
    pid_t child = 0;
    REQUIRE_EQ(::posix_spawn(&child, shell.c_str(), nullptr, nullptr,
                             arguments, environ),
               0);

    const bool seen = vss::testkit::wait_until(
        [&] { return peer_open(paths)[0] == peer_open_state::open; }, 5000ms);
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(seen);

    REQUIRE(vss::testkit::wait_until(
        [&] { return peer_open(paths)[0] == peer_open_state::not_open; },
        5000ms));
}

TEST_CASE("pseudo terminal: the baud rate and framing a program sets are reported")
{
    boost::asio::io_context io;
    const auto created =
        create_virtual_port(io.get_executor(), virtual_port_kind::pty, "fmt");
    REQUIRE(created);
    REQUIRE(created->port != nullptr);

    // Nobody has chosen anything yet: Studio's own raw start is not a choice.
    REQUIRE(!created->port->peer_format().has_value());

    const int slave = ::open(created->path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(slave >= 0);

    struct termios settings{};
    REQUIRE_EQ(::tcgetattr(slave, &settings), 0);
    const struct termios original = settings;
    ::cfsetispeed(&settings, B9600);
    ::cfsetospeed(&settings, B9600);
    settings.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARODD);
    settings.c_cflag |= CS7 | PARENB | CSTOPB;
    REQUIRE_EQ(::tcsetattr(slave, TCSANOW, &settings), 0);

    // The baud rate always comes through.  Whether the framing does is up to
    // the system: Linux forces the slave to 8N1 whatever a program asks for,
    // macOS keeps it.  The port says which it is, and never claims a framing
    // the program did not choose.
    const auto reported = created->port->peer_format();
    REQUIRE(reported.has_value());
    REQUIRE_EQ(reported->baud_rate, 9600u);
    if (reported->framing_known)
    {
        REQUIRE_EQ(reported->data_bits, 7u);
        REQUIRE(reported->parity == parity_mode::even);
        REQUIRE(reported->stop_bits == stop_bits_mode::two);

        // Odd parity and one stop bit.
        settings.c_cflag |= PARODD;
        settings.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
        REQUIRE_EQ(::tcsetattr(slave, TCSANOW, &settings), 0);
        const auto odd = created->port->peer_format();
        REQUIRE(odd.has_value());
        REQUIRE(odd->parity == parity_mode::odd);
        REQUIRE(odd->stop_bits == stop_bits_mode::one);
    }
    else
    {
        REQUIRE_EQ(to_label(*reported), std::string("9600"));
    }

    // Back to where it started: no longer a program's choice.
    REQUIRE_EQ(::tcsetattr(slave, TCSANOW, &original), 0);
    REQUIRE(!created->port->peer_format().has_value());

    ::close(slave);
    created->port->close();
    // A closed port reports nothing.
    REQUIRE(!created->port->peer_format().has_value());
}

TEST_CASE("process control: there is nothing to ask where there is no driver")
{
    REQUIRE(!vss::describe_process(1).has_value());
    REQUIRE_EQ(vss::terminate_process(1),
               vss::make_error_code(vss::errc::unsupported));
}
