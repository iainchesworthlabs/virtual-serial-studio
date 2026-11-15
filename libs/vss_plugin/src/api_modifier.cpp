//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_modifier.hpp>

#include "api_codec.hpp"

namespace vss::plugin
{

namespace
{

using namespace detail;

constexpr std::array<std::pair<std::string_view, modifier_direction>, 2>
    direction_names{{{"a_to_b", modifier_direction::a_to_b},
                     {"b_to_a", modifier_direction::b_to_a}}};

constexpr std::array<std::pair<std::string_view, timeout_action>, 2>
    timeout_names{{{"fail_open", timeout_action::fail_open},
                   {"fail_closed", timeout_action::fail_closed}}};

constexpr std::array<std::pair<std::string_view, modifier_action_kind>, 8>
    action_names{{{"pass", modifier_action_kind::pass},
                  {"drop", modifier_action_kind::drop},
                  {"replace", modifier_action_kind::replace},
                  {"delay_us", modifier_action_kind::delay_us},
                  {"split", modifier_action_kind::split},
                  {"insert_before", modifier_action_kind::insert_before},
                  {"insert_after", modifier_action_kind::insert_after},
                  {"fault", modifier_action_kind::fault}}};

std::uint64_t instance_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "instance", 1, max_api_number, true)
        .value_or(0);
}

std::uint64_t sequence_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "seq", 1, max_api_number, true)
        .value_or(0);
}

// -- modifier.create -----------------------------------------------------------

void read_create(reading& state, const json::object& object,
                 modifier_create_message& out)
{
    constexpr auto where = modifier_create_message::op;
    out.instance = instance_of(state, object, where);
    out.link = number_of(state, object, where, "link", 1, max_api_number, true)
                   .value_or(0);
    const auto direction =
        choice_of(state, object, where, "direction", direction_names,
                  "\"a_to_b\" or \"b_to_a\"", true);
    if (direction.has_value())
        out.direction = *direction;
    out.params = object_text_of(state, object, where, "params", false);
    out.budget_us = number_of(state, object, where, "budget_us", 1,
                              max_modifier_budget_us, true)
                        .value_or(0);
    out.max_batch_bytes = number_of(state, object, where, "max_batch_bytes", 1,
                                    max_modifier_batch, true)
                              .value_or(0);
    out.max_batch_us = number_of(state, object, where, "max_batch_us", 1,
                                 max_modifier_batch_us, true)
                           .value_or(0);
    const auto action =
        choice_of(state, object, where, "on_timeout", timeout_names,
                  "\"fail_open\" or \"fail_closed\"", true);
    if (action.has_value())
        out.on_timeout = *action;
    const auto flush = number_of(state, object, where, "flush_on", 0, 255, false);
    if (flush.has_value())
        out.flush_on = static_cast<std::uint8_t>(*flush);
}

bool write_create(json::object& header, const modifier_create_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("link", body.link);
    header.emplace("direction", text_value(to_string(body.direction)));
    if (!put_object_text(header, "params", body.params))
        return false;
    header.emplace("budget_us", body.budget_us);
    header.emplace("max_batch_bytes", body.max_batch_bytes);
    header.emplace("max_batch_us", body.max_batch_us);
    header.emplace("on_timeout", text_value(to_string(body.on_timeout)));
    if (body.flush_on.has_value())
        header.emplace("flush_on", static_cast<std::uint64_t>(*body.flush_on));
    return true;
}

// -- modifier.batch ------------------------------------------------------------

