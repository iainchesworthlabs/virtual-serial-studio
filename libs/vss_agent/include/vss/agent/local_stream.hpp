//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace vss::agent
{

/// One connection between a client and the agent: a named pipe on Windows and
/// a Unix domain socket elsewhere.  Nothing here listens on a network
/// interface.
///
/// Handlers run on the executor the stream was made with.  A stream is used
/// from one thread at a time; destroying it completes what it still owes with
/// `operation_aborted`.
class local_stream
{
public:
    using io_handler =
        std::function<void(boost::system::error_code, std::size_t)>;

    virtual ~local_stream() = default;

    virtual void async_read_some(boost::asio::mutable_buffer buffer,
                                 io_handler handler) = 0;
    virtual void async_write_some(boost::asio::const_buffer buffer,
                                  io_handler handler) = 0;
    virtual void close() = 0;
};

/// The agent's end: where clients connect.
///
/// Only the user who runs the agent can connect.  The operating system
/// refuses everyone else (the pipe's access list, the mode of the socket's
/// directory), and a connection that still arrives from another user is
/// closed here and never reaches the handler.
class local_listener
{
public:
    using accept_handler = std::function<void(boost::system::error_code,
                                              std::unique_ptr<local_stream>)>;

    virtual ~local_listener() = default;

    /// Waits for one client.  One accept at a time.
    virtual void async_accept(accept_handler handler) = 0;
    /// Stops listening and removes the address.  A waiting accept completes
    /// with `operation_aborted`.
    virtual void close() = 0;
    /// The pipe name or the socket path, for a person to read.
    virtual std::string address() const = 0;
};

/// True for a name an agent instance may have: up to 32 of the characters
/// `A-Z a-z 0-9 _ -`.  The empty name is the user's one ordinary agent; a test
/// gives its own agent another name so that it never meets that one.  The
/// name is the only part of the address a caller chooses, so an address can
/// never be made to point outside the user's own pipe names or directory.
bool valid_instance_name(std::string_view name) noexcept;

/// The address of an instance for this user: `\\.\pipe\vss-agent-<SID>` on
/// Windows, `<runtime directory>/agent.sock` elsewhere, with `-<name>` added
/// for a named instance.  `errc::invalid_argument` for a name that is not
/// valid.
result<std::string> local_address(std::string_view instance);

/// Starts listening as this user's agent.
/// `protocol_errc::already_running` when an agent of this user and instance
/// name already listens, and `errc::permission_denied` when the address
/// belongs to someone else or its directory is open to others.
result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             std::string_view instance);

/// Connects to this user's agent.  `protocol_errc::not_running` when none
/// listens and `protocol_errc::not_same_user` when what listens there runs as
/// someone else.
result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              std::string_view instance);

} // namespace vss::agent
