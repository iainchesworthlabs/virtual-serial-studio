//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_fixture.hpp>

#include <vss/rules/model.hpp>
#include <vss/rules/parse.hpp>

#include "api_codec.hpp"

#include <algorithm>
#include <vector>

namespace vss::plugin
{

static_assert(max_fixture_reply == rules::max_reply_bytes);
static_assert(max_rule_name == rules::max_name_bytes);
static_assert(max_rule_actions == rules::max_actions);
static_assert(max_rule_count == rules::max_rules);

std::string_view to_string(line_fault value) noexcept
{
    return detail::name_of(line_fault_names, value);
}

namespace
{

using namespace detail;

std::uint64_t instance_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "instance", 1, max_api_number, true)
        .value_or(0);
}

std::uint64_t time_of(reading& state, const json::object& object,
                      std::string_view where)
{
    return number_of(state, object, where, "ts_ns", 0, max_api_number, true)
        .value_or(0);
}

// -- fixture.create ------------------------------------------------------------

void read_create(reading& state, const json::object& object,
                 fixture_create_message& out)
{
    constexpr auto where = fixture_create_message::op;
    out.instance = instance_of(state, object, where);
    out.endpoint =
        number_of(state, object, where, "endpoint", 1, max_api_number, true)
            .value_or(0);
    out.params = object_text_of(state, object, where, "params", false);
}

bool write_create(json::object& header, const fixture_create_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("endpoint", body.endpoint);
    return put_object_text(header, "params", body.params);
}

// -- fixture.data --------------------------------------------------------------

void read_data(reading& state, const json::object& object,
               fixture_data_message& out)
{
    constexpr auto where = fixture_data_message::op;
    out.instance = instance_of(state, object, where);
    out.ts_ns = time_of(state, object, where);
    blob_between(state, where, out.data, 1, max_fixture_data);
}

bool write_data(json::object& header, const fixture_data_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("ts_ns", body.ts_ns);
    return true;
}

// -- fixture.line --------------------------------------------------------------

void read_line(reading& state, const json::object& object,
               fixture_line_message& out)
{
    constexpr auto where = fixture_line_message::op;
    out.instance = instance_of(state, object, where);
    out.ts_ns = time_of(state, object, where);
    out.dtr = bool_of(state, object, where, "dtr", true).value_or(false);
    out.rts = bool_of(state, object, where, "rts", true).value_or(false);
}

bool write_line(json::object& header, const fixture_line_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("ts_ns", body.ts_ns);
    header.emplace("dtr", body.dtr);
    header.emplace("rts", body.rts);
    return true;
}

// -- fixture.reply -------------------------------------------------------------

void read_reply(reading& state, const json::object& object,
                fixture_reply_message& out)
{
    constexpr auto where = fixture_reply_message::op;
    out.instance = instance_of(state, object, where);
    blob_between(state, where, out.data, 1, max_fixture_reply);
    out.after_us = number_of(state, object, where, "after_us", 0,
                             max_api_delay_us, false);
}

bool write_reply(json::object& header, const fixture_reply_message& body)
{
    header.emplace("instance", body.instance);
    if (body.after_us.has_value())
        header.emplace("after_us", *body.after_us);
    return true;
}

// -- fixture.line.set ----------------------------------------------------------

void read_line_set(reading& state, const json::object& object,
                   fixture_line_set_message& out)
{
    constexpr auto where = fixture_line_set_message::op;
    out.instance = instance_of(state, object, where);
    out.dtr = bool_of(state, object, where, "dtr", false);
    out.rts = bool_of(state, object, where, "rts", false);
    out.after_us = number_of(state, object, where, "after_us", 0,
                             max_api_delay_us, false);
    if (!state.failed() && !out.dtr.has_value() && !out.rts.has_value())
        state.fail(where, "needs dtr or rts, or both");
}

bool write_line_set(json::object& header, const fixture_line_set_message& body)
{
    header.emplace("instance", body.instance);
    if (body.dtr.has_value())
        header.emplace("dtr", *body.dtr);
    if (body.rts.has_value())
        header.emplace("rts", *body.rts);
    if (body.after_us.has_value())
        header.emplace("after_us", *body.after_us);
    return true;
}

// -- fixture.fault -------------------------------------------------------------