void read_batch(reading& state, const json::object& object,
                modifier_batch_message& out)
{
    constexpr auto where = modifier_batch_message::op;
    out.instance = instance_of(state, object, where);
    out.seq = sequence_of(state, object, where);
    out.ts_ns = number_of(state, object, where, "ts_ns", 0, max_api_number, true)
                    .value_or(0);
    blob_between(state, where, out.data, 1, max_modifier_batch);
    // The length of the list is checked before any entry is read.
    const json::array* const list =
        list_of(state, object, where, "chunks", max_modifier_chunks, true);
    if (state.failed() || list == nullptr)
        return;
    const std::string chunks = member(where, "chunks");
    if (list->empty())
    {
        state.fail(chunks, "must have at least 1 entry");
        return;
    }
    out.chunks.reserve(list->size());
    std::uint64_t end = 0;
    std::uint64_t time = out.ts_ns;
    for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
    {
        const std::string here = entry(chunks, i);
        const json::object* const item = entry_object(state, (*list)[i], here);
        if (item == nullptr)
            return;
        modifier_chunk chunk;
        chunk.offset = number_of(state, *item, here, "offset", 0,
                                 max_modifier_batch, true)
                           .value_or(0);
        chunk.length = number_of(state, *item, here, "length", 1,
                                 max_modifier_batch, true)
                           .value_or(0);
        chunk.ts_ns =
            number_of(state, *item, here, "ts_ns", 0, max_api_number, true)
                .value_or(0);
        if (state.failed())
            return;
        if (chunk.offset > out.data.size() ||
            chunk.length > out.data.size() - chunk.offset)
            state.fail(here, "is not inside the blob");
        else if (chunk.offset < end)
            state.fail(here, "is not after the chunk before it");
        else if (i == 0 && chunk.ts_ns != out.ts_ns)
            state.fail(member(here, "ts_ns"),
                       "of the first chunk must be the ts_ns of the batch");
        else if (chunk.ts_ns < time)
            state.fail(member(here, "ts_ns"),
                       "must not be before the chunk before it");
        if (state.failed())
            return;
        end = chunk.offset + chunk.length;
        time = chunk.ts_ns;
        out.chunks.push_back(chunk);
    }
}

bool write_batch(json::object& header, const modifier_batch_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("seq", body.seq);
    header.emplace("ts_ns", body.ts_ns);
    // A list that is too long is not written: the reader would refuse it.
    if (body.chunks.size() > max_modifier_chunks)
        return false;
    json::array list;
    list.reserve(body.chunks.size());
    for (const modifier_chunk& chunk : body.chunks)
    {
        json::object item;
        item.emplace("offset", chunk.offset);
        item.emplace("length", chunk.length);
        item.emplace("ts_ns", chunk.ts_ns);
        list.emplace_back(std::move(item));
    }
    header.emplace("chunks", std::move(list));
    return true;
}

// -- modifier.result -----------------------------------------------------------

void read_action(reading& state, const json::object& object,
                 const std::string& where, std::size_t blob_size,
                 modifier_action& out)
{
    const auto kind = choice_of(
        state, object, where, "action", action_names,
        "\"pass\", \"drop\", \"replace\", \"delay_us\", \"split\", "
        "\"insert_before\", \"insert_after\" or \"fault\"",
        true);
    if (!kind.has_value())
        return;
    out.kind = *kind;
    const bool ranged = out.kind == modifier_action_kind::replace ||
                        out.kind == modifier_action_kind::insert_before ||
                        out.kind == modifier_action_kind::insert_after;
    if (ranged)
    {
        const std::uint64_t least =
            out.kind == modifier_action_kind::replace ? 0 : 1;
        out.offset =
            number_of(state, object, where, "offset", 0, max_api_blob, true)
                .value_or(0);
        out.length =
            number_of(state, object, where, "length", least, max_api_blob, true)
                .value_or(0);
        if (!state.failed() &&
            (out.offset > blob_size || out.length > blob_size - out.offset))
            state.fail(where, "has a range that is not inside the result blob");
    }
    else if (out.kind == modifier_action_kind::delay_us)
    {
        out.us = number_of(state, object, where, "us", 1, max_api_delay_us, true)
                     .value_or(0);
    }
    else if (out.kind == modifier_action_kind::split)
    {
        out.parts = number_of(state, object, where, "parts", 2,
                              max_modifier_parts, true)
                        .value_or(0);
        out.gap_us = number_of(state, object, where, "gap_us", 0,
                               max_api_delay_us, true)
                         .value_or(0);
    }
    else if (out.kind == modifier_action_kind::fault)
    {
        const auto fault = choice_of(
            state, object, where, "fault", line_fault_names,
            "\"parity\", \"framing\", \"overrun\", \"break\", \"hang_up\" or "
            "\"disconnect\"",
            true);
        if (fault.has_value())
            out.fault = *fault;
    }
}

