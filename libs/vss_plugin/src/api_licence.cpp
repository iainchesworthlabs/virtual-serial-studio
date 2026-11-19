//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_licence.hpp>

#include "api_codec.hpp"

#include <array>
#include <utility>

namespace vss::plugin
{

namespace
{

using namespace detail;

constexpr std::array reason_names{
    std::pair{"ok"sv, license::reason::ok},
    std::pair{"no_licence"sv, license::reason::no_licence},
    std::pair{"expired"sv, license::reason::expired},
    std::pair{"not_yet_valid"sv, license::reason::not_yet_valid},
    std::pair{"clock_suspect"sv, license::reason::clock_suspect},
    std::pair{"malformed"sv, license::reason::malformed},
    std::pair{"newer_version"sv, license::reason::newer_version},
    std::pair{"bad_signature"sv, license::reason::bad_signature},
    std::pair{"revoked"sv, license::reason::revoked},
    std::pair{"wrong_computer"sv, license::reason::wrong_computer},
    std::pair{"not_in_licence"sv, license::reason::not_in_licence}};

void read_get(reading&, const json::object&, licence_get_message&) {}

bool write_get(json::object&, const licence_get_message&)
{
    return true;
}

void read_file(reading& state, const json::object& object,
               licence_file_message& out)
{
    constexpr auto where = licence_file_message::op();
    out.present =
        bool_of(state, object, where, "present", true).value_or(false);
    if (state.failed())
        return;
    if (out.present)
        blob_between(state, where, out.data, 1, max_licence_file);
    else if (!out.data.empty())
        state.fail(where, "has no licence file, and the frame has a blob");
}

bool write_file(json::object& header, const licence_file_message& body)
{
    header.emplace("present", body.present);
    return true;
}

void read_report(reading& state, const json::object& object,
                 licence_report_message& out)
{
    constexpr auto where = licence_report_message::op();
    out.reason = choice_of(state, object, where, "reason", reason_names,
                           "a reason of the licence library", true)
                     .value_or(license::reason::ok);
    out.licence_id = text_of(state, object, where, "licence_id", max_licence_id,
                             false, true);
}

bool write_report(json::object& header, const licence_report_message& body)
{
    header.emplace("reason", text_value(name_of(reason_names, body.reason)));
    put_optional(header, "licence_id", body.licence_id);
    return true;
}

} // namespace

std::string_view to_string(license::reason value) noexcept
{
    return name_of(reason_names, value);
}

template <>
result<licence_get_message>
decode_as<licence_get_message>(const api_message& message, std::string* explain)
{
    return decode_with<licence_get_message>(message, explain, read_get);
}

template <>
result<api_message>
encode_as<licence_get_message>(const licence_get_message& body,
                               std::uint64_t number)
{
    return encode_with(body, number, write_get);
}

template <>
result<licence_file_message>
decode_as<licence_file_message>(const api_message& message,
                                std::string* explain)
{
    return decode_with<licence_file_message>(message, explain, read_file);
}

template <>
result<api_message>
encode_as<licence_file_message>(const licence_file_message& body,
                                std::uint64_t number)
{
    return encode_with(body, number, write_file);
}

template <>
result<licence_report_message>
decode_as<licence_report_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<licence_report_message>(message, explain, read_report);
}

template <>
result<api_message>
encode_as<licence_report_message>(const licence_report_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_report);
}

} // namespace vss::plugin