void read_fault(reading& state, const json::object& object,
                fixture_fault_message& out)
{
    constexpr auto where = fixture_fault_message::op;
    out.instance = instance_of(state, object, where);
    const auto kind = choice_of(
        state, object, where, "fault_kind", line_fault_names,
        "\"parity\", \"framing\", \"overrun\", \"break\", \"hang_up\" or "
        "\"disconnect\"",
        true);
    if (kind.has_value())
        out.fault_kind = *kind;
}

bool write_fault(json::object& header, const fixture_fault_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("fault_kind", text_value(to_string(body.fault_kind)));
    return true;
}

// -- rules.set -----------------------------------------------------------------

void read_rules_set(reading& state, const json::object& object,
                    rules_set_message& out)
{
    constexpr auto where = rules_set_message::op;
    out.instance = instance_of(state, object, where);
    blob_between(state, where, out.data, 1, max_api_blob);
    if (state.failed())
        return;
    std::string text;
    text.reserve(out.data.size());
    for (const auto byte : out.data)
        text.push_back(static_cast<char>(byte));
    std::string what;
    const std::optional<json::value> document =
        parse_strict(text, rules::max_document_depth, what);
    if (!document.has_value())
        state.fail(member(where, "data"), what);
    else if (!document->is_object())
        state.fail(member(where, "data"), "must be a JSON object");
}

bool write_rules_set(json::object& header, const rules_set_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

// -- rules.result --------------------------------------------------------------

void read_rules_result(reading& state, const json::object& object,
                       rules_result_message& out)
{
    constexpr auto where = rules_result_message::op;
    out.accepted =
        bool_of(state, object, where, "accepted", true).value_or(false);
    if (state.failed())
        return;
    if (out.accepted)
    {
        for (const std::string_view key : {"rule"sv, "path"sv, "reason"sv})
        {
            if (object.contains(key))
                state.fail(member(where, key),
                           "must be absent when the set is accepted");
        }
        return;
    }
    out.rule = number_of(state, object, where, "rule", 0, max_rule_count - 1,
                         false);
    out.path = text_of(state, object, where, "path", max_rules_path, false);
    out.reason =
        text_of(state, object, where, "reason", max_api_text, true, true);
}

bool write_rules_result(json::object& header, const rules_result_message& body)
{
    header.emplace("accepted", body.accepted);
    if (body.accepted)
        return !body.rule.has_value() && body.path.empty() &&
               body.reason.empty();
    if (body.rule.has_value())
        header.emplace("rule", *body.rule);
    put_optional(header, "path", body.path);
    header.emplace("reason", text_value(body.reason));
    return true;
}

// -- rules.event ---------------------------------------------------------------

void read_rules_event(reading& state, const json::object& object,
                      rules_event_message& out)
{
    constexpr auto where = rules_event_message::op;
    out.instance = instance_of(state, object, where);
    out.ts_ns = time_of(state, object, where);
    out.rule = text_of(state, object, where, "rule", max_rule_name, true, true);
    out.action = number_of(state, object, where, "action", 0,
                           max_rule_actions - 1, true)
                     .value_or(0);
}

bool write_rules_event(json::object& header, const rules_event_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("ts_ns", body.ts_ns);
    header.emplace("rule", text_value(body.rule));
    header.emplace("action", body.action);
    return true;
}

// -- check_rule_set ------------------------------------------------------------

/// Plain ASCII, at most `most` bytes: anything else becomes a question mark.
std::string plain_ascii(std::string_view text, std::size_t most)
{
    std::string out;
    out.reserve(std::min(text.size(), most));
    for (const char c : text)
    {
        if (out.size() == most)
            break;
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(byte >= 0x20 && byte < 0x7f ? c : '?');
    }
    return out;
}

/// The number in a path that starts `rules[N]`.
std::optional<std::uint64_t> rule_of_path(std::string_view path)
{
    constexpr std::string_view start = "rules[";
    if (!path.starts_with(start))
        return std::nullopt;
    std::uint64_t number = 0;
    std::size_t at = start.size();
    const std::size_t first = at;
    while (at < path.size() && path[at] >= '0' && path[at] <= '9')
    {
        number = number * 10 + static_cast<std::uint64_t>(path[at] - '0');
        if (number >= max_rule_count)
            return std::nullopt;
        ++at;
    }
    if (at == first || at >= path.size() || path[at] != ']')
        return std::nullopt;
    return number;
}

} // namespace

