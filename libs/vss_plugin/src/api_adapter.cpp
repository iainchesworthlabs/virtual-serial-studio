//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_adapter.hpp>

#include "api_codec.hpp"

#include <utility>

namespace vss::plugin
{

namespace
{

using namespace detail;

// The "data" of agent.subscribe is a flag in the header, not bytes in the blob.
static_assert(has_bytes<agent_call_message>);
static_assert(has_bytes<agent_result_message>);
static_assert(!has_bytes<agent_subscribe_message>);

/// A list of names, each 1 to `max_api_name` bytes of plain text.  Empty when
/// the member is absent or wrong.
std::vector<std::string> names_of(reading& state, const json::object& object,
                                  std::string_view where, std::string_view key,
                                  std::size_t most)
{
    std::vector<std::string> out;
    const json::array* const list =
        list_of(state, object, where, key, most, false);
    if (list == nullptr)
        return out;
    const std::string list_name = member(where, key);
    out.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const std::string at = entry(list_name, i);
        const json::value& item = (*list)[i];
        if (!item.is_string())
        {
            state.fail(at, "must be a string");
            return {};
        }
        const std::string_view text = view_of(item.get_string());
        if (text.empty() || text.size() > max_api_name)
        {
            state.fail(at, "must be 1 to " + std::to_string(max_api_name) +
                               " bytes");
            return {};
        }
        if (!clean_text(text))
        {
            state.fail(at, "must be plain text, with no control character or "
                           "direction mark");
            return {};
        }
        out.emplace_back(text);
    }
    return out;
}

json::array names_value(const std::vector<std::string>& names)
{
    json::array out;
    out.reserve(names.size());
    for (const std::string& name : names)
        out.emplace_back(text_value(name));
    return out;
}

void read_call(reading& state, const json::object& object,
               agent_call_message& out)
{
    constexpr auto where = agent_call_message::op;
    out.agent_op =
        token_of(state, object, where, "agent_op", max_agent_op, true);
    out.args = object_text_of(state, object, where, "args", false);
    blob_between(state, where, out.data, 0, max_api_blob);
}

bool write_call(json::object& header, const agent_call_message& body)
{
    header.emplace("agent_op", text_value(body.agent_op));
    return put_object_text(header, "args", body.args);
}

void read_result(reading& state, const json::object& object,
                 agent_result_message& out)
{
    constexpr auto where = agent_result_message::op;
    out.result = object_text_of(state, object, where, "result", false);
    blob_between(state, where, out.data, 0, max_api_blob);
}

bool write_result(json::object& header, const agent_result_message& body)
{
    return put_object_text(header, "result", body.result);
}

void read_subscribe(reading& state, const json::object& object,
                    agent_subscribe_message& out)
{
    constexpr auto where = agent_subscribe_message::op;
    out.data = bool_of(state, object, where, "data", false).value_or(false);
    out.places = names_of(state, object, where, "places", max_subscribe_places);
}

bool write_subscribe(json::object& header, const agent_subscribe_message& body)
{
    header.emplace("data", body.data);
    if (!body.places.empty())
        header.emplace("places", names_value(body.places));
    return true;
}

void read_state(reading& state, const json::object& object,
                adapter_state_message& out)
{
    constexpr auto where = adapter_state_message::op;
    out.listening =
        bool_of(state, object, where, "listening", true).value_or(false);
    if (out.listening)
    {
        out.address = text_of(state, object, where, "address",
                              max_adapter_address, true, true);
    }
    else if (object.if_contains("address") != nullptr)
    {
        state.fail(member(where, "address"),
                   "must not be given when listening is false");
    }
    out.protocol = text_of(state, object, where, "protocol",
                           max_adapter_protocol, true, true);
}

bool write_state(json::object& header, const adapter_state_message& body)
{
    header.emplace("listening", body.listening);
    if (body.listening)
        header.emplace("address", text_value(body.address));
    else if (!body.address.empty())
        return false;
    header.emplace("protocol", text_value(body.protocol));
    return true;
}

} // namespace

api_message make_plugin_unavailable(std::uint64_t re)
{
    return make_error(re, api_errc::plugin_unavailable,
                      "The plugin is not running.");
}

template <>
result<agent_call_message>
decode_as<agent_call_message>(const api_message& message, std::string* explain)
{
    return decode_with<agent_call_message>(message, explain, read_call);
}

template <>
result<api_message>
encode_as<agent_call_message>(const agent_call_message& body,
                              std::uint64_t number)
{
    return encode_with(body, number, write_call);
}

template <>
result<agent_result_message>
decode_as<agent_result_message>(const api_message& message,
                                std::string* explain)
{
    return decode_with<agent_result_message>(message, explain, read_result);
}

template <>
result<api_message>
encode_as<agent_result_message>(const agent_result_message& body,
                                std::uint64_t number)
{
    return encode_with(body, number, write_result);
}

template <>
result<agent_subscribe_message>
decode_as<agent_subscribe_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<agent_subscribe_message>(message, explain,
                                                read_subscribe);
}

template <>
result<api_message>
encode_as<agent_subscribe_message>(const agent_subscribe_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_subscribe);
}

template <>
result<adapter_state_message>
decode_as<adapter_state_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<adapter_state_message>(message, explain, read_state);
}

template <>
result<api_message>
encode_as<adapter_state_message>(const adapter_state_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_state);
}

} // namespace vss::plugin
