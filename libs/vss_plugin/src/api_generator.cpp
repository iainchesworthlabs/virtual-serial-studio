//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_generator.hpp>

#include "api_codec.hpp"

namespace vss::plugin
{

namespace
{

using namespace detail;

std::uint64_t instance_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "instance", 1, max_api_number, true)
        .value_or(0);
}

void read_start(reading& state, const json::object& object,
                generator_start_message& out)
{
    constexpr auto where = generator_start_message::op;
    out.instance = instance_of(state, object, where);
    out.port =
        number_of(state, object, where, "port", 1, max_api_number, true)
            .value_or(0);
    out.params = object_text_of(state, object, where, "params", false);
}

bool write_start(json::object& header, const generator_start_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("port", body.port);
    return put_object_text(header, "params", body.params);
}

void read_write(reading& state, const json::object& object,
                generator_write_message& out)
{
    constexpr auto where = generator_write_message::op;
    out.instance = instance_of(state, object, where);
    blob_between(state, where, out.data, 1, max_generator_write);
}

bool write_write(json::object& header, const generator_write_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

void read_credit(reading& state, const json::object& object,
                 generator_credit_message& out)
{
    constexpr auto where = generator_credit_message::op;
    out.instance = instance_of(state, object, where);
    out.bytes = number_of(state, object, where, "bytes", 1,
                          max_generator_credit, true)
                    .value_or(0);
}

bool write_credit(json::object& header, const generator_credit_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("bytes", body.bytes);
    return true;
}

void read_pattern(reading& state, const json::object& object,
                  generator_pattern_message& out)
{
    constexpr auto where = generator_pattern_message::op;
    out.instance = instance_of(state, object, where);
    blob_between(state, where, out.data, 1, max_pattern_bytes);
    out.repeat = number_of(state, object, where, "repeat", 0,
                           max_pattern_repeat, false)
                     .value_or(1);
    out.period_us = number_of(state, object, where, "period_us", 0,
                              max_api_delay_us, false)
                        .value_or(0);
    out.burst =
        number_of(state, object, where, "burst", 1, max_pattern_burst, false)
            .value_or(1);
    out.jitter = number_of(state, object, where, "jitter", 0,
                           max_api_delay_us, false)
                     .value_or(0);
}

bool write_pattern(json::object& header, const generator_pattern_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("repeat", body.repeat);
    header.emplace("period_us", body.period_us);
    header.emplace("burst", body.burst);
    header.emplace("jitter", body.jitter);
    return true;
}

void read_stop(reading& state, const json::object& object,
               generator_stop_message& out)
{
    out.instance = instance_of(state, object, generator_stop_message::op);
}

bool write_stop(json::object& header, const generator_stop_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

void read_done(reading& state, const json::object& object,
               generator_done_message& out)
{
    constexpr auto where = generator_done_message::op;
    out.instance = instance_of(state, object, where);
    out.reason =
        text_of(state, object, where, "reason", max_api_text, true, true);
}

bool write_done(json::object& header, const generator_done_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("reason", text_value(body.reason));
    return true;
}

} // namespace

template <>
result<generator_start_message>
decode_as<generator_start_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<generator_start_message>(message, explain, read_start);
}

template <>
result<api_message>
encode_as<generator_start_message>(const generator_start_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_start);
}

template <>
result<generator_write_message>
decode_as<generator_write_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<generator_write_message>(message, explain, read_write);
}

template <>
result<api_message>
encode_as<generator_write_message>(const generator_write_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_write);
}

template <>
result<generator_credit_message>
decode_as<generator_credit_message>(const api_message& message,
                                    std::string* explain)
{
    return decode_with<generator_credit_message>(message, explain,
                                                 read_credit);
}

template <>
result<api_message>
encode_as<generator_credit_message>(const generator_credit_message& body,
                                    std::uint64_t number)
{
    return encode_with(body, number, write_credit);
}

template <>
result<generator_pattern_message>
decode_as<generator_pattern_message>(const api_message& message,
                                     std::string* explain)
{
    return decode_with<generator_pattern_message>(message, explain,
                                                  read_pattern);
}

template <>
result<api_message>
encode_as<generator_pattern_message>(const generator_pattern_message& body,
                                     std::uint64_t number)
{
    return encode_with(body, number, write_pattern);
}

template <>
result<generator_stop_message>
decode_as<generator_stop_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<generator_stop_message>(message, explain, read_stop);
}

template <>
result<api_message>
encode_as<generator_stop_message>(const generator_stop_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_stop);
}

template <>
result<generator_done_message>
decode_as<generator_done_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<generator_done_message>(message, explain, read_done);
}

template <>
result<api_message>
encode_as<generator_done_message>(const generator_done_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_done);
}

} // namespace vss::plugin