rules_result_message check_rule_set(const rules_set_message& message)
{
    rules_result_message out;
    try
    {
        std::string text;
        text.reserve(message.data.size());
        for (const auto byte : message.data)
            text.push_back(static_cast<char>(byte));
        std::vector<rules::problem> problems;
        const auto compiled = rules::compile_rule_set(text, &problems);
        if (compiled.has_value())
        {
            out.accepted = true;
            return out;
        }
        out.accepted = false;
        out.reason = "the rule set is not accepted";
        if (!problems.empty())
        {
            const rules::problem& first = problems.front();
            out.path = plain_ascii(first.path, max_rules_path);
            out.rule = rule_of_path(first.path);
            std::string reason = plain_ascii(first.message, max_api_text);
            if (!reason.empty())
                out.reason = std::move(reason);
        }
    }
    catch (...)
    {
        out = rules_result_message{};
        out.reason = "the rule set could not be checked";
    }
    return out;
}

template <>
result<fixture_create_message>
decode_as<fixture_create_message>(const api_message& message,
                                  std::string* explain)
{
    return detail::decode_with<fixture_create_message>(message, explain,
                                                       read_create);
}

template <>
result<api_message>
encode_as<fixture_create_message>(const fixture_create_message& body,
                                  std::uint64_t number)
{
    return detail::encode_with(body, number, write_create);
}

template <>
result<fixture_data_message>
decode_as<fixture_data_message>(const api_message& message,
                                std::string* explain)
{
    return detail::decode_with<fixture_data_message>(message, explain,
                                                     read_data);
}

template <>
result<api_message>
encode_as<fixture_data_message>(const fixture_data_message& body,
                                std::uint64_t number)
{
    return detail::encode_with(body, number, write_data);
}

template <>
result<fixture_line_message>
decode_as<fixture_line_message>(const api_message& message,
                                std::string* explain)
{
    return detail::decode_with<fixture_line_message>(message, explain,
                                                     read_line);
}

template <>
result<api_message>
encode_as<fixture_line_message>(const fixture_line_message& body,
                                std::uint64_t number)
{
    return detail::encode_with(body, number, write_line);
}

template <>
result<fixture_reply_message>
decode_as<fixture_reply_message>(const api_message& message,
                                 std::string* explain)
{
    return detail::decode_with<fixture_reply_message>(message, explain,
                                                      read_reply);
}

template <>
result<api_message>
encode_as<fixture_reply_message>(const fixture_reply_message& body,
                                 std::uint64_t number)
{
    return detail::encode_with(body, number, write_reply);
}

template <>
result<fixture_line_set_message>
decode_as<fixture_line_set_message>(const api_message& message,
                                    std::string* explain)
{
    return detail::decode_with<fixture_line_set_message>(message, explain,
                                                         read_line_set);
}

template <>
result<api_message>
encode_as<fixture_line_set_message>(const fixture_line_set_message& body,
                                    std::uint64_t number)
{
    return detail::encode_with(body, number, write_line_set);
}

template <>
result<fixture_fault_message>
decode_as<fixture_fault_message>(const api_message& message,
                                 std::string* explain)
{
    return detail::decode_with<fixture_fault_message>(message, explain,
                                                      read_fault);
}

template <>
result<api_message>
encode_as<fixture_fault_message>(const fixture_fault_message& body,
                                 std::uint64_t number)
{
    return detail::encode_with(body, number, write_fault);
}

template <>
result<rules_set_message>
decode_as<rules_set_message>(const api_message& message, std::string* explain)
{
    return detail::decode_with<rules_set_message>(message, explain,
                                                  read_rules_set);
}

template <>
result<api_message>
encode_as<rules_set_message>(const rules_set_message& body,
                             std::uint64_t number)
{
    return detail::encode_with(body, number, write_rules_set);
}

template <>
result<rules_result_message>
decode_as<rules_result_message>(const api_message& message,
                                std::string* explain)
{
    return detail::decode_with<rules_result_message>(message, explain,
                                                     read_rules_result);
}

template <>
result<api_message>
encode_as<rules_result_message>(const rules_result_message& body,
                                std::uint64_t number)
{
    return detail::encode_with(body, number, write_rules_result);
}

template <>
result<rules_event_message>
decode_as<rules_event_message>(const api_message& message,
                               std::string* explain)
{
    return detail::decode_with<rules_event_message>(message, explain,
                                                    read_rules_event);
}

template <>
result<api_message>
encode_as<rules_event_message>(const rules_event_message& body,
                               std::uint64_t number)
{
    return detail::encode_with(body, number, write_rules_event);
}

} // namespace vss::plugin
