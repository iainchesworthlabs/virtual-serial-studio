//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>

#include <string>
#include <type_traits>

namespace vss::agent
{

/// What can go wrong between a client and the agent, apart from what the
/// engine itself reports (those keep their `vss::errc` value across the wire).
enum class protocol_errc
{
    success = 0,
    /// The bytes are not a frame: a length that cannot be right.
    malformed_frame,
    /// A frame longer than `max_frame_size`.
    oversized_frame,
    /// A frame whose header is not the JSON this protocol expects.
    malformed_message,
    /// A request names an operation this agent does not have.
    unknown_operation,
    /// A request arrived before `hello`.
    handshake_required,
    /// The client and the agent speak different protocol versions.
    version_mismatch,
    /// The agent has as many clients as it takes.
    busy,
    /// The other side closed the connection.
    connection_closed,
    /// The other end of the connection runs as another user.
    not_same_user,
    /// An agent for this user and instance name is already running.
    already_running,
    /// No agent for this user and instance name is running.
    not_running
};

/// The category of `protocol_errc`, named `vss.agent`.
const boost::system::error_category& protocol_category() noexcept;
boost::system::error_code make_error_code(protocol_errc value) noexcept;

/// An error as it crosses the wire: the name of its category, its value in
/// that category and its message as the agent rendered it.
struct wire_error
{
    std::string category;
    int value = 0;
    std::string message;

    friend bool operator==(const wire_error&, const wire_error&) = default;
};

/// The wire form of an error code.
wire_error to_wire(const error_code& error);

/// The error code a wire error stands for on this side.  An engine error
/// (`vss`), a protocol error (`vss.agent`) and an operating system error
/// (`system`, `generic`) keep their category and value, because the client
/// and the agent run on the same computer.  A value the category does not
/// have becomes `errc::internal`, and any other category `errc::io_error`;
/// the message still says what the agent saw.
error_code from_wire(const wire_error& error);

} // namespace vss::agent

namespace boost::system
{
template <>
struct is_error_code_enum<vss::agent::protocol_errc> : std::true_type
{
};
} // namespace boost::system
