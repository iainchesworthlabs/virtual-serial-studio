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
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace vss::agent
{

/// Which of this user's local addresses a listener or a connection means.
/// Each has its own pipe name or socket file, and its own rule that only one
/// program listens there.
enum class local_endpoint
{
    /// `vss_agent`: the studio itself (docs/agent.md).
    agent,
    /// The gateway's public endpoint for scripts and test harnesses
    /// (docs/remote-control.md, L1).
    control,
    /// The gateway's administration endpoint, for Studio and `vss_cli`.
    control_admin
};

/// What the operating system says about the process at the other end of a
/// connection.
struct peer_identity
{
    /// The user who opened the connection: the SID as text on Windows
    /// (`S-1-5-21-...`), the effective user id as a decimal number elsewhere.
    std::string user;
    /// True when that user is the one who runs this process.
    bool same_user = false;
    /// The process that connected, or that serves the other end.  0 when the
    /// system does not say.
    std::uint64_t process_id = 0;
    /// The path of that process's program, for a log.  Empty when it cannot
    /// be read.  It is looked up by process number after the connection was
    /// made, so it is a hint for a person and never a ground for a decision.
    std::string program;
};

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

    /// Asks the operating system who is at the other end.  `user` and
    /// `same_user` come from the kernel's record of the connection (the
    /// credentials of a Unix socket, the security context of a pipe's
    /// client), not from anything the other end sent.
    virtual result<peer_identity> peer() = 0;
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
/// As above, for any of this user's local endpoints.
result<std::string> local_address(local_endpoint endpoint,
                                  std::string_view instance);

/// Starts listening as this user's agent.
/// `protocol_errc::already_running` when an agent of this user and instance
/// name already listens, and `errc::permission_denied` when the address
/// belongs to someone else or its directory is open to others.
result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             std::string_view instance);
/// As above, for any of this user's local endpoints.
result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             local_endpoint endpoint, std::string_view instance);

/// Connects to this user's agent.  `protocol_errc::not_running` when none
/// listens and `protocol_errc::not_same_user` when what listens there runs as
/// someone else.
result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              std::string_view instance);
/// As above, for any of this user's local endpoints.
result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              local_endpoint endpoint, std::string_view instance);

} // namespace vss::agent
