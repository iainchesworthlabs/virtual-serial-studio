//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace vss::control
{

/// The errors of the control API (docs/remote-control.md, section 9).  The
/// list is closed within a major version of the API: a client can switch on
/// the names, and nothing about the engine or the computer crosses with them.
enum class api_errc
{
    success = 0,
    /// The caller has not shown who it is, or what it showed was refused.
    /// Every such refusal looks the same to the caller.
    unauthenticated,
    /// The caller is known and may not do this.
    forbidden,
    /// The request is not what this API expects.
    invalid_argument,
    not_found,
    conflict,
    /// An operation, a version or a feature this build does not have.
    unsupported,
    /// The endpoint has as many sessions as it takes.
    busy,
    timeout,
    /// More than a limit allows: a frame too large, requests too fast.
    limit_exceeded,
    cancelled,
    /// The agent that owns the ports is not running or went away.
    unavailable,
    internal
};

/// The category of `api_errc`, named `vss.control`.
const boost::system::error_category& api_category() noexcept;
boost::system::error_code make_error_code(api_errc value) noexcept;

/// The name of an error on the wire, such as `limit_exceeded`.
std::string_view code_name(api_errc value) noexcept;

/// The error a wire name stands for, or nothing for a name this build does
/// not know.
std::optional<api_errc> parse_code(std::string_view name) noexcept;

/// True when the same request may succeed later without a change.
bool retryable(api_errc value) noexcept;

/// An error as a client receives it.
struct api_error
{
    std::string code;
    std::string message;
    bool retryable = false;

    friend bool operator==(const api_error&, const api_error&) = default;
};

/// The wire form of `value`, with the category's own message.
api_error to_api_error(api_errc value);

/// The error code a wire error stands for.  A name this build does not know
/// becomes `api_errc::internal`.
error_code from_api_error(const api_error& error) noexcept;

/// The public name of an error of the engine: `engine.not_open`.  An error
/// of the operating system or of anything else is `engine.io_error`, so no
/// number or text of the computer crosses with it.  Empty for no error.
std::string engine_error_name(const error_code& error);

} // namespace vss::control

namespace boost::system
{
template <> struct is_error_code_enum<vss::control::api_errc> : std::true_type
{
};
} // namespace boost::system
