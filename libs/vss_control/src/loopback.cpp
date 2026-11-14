//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/loopback.hpp>

#include "loopback_platform.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace vss::control
{
namespace
{

using boost::asio::ip::tcp;

/// Connections that were accepted and that the gateway has not taken yet.
/// One more is closed: the gateway takes them as fast as it can, so a queue
/// this long means a flood.
constexpr std::size_t max_ready = 16;
constexpr int listen_backlog = 16;
constexpr std::chrono::milliseconds accept_retry_delay{100};

std::string text_of(const tcp::endpoint& endpoint)
{
    return endpoint.address().to_string() + ":" +
           std::to_string(endpoint.port());
}

class tcp_stream final : public agent::local_stream
{
public:
    tcp_stream(tcp::socket socket, std::string remote)
        : socket_(std::move(socket)), remote_(std::move(remote))
    {
    }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        socket_.async_read_some(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        socket_.async_write_some(buffer, std::move(handler));
    }

    void close() override
    {
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

    /// Where the connection came from, and nothing about who made it: the
    /// user is left empty and `same_user` false, whoever it is.
    result<agent::peer_identity> peer() override
    {
        agent::peer_identity out;
        out.program = "tcp " + remote_;
        return out;
    }

private:
    tcp::socket socket_;
    std::string remote_;
};

/// What the listener and its accepts in flight share.  Everything runs on
/// the executor the listener was made with, one handler at a time.
struct listening
{
    explicit listening(const boost::asio::any_io_executor& executor)
        : retry(executor)
    {
    }

    std::vector<tcp::acceptor> acceptors;
    std::deque<tcp::socket> ready;
    agent::local_listener::accept_handler waiting;
    boost::asio::steady_timer retry;
    std::string address;
    bool closed = false;
};

void deliver(const std::shared_ptr<listening>& state)
{
    if (state->closed || !state->waiting || state->ready.empty())
        return;
    tcp::socket socket = std::move(state->ready.front());
    state->ready.pop_front();
    auto handler = std::move(state->waiting);
    state->waiting = nullptr;

    boost::system::error_code failed;
    const tcp::endpoint remote = socket.remote_endpoint(failed);
    if (failed)
    {
        // It came and went.  The gateway is told, and asks again.
        handler(failed, nullptr);
        return;
    }
    boost::system::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    handler({},
            std::make_unique<tcp_stream>(std::move(socket), text_of(remote)));
}

void accept_on(const std::shared_ptr<listening>& state, std::size_t index)
{
    state->acceptors[index].async_accept(
        [state, index](boost::system::error_code failed, tcp::socket socket)
        {
            if (state->closed ||
                failed == boost::asio::error::operation_aborted)
                return;
            if (failed)
            {
                // Out of handles for a moment, or a client that gave up.
                // Not in a tight loop.
                state->retry.expires_after(accept_retry_delay);
                state->retry.async_wait(
                    [state, index](boost::system::error_code cancelled)
                    {
                        if (!cancelled && !state->closed)
                            accept_on(state, index);
                    });
                return;
            }
            boost::system::error_code unknown;
            const tcp::endpoint remote = socket.remote_endpoint(unknown);
            // The listener is bound to loopback, so this cannot be another
            // address.  It is looked at all the same, and a connection from
            // anywhere else is closed here.
            if (!unknown && remote.address().is_loopback() &&
                state->ready.size() < max_ready)
            {
                state->ready.push_back(std::move(socket));
                deliver(state);
            }
            accept_on(state, index);
        });
}

class loopback_listener final : public agent::local_listener
{
public:
    explicit loopback_listener(std::shared_ptr<listening> state)
        : state_(std::move(state))
    {
        for (std::size_t index = 0; index < state_->acceptors.size(); ++index)
            accept_on(state_, index);
    }

    ~loopback_listener() override { stop(); }

    loopback_listener(const loopback_listener&) = delete;
    loopback_listener& operator=(const loopback_listener&) = delete;

    void async_accept(accept_handler handler) override
    {
        state_->waiting = std::move(handler);
        // Never from inside this call: the caller asks again from its own
        // handler, and a queue of ready connections must not become a
        // recursion.
        boost::asio::post(state_->retry.get_executor(),
                          [state = state_] { deliver(state); });
    }

    void close() override { stop(); }

    std::string address() const override { return state_->address; }

private:
    void stop() noexcept
    {
        if (state_->closed)
            return;
        state_->closed = true;
        boost::system::error_code ignored;
        for (tcp::acceptor& acceptor : state_->acceptors)
            acceptor.close(ignored);
        state_->retry.cancel();
        state_->ready.clear();
        state_->waiting = nullptr;
    }

    std::shared_ptr<listening> state_;
};

/// An acceptor bound to one loopback address and listening, or the error.
result<tcp::acceptor> bound(const boost::asio::any_io_executor& executor,
                            const tcp::endpoint& where)
{
    tcp::acceptor acceptor(executor);
    boost::system::error_code failed;
    acceptor.open(where.protocol(), failed);
    if (failed)
        return failed;
    if (where.protocol() == tcp::v6())
    {
        // `::1` only: never an address of the other family through it.
        acceptor.set_option(boost::asio::ip::v6_only(true), failed);
        if (failed)
            return failed;
    }
    // The port is this listener's alone.  No option that would let another
    // program bind it too is ever set, and where the system needs to be
    // told so, it is.
    platform::make_listener_exclusive(
        static_cast<std::uintptr_t>(acceptor.native_handle()));
    acceptor.bind(where, failed);
    if (failed)
        return failed;
    acceptor.listen(listen_backlog, failed);
    if (failed)
        return failed;
    return acceptor;
}

} // namespace

result<std::unique_ptr<agent::local_listener>>
listen_loopback(const boost::asio::any_io_executor& executor,
                std::uint16_t port)
{
    auto state = std::make_shared<listening>(executor);
    auto first = bound(
        executor, tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    if (!first)
        return first.error();
    boost::system::error_code failed;
    const tcp::endpoint where = first->local_endpoint(failed);
    if (failed)
        return failed;
    state->address = text_of(where);
    state->acceptors.push_back(std::move(*first));

    // The same port on `::1`.  A computer that has no IPv6 goes without; a
    // port that someone else holds there is an error, because a client that
    // asks for "localhost" might reach them and not us.
    auto second =
        bound(executor, tcp::endpoint(boost::asio::ip::address_v6::loopback(),
                                      where.port()));
    if (second)
    {
        state->acceptors.push_back(std::move(*second));
    }
    else if (second.error() == boost::asio::error::address_in_use ||
             second.error() == boost::asio::error::access_denied)
    {
        boost::system::error_code ignored;
        state->acceptors.front().close(ignored);
        return second.error();
    }
    return std::unique_ptr<agent::local_listener>(
        new loopback_listener(std::move(state)));
}

result<std::unique_ptr<agent::local_stream>>
connect_loopback(const boost::asio::any_io_executor& executor,
                 std::uint16_t port)
{
    tcp::socket socket(executor);
    boost::system::error_code failed;
    const tcp::endpoint where(boost::asio::ip::address_v4::loopback(), port);
    socket.connect(where, failed);
    if (failed)
        return failed;
    boost::system::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    return std::unique_ptr<agent::local_stream>(
        new tcp_stream(std::move(socket), text_of(where)));
}

std::uint16_t loopback_port_of(const std::string& address) noexcept
{
    const std::size_t colon = address.rfind(':');
    if (colon == std::string::npos || colon + 1 >= address.size())
        return 0;
    std::uint32_t value = 0;
    for (std::size_t index = colon + 1; index < address.size(); ++index)
    {
        const char digit = address[index];
        if (digit < '0' || digit > '9')
            return 0;
        value = value * 10 + static_cast<std::uint32_t>(digit - '0');
        if (value > 65535)
            return 0;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace vss::control
