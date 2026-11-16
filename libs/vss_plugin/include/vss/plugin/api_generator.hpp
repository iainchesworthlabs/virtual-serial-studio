//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of a generator: a plugin that produces bytes onto a port.
// docs/plugins.md, section 7.4.
//
// Flow control is by credit.  The host sends `generator.credit`, and the
// plugin may send that many bytes in `generator.write` messages.  A write
// without credit is the host's business: it answers `busy` the first time and
// ends the plugin as misbehaving when it goes on.  The messages here only say
// what a write and a credit look like.
//
// The timing class of a generator ("rules" or "async") is in the plugin's
// manifest and not in a message; a `generator.pattern` from an instance that
// did not declare "rules" is refused with `not_declared` (section 6.4).
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <cstdint>
#include <string>

namespace vss::plugin
{

/// The most bytes one `generator.write` carries.
inline constexpr std::size_t max_generator_write = std::size_t{64} * 1024;
/// The most credit one `generator.credit` grants: the credit a generator
/// starts with (section 7.4).
inline constexpr std::uint64_t max_generator_credit = std::uint64_t{64} * 1024;
/// The most bytes of a declared pattern.
inline constexpr std::size_t max_pattern_bytes = 1024;
/// The most times a pattern is repeated in one burst.
inline constexpr std::uint64_t max_pattern_burst = 4096;
/// The most times a pattern is repeated in all, when it is not endless.
inline constexpr std::uint64_t max_pattern_repeat = 4'294'967'295u;

/// Starts a generator on a port.  The plugin answers "ok".
struct generator_start_message
{
    static constexpr std::string_view op() noexcept
    {
        return "generator.start";
    }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// The name the hub gives the port the bytes go to: 1 to `max_api_name`
    /// bytes.
    std::string place;
    /// The parameters as one JSON object, already checked.  "{}" for none.
    std::string params = "{}";

    friend bool operator==(const generator_start_message&,
                           const generator_start_message&) = default;
};

/// Bytes for the port, in the blob.  Needs credit.
struct generator_write_message
{
    static constexpr std::string_view op() noexcept
    {
        return "generator.write";
    }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// 1 to `max_generator_write` bytes.  They travel in the blob.
    byte_buffer data;

    friend bool operator==(const generator_write_message&,
                           const generator_write_message&) = default;
};

/// The plugin may send this many more bytes.
struct generator_credit_message
{
    static constexpr std::string_view op() noexcept
    {
        return "generator.credit";
    }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// 1 to `max_generator_credit`.
    std::uint64_t bytes = 0;

    friend bool operator==(const generator_credit_message&,
                           const generator_credit_message&) = default;
};

/// A pattern for the engine to generate, so that it is exact (T0).  The
/// bytes are in the blob, and the host answers "ok" or an "error".
struct generator_pattern_message
{
    static constexpr std::string_view op() noexcept
    {
        return "generator.pattern";
    }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// 1 to `max_pattern_bytes` bytes, in the blob.
    byte_buffer data;
    /// How many times the pattern is sent.  0 is without end.
    std::uint64_t repeat = 1;
    /// The time from the start of one repeat to the start of the next, in
    /// microseconds.  0 is one after the other.
    std::uint64_t period_us = 0;
    /// How many repeats go out together at each period.
    std::uint64_t burst = 1;
    /// The most that the engine moves a period either way, in
    /// microseconds, from its seeded source.  0 is none.
    std::uint64_t jitter = 0;

    friend bool operator==(const generator_pattern_message&,
                           const generator_pattern_message&) = default;
};

/// Stops a generator.  The plugin answers "ok".
struct generator_stop_message
{
    static constexpr std::string_view op() noexcept { return "generator.stop"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;

    friend bool operator==(const generator_stop_message&,
                           const generator_stop_message&) = default;
};

/// The generator has nothing more to send.
struct generator_done_message
{
    static constexpr std::string_view op() noexcept { return "generator.done"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// At most `max_api_text` bytes.  Plain text.
    std::string reason;

    friend bool operator==(const generator_done_message&,
                           const generator_done_message&) = default;
};

template <>
result<generator_start_message>
decode_as<generator_start_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_start_message>(const generator_start_message&,
                                   std::uint64_t);
template <>
result<generator_write_message>
decode_as<generator_write_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_write_message>(const generator_write_message&,
                                   std::uint64_t);
template <>
result<generator_credit_message>
decode_as<generator_credit_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_credit_message>(const generator_credit_message&,
                                    std::uint64_t);
template <>
result<generator_pattern_message>
decode_as<generator_pattern_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_pattern_message>(const generator_pattern_message&,
                                     std::uint64_t);
template <>
result<generator_stop_message>
decode_as<generator_stop_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_stop_message>(const generator_stop_message&, std::uint64_t);
template <>
result<generator_done_message>
decode_as<generator_done_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<generator_done_message>(const generator_done_message&, std::uint64_t);

} // namespace vss::plugin