void read_result(reading& state, const json::object& object,
                 modifier_result_message& out)
{
    constexpr auto where = modifier_result_message::op;
    out.instance = instance_of(state, object, where);
    out.seq = sequence_of(state, object, where);
    blob_between(state, where, out.data, 0, max_api_blob);
    const json::array* const list =
        list_of(state, object, where, "actions", max_modifier_chunks, true);
    if (state.failed() || list == nullptr)
        return;
    const std::string actions = member(where, "actions");
    if (list->empty())
    {
        state.fail(actions, "must have at least 1 entry");
        return;
    }
    out.actions.reserve(list->size());
    for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
    {
        const std::string here = entry(actions, i);
        const json::object* const item = entry_object(state, (*list)[i], here);
        if (item == nullptr)
            return;
        modifier_action action;
        read_action(state, *item, here, out.data.size(), action);
        if (state.failed())
            return;
        out.actions.push_back(action);
    }
}

bool write_result(json::object& header, const modifier_result_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("seq", body.seq);
    if (body.actions.size() > max_modifier_chunks)
        return false;
    json::array list;
    list.reserve(body.actions.size());
    for (const modifier_action& action : body.actions)
    {
        json::object item;
        item.emplace("action", text_value(to_string(action.kind)));
        switch (action.kind)
        {
        case modifier_action_kind::pass:
        case modifier_action_kind::drop:
            break;
        case modifier_action_kind::replace:
        case modifier_action_kind::insert_before:
        case modifier_action_kind::insert_after:
            item.emplace("offset", action.offset);
            item.emplace("length", action.length);
            break;
        case modifier_action_kind::delay_us:
            item.emplace("us", action.us);
            break;
        case modifier_action_kind::split:
            item.emplace("parts", action.parts);
            item.emplace("gap_us", action.gap_us);
            break;
        case modifier_action_kind::fault:
            item.emplace("fault", text_value(to_string(action.fault)));
            break;
        }
        list.emplace_back(std::move(item));
    }
    header.emplace("actions", std::move(list));
    return true;
}

// -- modifier.stop -------------------------------------------------------------

void read_stop(reading& state, const json::object& object,
               modifier_stop_message& out)
{
    out.instance = instance_of(state, object, modifier_stop_message::op);
}

bool write_stop(json::object& header, const modifier_stop_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

} // namespace

std::string_view to_string(modifier_direction value) noexcept
{
    return name_of(direction_names, value);
}

std::string_view to_string(timeout_action value) noexcept
{
    return name_of(timeout_names, value);
}

std::string_view to_string(modifier_action_kind value) noexcept
{
    return name_of(action_names, value);
}

bool modifier_result_within_bounds(std::size_t batch_bytes,
                                   const modifier_result_message& result)
    noexcept
{
    // Four times the batch and a little more, without overflow.
    const std::size_t size = result.data.size();
    const std::size_t excess =
        size > modifier_result_extra ? size - modifier_result_extra : 0;
    if (batch_bytes <= static_cast<std::size_t>(-1) / 4 &&
        excess > batch_bytes * 4)
        return false;

    // The delays and the gaps of the splits add up to at most a minute.  Each
    // term is checked first, so that nothing wraps.
    std::uint64_t total = 0;
    for (const modifier_action& action : result.actions)
    {
        std::uint64_t term = 0;
        if (action.kind == modifier_action_kind::delay_us)
        {
            term = action.us;
        }
        else if (action.kind == modifier_action_kind::split &&
                 action.parts > 1)
        {
            const std::uint64_t gaps = action.parts - 1;
            if (action.gap_us > max_api_delay_us / gaps)
                return false;
            term = action.gap_us * gaps;
        }
        if (term > max_api_delay_us || total > max_api_delay_us - term)
            return false;
        total += term;
    }
    return true;
}

template <>
result<modifier_create_message>
decode_as<modifier_create_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<modifier_create_message>(message, explain, read_create);
}

template <>
result<api_message>
encode_as<modifier_create_message>(const modifier_create_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_create);
}

template <>
result<modifier_batch_message>
decode_as<modifier_batch_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<modifier_batch_message>(message, explain, read_batch);
}

template <>
result<api_message>
encode_as<modifier_batch_message>(const modifier_batch_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_batch);
}

template <>
result<modifier_result_message>
decode_as<modifier_result_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<modifier_result_message>(message, explain, read_result);
}

template <>
result<api_message>
encode_as<modifier_result_message>(const modifier_result_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_result);
}

template <>
result<modifier_stop_message>
decode_as<modifier_stop_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<modifier_stop_message>(message, explain, read_stop);
}

template <>
result<api_message>
encode_as<modifier_stop_message>(const modifier_stop_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_stop);
}

} // namespace vss::plugin
