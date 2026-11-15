//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The TCP listener of the gateway (src/loopback.cpp) on a port that was used
// before.  A gateway that stops with a client connected closes that
// connection first, so its side of the port is in TIME_WAIT; the next gateway
// must still be able to listen on the same port, and must not be able to
// share a port that is in use.
//

#include <vss/agent/local_stream.hpp>
#include <vss/control/loopback.hpp>
#include <vss/core/error.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace control = vss::control;
namespace agent = vss::agent;
using namespace std::chrono_literals;

namespace
{

/// Runs the executor until `done` or `limit` passes.
template <class Predicate>
bool run_until(boost::asio::io_context& io, Predicate done,
               std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        io.restart();
        io.run_one_for(deadline - now);
    }
    return true;
}

struct running_listener
{
    std::unique_ptr<agent::local_listener> listener;
    std::uint16_t port = 0;
};

running_listener listen_on(boost::asio::io_context& io, std::uint16_t port)
{
    auto listening = control::listen_loopback(io.get_executor(), port);
    REQUIRE(listening);
    running_listener out;
    out.listener = std::move(*listening);
    out.port = control::loopback_port_of(out.listener->address());
    REQUIRE(out.port != 0);
    return out;
}

/// A client that has connected to `listener`, and the stream the listener
/// accepted for it.
struct connection
{
    std::unique_ptr<agent::local_stream> client;
    std::unique_ptr<agent::local_stream> accepted;
};

connection connect_to(boost::asio::io_context& io, running_listener& where)
{
    connection made;
    where.listener->async_accept(
        [&made](boost::system::error_code failed,
                std::unique_ptr<agent::local_stream> stream)
        {
            if (!failed)
                made.accepted = std::move(stream);
        });
    auto client = control::connect_loopback(io.get_executor(), where.port);
    REQUIRE(client);
    made.client = std::move(*client);
    REQUIRE(
        run_until(io, [&made] { return made.accepted != nullptr; }, 5000ms));
    return made;
}

} // namespace

TEST_CASE("control loopback: a listener can listen again on the port it "
          "closed, with a connection still open")
{
    boost::asio::io_context io;
    running_listener first = listen_on(io, 0);
    const std::uint16_t port = first.port;
    connection open = connect_to(io, first);

    // The gateway stops: the listener closes, and so do its connections
    // before the client does.
    first.listener->close();
    first.listener.reset();

    // Another gateway starts at once, on the port the first one had, while
    // its connection is still open.
    auto again = control::listen_loopback(io.get_executor(), port);
    REQUIRE(again);
    REQUIRE_EQ(control::loopback_port_of((*again)->address()), port);
    (*again)->close();
    again->reset();

    // The side that closes first holds the port in TIME_WAIT afterwards.
    open.accepted->close();
    open.accepted.reset();
    run_until(io, [] { return false; }, 100ms);
    open.client->close();
    open.client.reset();
    run_until(io, [] { return false; }, 100ms);

    auto last = control::listen_loopback(io.get_executor(), port);
    REQUIRE(last);
    REQUIRE_EQ(control::loopback_port_of((*last)->address()), port);

    // The client library still reaches the new listener.
    auto reached = control::connect_loopback(io.get_executor(), port);
    REQUIRE(reached);
}

TEST_CASE("control loopback: a port that a listener holds is not shared")
{
    boost::asio::io_context io;
    running_listener first = listen_on(io, 0);

    // The same port, while the first listens: refused, never shared.
    const auto second = control::listen_loopback(io.get_executor(), first.port);
    REQUIRE(!second);

    // Not even by a program that asks for a shared port with the option that
    // lets two listeners share one on some systems.
    boost::asio::ip::tcp::acceptor other(io.get_executor());
    boost::system::error_code failed;
    other.open(boost::asio::ip::tcp::v4(), failed);
    REQUIRE(!failed);
    other.set_option(boost::asio::socket_base::reuse_address(true), failed);
    other.bind({boost::asio::ip::address_v4::loopback(), first.port}, failed);
    REQUIRE(static_cast<bool>(failed));
}
