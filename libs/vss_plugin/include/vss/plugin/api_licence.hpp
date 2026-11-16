//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages about the licence on this computer: an official plugin asks
// the host for the licence file, so that it can check it with the same
// library as Studio, and tells the host what it found.  docs/plugins.md,
// section 8.7, and docs/licensing.md.
//
// A plugin may use these only when the host offers the engine capability
// `licence.file` and the plugin has the permission `licence.read`.  The host
// never judges a licence for the plugin and never sends one that was not
// asked for.  The bytes of a licence file are untrusted until the plugin has
// verified them.
//

#pragma once

#include <vss/license/license.hpp>
#include <vss/plugin/api_message.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace vss::plugin
{

/// The largest licence file the host sends, in bytes.  Studio reads no larger
/// file either (docs/licensing.md, "Where Studio finds a licence").
inline constexpr std::size_t max_licence_file = std::size_t{64} * 1024;

/// The longest licence number a report may name, in bytes.
inline constexpr std::size_t max_licence_id = 64;

/// The engine capability the host names in `hello_ack` when it answers
/// `licence.get`.
inline constexpr std::string_view licence_file_capability = "licence.file";

/// The name a message uses for a reason of the licence library, such as
/// `not_yet_valid`.  The library has the sentences and not the names.
std::string_view to_string(license::reason value) noexcept;

/// Asks the host for the best licence file on this computer.  The host
/// answers `licence.file`, or an `error` (`not_granted` when the plugin does
/// not have `licence.read`).
struct licence_get_message
{
    static constexpr std::string_view op() noexcept { return "licence.get"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    friend bool operator==(const licence_get_message&,
                           const licence_get_message&) = default;
};

/// The answer to `licence.get`.  When `present` is true the blob is the
/// bytes of the licence file exactly as on disk, 1 to `max_licence_file`.
/// When it is false there is no licence file and the blob is empty.
struct licence_file_message
{
    static constexpr std::string_view op() noexcept { return "licence.file"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::answer; }

    bool present = false;
    byte_buffer data;

    friend bool operator==(const licence_file_message&,
                           const licence_file_message&) = default;
};

/// What the plugin found when it checked the licence, as a reason of the
/// licence library.  The plugin sends no text of its own: Studio shows its
/// own sentence for the reason.  The host asked its own question
/// (`docs/plugins.md`, section 8.7), and a report changes nothing in it.
struct licence_report_message
{
    static constexpr std::string_view op() noexcept { return "licence.report"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    license::reason reason = license::reason::ok;
    /// The licence number, plain text of 1 to `max_licence_id` bytes.  Empty
    /// when the plugin has none to name, and then it is not written.
    std::string licence_id;

    friend bool operator==(const licence_report_message&,
                           const licence_report_message&) = default;
};

template <>
result<licence_get_message> decode_as<licence_get_message>(const api_message&,
                                                           std::string*);
template <>
result<api_message> encode_as<licence_get_message>(const licence_get_message&,
                                                   std::uint64_t);
template <>
result<licence_file_message> decode_as<licence_file_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<licence_file_message>(const licence_file_message&,
                                                    std::uint64_t);
template <>
result<licence_report_message>
decode_as<licence_report_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<licence_report_message>(const licence_report_message&, std::uint64_t);

} // namespace vss::plugin
