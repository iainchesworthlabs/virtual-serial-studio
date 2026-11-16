//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of a fixture: a plugin that simulates a device on an endpoint
// the agent makes.  docs/plugins.md, sections 6.3, 6.4 and 7.5.
//
// A fixture hears what the other side writes (`fixture.data`) and the lines
// it sets (`fixture.line`), and answers with bytes (`fixture.reply`), lines
// (`fixture.line.set`) and faults (`fixture.fault`).  A reply with `after_us`
// is held by the engine and written at that time, so the delay is exact.
//
// Most fixtures send a rule set (`rules.set`) when they are created and again
// when a parameter changes.  The rule set travels in the blob as the JSON
// document of docs/response-timing.md.  Decoding checks only that the blob
// is one well formed JSON object that nests no deeper than
// `vss::rules::max_document_depth`, has no key twice and is valid UTF-8.
// What the rules mean is checked by the host, with `check_rule_set`, which
// runs the rule compiler and says in a `rules.result` whether the set was
// accepted and where the first problem is.  A rule set has at most
// `vss::rules::max_rules` (256) rules and a rule at most
// `vss::rules::max_actions` (16) actions.
//
// `line_fault` and its spellings are here because the modifier messages
// (api_modifier.hpp) use them too.
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vss::plugin
{

/// The most bytes one `fixture.data` carries (section 7.5: up to 8 KiB).
inline constexpr std::size_t max_fixture_data = std::size_t{8} * 1024;
/// The most bytes one `fixture.reply` carries.  It is
/// `vss::rules::max_reply_bytes`, and api_fixture.cpp keeps it so.
inline constexpr std::size_t max_fixture_reply = std::size_t{4} * 1024;
/// The longest path of a problem in a `rules.result`, in bytes.
inline constexpr std::size_t max_rules_path = 256;
/// The longest name of a rule in a `rules.event`.  It is
/// `vss::rules::max_name_bytes`.
inline constexpr std::size_t max_rule_name = 64;
/// The most actions in one rule, so an action of a `rules.event` is a number
/// below it.  It is `vss::rules::max_actions`.
inline constexpr std::size_t max_rule_actions = 16;
/// The most rules in a rule set, so the number of a rule in a `rules.result`
/// is below it.  It is `vss::rules::max_rules`.
inline constexpr std::size_t max_rule_count = 256;

/// A fault that a line can have.  The spellings are those of the rule
/// parser's `line_error` (docs/response-timing.md).
enum class line_fault
{
    parity,
    framing,
    overrun,
    break_signal,
    hang_up,
    disconnect
};

/// The JSON spelling of each `line_fault`.
inline constexpr std::array<std::pair<std::string_view, line_fault>, 6>
    line_fault_names{{{"parity", line_fault::parity},
                      {"framing", line_fault::framing},
                      {"overrun", line_fault::overrun},
                      {"break", line_fault::break_signal},
                      {"hang_up", line_fault::hang_up},
                      {"disconnect", line_fault::disconnect}}};

/// "parity", "framing", "overrun", "break", "hang_up" or "disconnect".
std::string_view to_string(line_fault value) noexcept;

/// Makes a fixture on an endpoint.  The plugin answers "ok".
struct fixture_create_message
{
    static constexpr std::string_view op() noexcept { return "fixture.create"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// The name the hub gives the port the fixture is on: 1 to
    /// `max_api_name` bytes.
    std::string place;
    /// The parameters as one JSON object, already checked.  "{}" for none.
    std::string params = "{}";

    friend bool operator==(const fixture_create_message&,
                           const fixture_create_message&) = default;
};

/// Bytes that arrived at the fixture.  The host gathers up to 8 KiB or 1 ms
/// of arrivals into one message.
struct fixture_data_message
{
    static constexpr std::string_view op() noexcept { return "fixture.data"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// The engine's clock, in nanoseconds: 0 to `max_api_number`.
    std::uint64_t ts_ns = 0;
    /// 1 to `max_fixture_data` bytes.  They travel in the blob.
    byte_buffer data;

    friend bool operator==(const fixture_data_message&,
                           const fixture_data_message&) = default;
};

/// The lines the program set have changed.
struct fixture_line_message
{
    static constexpr std::string_view op() noexcept { return "fixture.line"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    std::uint64_t ts_ns = 0;
    bool dtr = false;
    bool rts = false;

    friend bool operator==(const fixture_line_message&,
                           const fixture_line_message&) = default;
};

/// Bytes for the fixture to write.  With `after_us` the engine holds them
/// and writes them at that time.
struct fixture_reply_message
{
    static constexpr std::string_view op() noexcept { return "fixture.reply"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// 1 to `max_fixture_reply` bytes, in the blob.
    byte_buffer data;
    /// The delay in microseconds, 0 to `max_api_delay_us`.  Absent is now.
    std::optional<std::uint64_t> after_us;

    friend bool operator==(const fixture_reply_message&,
                           const fixture_reply_message&) = default;
};

/// Sets the lines of the fixture.  At least one of `dtr` and `rts` is given.
struct fixture_line_set_message
{
    static constexpr std::string_view op() noexcept
    {
        return "fixture.line.set";
    }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    std::optional<bool> dtr;
    std::optional<bool> rts;
    /// 0 to `max_api_delay_us`.  Absent is now.
    std::optional<std::uint64_t> after_us;

    friend bool operator==(const fixture_line_set_message&,
                           const fixture_line_set_message&) = default;
};

/// Raises a fault on the fixture's endpoint.
struct fixture_fault_message
{
    static constexpr std::string_view op() noexcept { return "fixture.fault"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    line_fault fault = line_fault::parity;

    friend bool operator==(const fixture_fault_message&,
                           const fixture_fault_message&) = default;
};

/// Replaces the rule set of an instance, all at once.  The rule set is the
/// blob: a UTF-8 JSON document of 1 to `max_api_blob` bytes.  The host
/// answers with a `rules.result`.
struct rules_set_message
{
    static constexpr std::string_view op() noexcept { return "rules.set"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// The bytes of the document.
    byte_buffer data;

    friend bool operator==(const rules_set_message&,
                           const rules_set_message&) = default;
};

/// The host's answer to a `rules.set`.  The design does not name it.
struct rules_result_message
{
    static constexpr std::string_view op() noexcept { return "rules.result"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::answer; }

    bool accepted = false;
    /// Only when not accepted: the zero-based number of the rule the first
    /// problem is in, 0 to `max_rule_count` less one.  Absent when the
    /// problem is not in a rule.
    std::optional<std::uint64_t> rule;
    /// Only when not accepted: where the first problem is, as
    /// `rules[2].do[0].delay`.  0 to `max_rules_path` bytes.
    std::string path;
    /// Only when not accepted: what is wrong.  1 to `max_api_text` bytes.
    std::string reason;

    friend bool operator==(const rules_result_message&,
                           const rules_result_message&) = default;
};

/// A rule of the set fired.  `rule` is the name of the rule, and `action` is
/// the kind of the action that was done, as `docs/response-timing.md` names
/// it (`reply`, `lines`, `line_error`, `set_state`, `set_slot`, `mark`,
/// `drop`, `corrupt`, `truncate`, `duplicate`, `hold`, `pass`, `ask`).
struct rules_event_message
{
    static constexpr std::string_view op() noexcept { return "rules.event"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    std::uint64_t ts_ns = 0;
    /// 1 to `max_rule_name` bytes.  Plain text.
    std::string rule;
    /// 1 to `max_rule_name` bytes.  Plain text.
    std::string action;

    friend bool operator==(const rules_event_message&,
                           const rules_event_message&) = default;
};

/// What the host answers to a `rules.set`.  Runs the rule compiler
/// (`vss::rules::compile_rule_set`) on the text of the blob and says whether
/// the set was accepted, and when not, where and why.  The text of the
/// problem is cut to the limits of the message and has only plain ASCII.
/// This is for the host, after `decode_as<rules_set_message>` has accepted
/// the message.
rules_result_message check_rule_set(const rules_set_message& message);

template <>
result<fixture_create_message>
decode_as<fixture_create_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<fixture_create_message>(const fixture_create_message&, std::uint64_t);
template <>
result<fixture_data_message> decode_as<fixture_data_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<fixture_data_message>(const fixture_data_message&,
                                                    std::uint64_t);
template <>
result<fixture_line_message> decode_as<fixture_line_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<fixture_line_message>(const fixture_line_message&,
                                                    std::uint64_t);
template <>
result<fixture_reply_message>
decode_as<fixture_reply_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<fixture_reply_message>(const fixture_reply_message&, std::uint64_t);
template <>
result<fixture_line_set_message>
decode_as<fixture_line_set_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<fixture_line_set_message>(const fixture_line_set_message&,
                                    std::uint64_t);
template <>
result<fixture_fault_message>
decode_as<fixture_fault_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<fixture_fault_message>(const fixture_fault_message&, std::uint64_t);
template <>
result<rules_set_message> decode_as<rules_set_message>(const api_message&,
                                                       std::string*);
template <>
result<api_message> encode_as<rules_set_message>(const rules_set_message&,
                                                 std::uint64_t);
template <>
result<rules_result_message> decode_as<rules_result_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<rules_result_message>(const rules_result_message&,
                                                    std::uint64_t);
template <>
result<rules_event_message> decode_as<rules_event_message>(const api_message&,
                                                           std::string*);
template <>
result<api_message> encode_as<rules_event_message>(const rules_event_message&,
                                                   std::uint64_t);

} // namespace vss::plugin
