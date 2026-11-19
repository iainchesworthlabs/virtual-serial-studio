//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/bytes.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vss::debug
{

/// The version of the debug protocol.  A client and a studio talk only when
/// both have the same number.
inline constexpr std::uint32_t protocol_version = 1;

/// What a client may ask for.  A client names the scopes it wants in its
/// `hello`, and the person at the computer grants some or all of them.
enum class scope : std::uint32_t
{
    /// Engine state, the call recorder, the COM number picture, the driver
    /// and the plugin host.
    read = 1,
    /// Trying an engine operation: it can change a port.
    act = 2,
    /// The state of the window and a picture of it: both can show names,
    /// paths and traffic.
    gui = 4,
    /// The bytes of traffic.  Needs the person's second switch as well.
    data = 8
};

/// A set of scopes, as bits.
using scope_set = std::uint32_t;

constexpr scope_set bit(scope value) noexcept
{
    return static_cast<scope_set>(value);
}

constexpr bool has(scope_set set, scope value) noexcept
{
    return (set & bit(value)) != 0;
}

/// `debug.read`, `debug.act`, `debug.gui` or `debug.data`.
std::string_view scope_name(scope value) noexcept;
std::optional<scope> scope_from_name(std::string_view name) noexcept;

/// The scope an operation needs, or nothing for a name that is not one of
/// the operations.  `hello` and `ping` need none and are not listed.
std::optional<scope> scope_of_operation(std::string_view operation) noexcept;

/// What an operation produced: JSON text for the header's `result`, and bytes
/// that go in the frame's blob (a picture).  A failure carries the code from
/// the closed list of docs/debug-api.md and a message.
struct op_result
{
    bool ok = true;
    std::string code;
    std::string message;
    std::string json = "{}";
    byte_buffer blob;

    static op_result done(std::string json_text = "{}")
    {
        op_result out;
        out.json = std::move(json_text);
        return out;
    }
    static op_result fail(std::string code_text, std::string text)
    {
        op_result out;
        out.ok = false;
        out.code = std::move(code_text);
        out.message = std::move(text);
        return out;
    }
};

} // namespace vss::debug
