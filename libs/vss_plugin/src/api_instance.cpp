//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_instance.hpp>

#include "api_codec.hpp"

#include <array>
#include <utility>

namespace vss::plugin
{

namespace
{

using namespace detail;

constexpr std::array kind_names{
    std::pair{"generator"sv, extension_kind::generator},
    std::pair{"fixture"sv, extension_kind::fixture},
    std::pair{"modifier"sv, extension_kind::modifier},
    std::pair{"adapter"sv, extension_kind::adapter},
    std::pair{"decoder"sv, extension_kind::decoder}};

std::uint64_t instance_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "instance", 1, max_api_number, true)
        .value_or(0);
}

void read_create(reading& state, const json::object& object,
                 instance_create_message& out)
{
    constexpr auto where = instance_create_message::op();
    out.instance = instance_of(state, object, where);
    out.kind =
        choice_of(state, object, where, "kind", kind_names,
                  "generator, fixture, modifier, adapter or decoder", true)
            .value_or(extension_kind::generator);
    out.type = token_of(state, object, where, "type", max_api_id, true);
    out.params = object_text_of(state, object, where, "params", false);
}

bool write_create(json::object& header, const instance_create_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("kind", text_value(name_of(kind_names, body.kind)));
    header.emplace("type", text_value(body.type));
    return put_object_text(header, "params", body.params);
}

void read_stop(reading& state, const json::object& object,
               instance_stop_message& out)
{
    out.instance = instance_of(state, object, instance_stop_message::op());
}

bool write_stop(json::object& header, const instance_stop_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

void read_changed(reading& state, const json::object& object,
                  params_changed_message& out)
{
    constexpr auto where = params_changed_message::op();
    out.instance = instance_of(state, object, where);
    out.params = object_text_of(state, object, where, "params", true);
}

bool write_changed(json::object& header, const params_changed_message& body)
{
    header.emplace("instance", body.instance);
    return put_object_text(header, "params", body.params);
}

} // namespace

template <>
result<instance_create_message>
decode_as<instance_create_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<instance_create_message>(message, explain, read_create);
}

template <>
result<api_message>
encode_as<instance_create_message>(const instance_create_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_create);
}

template <>
result<instance_stop_message>
decode_as<instance_stop_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<instance_stop_message>(message, explain, read_stop);
}

template <>
result<api_message>
encode_as<instance_stop_message>(const instance_stop_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_stop);
}

template <>
result<params_changed_message>
decode_as<params_changed_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<params_changed_message>(message, explain, read_changed);
}

template <>
result<api_message>
encode_as<params_changed_message>(const params_changed_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_changed);
}

} // namespace vss::plugin
