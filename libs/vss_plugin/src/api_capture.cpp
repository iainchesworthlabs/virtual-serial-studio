//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_capture.hpp>

#include "api_codec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace vss::plugin
{

namespace
{

using namespace detail;

bool has_name(const std::vector<std::string>& names, std::string_view name)
{
    return std::ranges::any_of(names, [name](const std::string& one)
                               { return one == name; });
}

/// A list of names, each 1 to `max_api_name` bytes of plain text, `least` to
/// `most` of them.  Empty when the member is absent or wrong.  A name given
/// twice is wrong when `unique`.
std::vector<std::string> names_of(reading& state, const json::object& object,
                                  std::string_view where, std::string_view key,
                                  std::size_t least, std::size_t most,
                                  bool unique)
{
    std::vector<std::string> out;
    const json::array* const list =
        list_of(state, object, where, key, most, least > 0);
    if (list == nullptr)
        return out;
    const std::string list_name = member(where, key);
    if (list->size() < least)
    {
        state.fail(list_name,
                   "must have at least " + std::to_string(least) + " entry");
        return out;
    }
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
        if (unique && has_name(out, text))
        {
            state.fail(at, "is given twice");
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

bool is_snapshot_kind(std::string_view text) noexcept
{
    return std::ranges::find(snapshot_kinds, text) != snapshot_kinds.end();
}

/// The kinds of `snapshot_on`: 1 to `max_snapshot_kinds` of `snapshot_kinds`,
/// none twice.
std::vector<std::string> kinds_of(reading& state, const json::object& object,
                                  std::string_view where)
{
    std::vector<std::string> out;
    const json::array* const list =
        list_of(state, object, where, "kinds", max_snapshot_kinds, true);
    if (list == nullptr)
        return out;
    const std::string list_name = member(where, "kinds");
    if (list->empty())
    {
        state.fail(list_name, "must have at least 1 entry");
        return out;
    }
    out.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const std::string at = entry(list_name, i);
        const json::value& item = (*list)[i];
        const std::string_view text =
            item.is_string() ? view_of(item.get_string()) : std::string_view{};
        if (!is_snapshot_kind(text))
        {
            state.fail(at, "must be a kind of trigger: framing, "
                           "unknown_type, unknown_value, check, length, "
                           "timeout, sequence, contention, line_error, "
                           "decoder_lost or custom");
            return {};
        }
        if (has_name(out, text))
        {
            state.fail(at, "is given twice");
            return {};
        }
        out.emplace_back(text);
    }
    return out;
}

snapshot_spec read_snapshot(reading& state, const json::object& object,
                            std::string_view where)
{
    snapshot_spec out;
    out.kinds = kinds_of(state, object, where);
    out.places =
        names_of(state, object, where, "places", 0, max_capture_places, false);
    return out;
}

void refuse_without_ring(reading& state, const json::object& object,
                         std::string_view where)
{
    for (const std::string_view key :
         {"snapshot_on"sv, "before_s"sv, "after_s"sv})
    {
        if (object.if_contains(key) != nullptr)
        {
            state.fail(member(where, key),
                       "is accepted only when ring is not 0");
        }
    }
}

void read_start(reading& state, const json::object& object,
                capture_start_message& out)
{
    constexpr auto where = capture_start_message::op;
    out.places =
        names_of(state, object, where, "places", 1, max_capture_places, true);
    out.limit_bytes = number_of(state, object, where, "limit_bytes", 1,
                                max_capture_bytes, false)
                          .value_or(default_capture_bytes);
    out.limit_seconds = number_of(state, object, where, "limit_seconds", 1,
                                  max_capture_seconds, false)
                            .value_or(default_capture_seconds);
    out.ring = number_of(state, object, where, "ring", 0,
                         max_capture_ring_minutes, false)
                   .value_or(0);
    if (state.failed())
        return;
    if (out.ring == 0)
        refuse_without_ring(state, object, where);
    if (state.failed())
        return;
    const std::string snapshot_where = member(where, "snapshot_on");
    if (const json::object* const snapshot =
            object_of(state, object, where, "snapshot_on", false))
    {
        out.snapshot_on = read_snapshot(state, *snapshot, snapshot_where);
    }
    out.before_s = number_of(state, object, where, "before_s", 0,
                             max_snapshot_seconds, false)
                       .value_or(0);
    out.after_s = number_of(state, object, where, "after_s", 0,
                            max_snapshot_seconds, false)
                      .value_or(0);
}

bool write_start(json::object& header, const capture_start_message& body)
{
    header.emplace("places", names_value(body.places));
    header.emplace("limit_bytes", body.limit_bytes);
    header.emplace("limit_seconds", body.limit_seconds);
    header.emplace("ring", body.ring);
    if (body.snapshot_on)
    {
        json::object snapshot;
        snapshot.emplace("kinds", names_value(body.snapshot_on->kinds));
        snapshot.emplace("places", names_value(body.snapshot_on->places));
        header.emplace("snapshot_on", std::move(snapshot));
    }
    if (body.before_s != 0)
        header.emplace("before_s", body.before_s);
    if (body.after_s != 0)
        header.emplace("after_s", body.after_s);
    return true;
}

void read_stop(reading&, const json::object&, capture_stop_message&) {}

bool write_stop(json::object&, const capture_stop_message&)
{
    return true;
}

void read_status(reading&, const json::object&, capture_status_message&) {}

bool write_status(json::object&, const capture_status_message&)
{
    return true;
}

void read_capture_result(reading& state, const json::object& object,
                         capture_result_message& out)
{
    constexpr auto where = capture_result_message::op;
    out.running =
        bool_of(state, object, where, "running", true).value_or(false);
    out.bytes =
        number_of(state, object, where, "bytes", 0, max_api_number, true)
            .value_or(0);
    out.seconds =
        number_of(state, object, where, "seconds", 0, max_api_number, true)
            .value_or(0);
    out.dropped =
        number_of(state, object, where, "dropped", 0, max_api_number, true)
            .value_or(0);
    out.ring = bool_of(state, object, where, "ring", true).value_or(false);
}

bool write_capture_result(json::object& header,
                          const capture_result_message& body)
{
    header.emplace("running", body.running);
    header.emplace("bytes", body.bytes);
    header.emplace("seconds", body.seconds);
    header.emplace("dropped", body.dropped);
    header.emplace("ring", body.ring);
    return true;
}

/// The number in `value`, as `real_of` reads it.  Not a number when it is
/// anything else.
double number_value(const json::value& value) noexcept
{
    if (value.is_double())
        return value.get_double();
    if (value.is_int64())
        return static_cast<double>(value.get_int64());
    if (value.is_uint64())
        return static_cast<double>(value.get_uint64());
    return std::nan("");
}

/// Whether `number` is written as text that reads back as the same double.
/// The reader of this library does not promise that for every number.
bool reads_back(double number)
{
    if (!std::isfinite(number))
        return false;
    const std::string text = json::serialize(json::value(number));
    boost::system::error_code failed;
    const json::value again = json::parse(text, failed);
    if (failed)
        return false;
    return std::bit_cast<std::uint64_t>(number_value(again)) ==
           std::bit_cast<std::uint64_t>(number);
}

void read_timeline(reading& state, const json::object& object,
                   timeline_load_message& out)
{
    constexpr auto where = timeline_load_message::op;
    out.instance =
        number_of(state, object, where, "instance", 1, max_api_number, true)
            .value_or(0);
    out.recording = text_of(state, object, where, "recording",
                            max_recording_path, true, true);
    out.place =
        text_of(state, object, where, "place", max_api_name, true, true);
    out.time_scale = real_of(state, object, where, "time_scale", min_time_scale,
                             max_time_scale, false)
                         .value_or(1.0);
    out.seed =
        number_of(state, object, where, "seed", 0, max_api_number, false);
}

bool write_timeline(json::object& header, const timeline_load_message& body)
{
    if (!reads_back(body.time_scale))
        return false;
    header.emplace("instance", body.instance);
    header.emplace("recording", text_value(body.recording));
    header.emplace("place", text_value(body.place));
    header.emplace("time_scale", body.time_scale);
    if (body.seed)
        header.emplace("seed", *body.seed);
    return true;
}

void read_prepare(reading& state, const json::object& object,
                  bundle_prepare_message& out)
{
    constexpr auto where = bundle_prepare_message::op;
    out.section = token_of(state, object, where, "section", max_api_name, true);
    out.settings = object_text_of(state, object, where, "settings", false);
}

bool write_prepare(json::object& header, const bundle_prepare_message& body)
{
    header.emplace("section", text_value(body.section));
    return put_object_text(header, "settings", body.settings);
}

bool plain_file_name(std::string_view name) noexcept
{
    if (name == "." || name == "..")
        return false;
    for (const char c : name)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || byte < 0x20 || byte == 0x7f)
            return false;
    }
    return clean_text(name);
}

void read_files(reading& state, const json::object& object,
                bundle_result_message& out)
{
    constexpr auto where = bundle_result_message::op;
    const json::array* const list =
        list_of(state, object, where, "files", max_bundle_files, true);
    if (list == nullptr)
        return;
    const std::string list_name = member(where, "files");
    out.files.reserve(list->size());
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const std::string at = entry(list_name, i);
        const json::object* const item = entry_object(state, (*list)[i], at);
        if (item == nullptr)
            return;
        bundle_file file;
        file.name =
            text_of(state, *item, at, "name", max_bundle_file_name, true, true);
        if (!state.failed() && !plain_file_name(file.name))
        {
            state.fail(member(at, "name"),
                       "must be a plain file name, with no slash, no "
                       "backslash and no control character, and not . or ..");
        }
        file.size = number_of(state, *item, at, "size", 0,
                              max_bundle_section_bytes, true)
                        .value_or(0);
        if (state.failed())
            return;
        if (std::ranges::any_of(out.files, [&file](const bundle_file& other)
                                { return other.name == file.name; }))
        {
            state.fail(member(at, "name"), "is given twice");
            return;
        }
        total += file.size;
        if (total > max_bundle_section_bytes)
        {
            state.fail(list_name, "has sizes that add up to more than " +
                                      std::to_string(max_bundle_section_bytes) +
                                      " bytes");
            return;
        }
        out.files.push_back(std::move(file));
    }
}

bool write_files(json::object& header, const bundle_result_message& body)
{
    json::array files;
    files.reserve(body.files.size());
    for (const bundle_file& file : body.files)
    {
        json::object one;
        one.emplace("name", text_value(file.name));
        one.emplace("size", file.size);
        files.emplace_back(std::move(one));
    }
    header.emplace("files", std::move(files));
    return true;
}

} // namespace

template <>
result<capture_start_message>
decode_as<capture_start_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<capture_start_message>(message, explain, read_start);
}

template <>
result<api_message>
encode_as<capture_start_message>(const capture_start_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_start);
}

template <>
result<capture_stop_message>
decode_as<capture_stop_message>(const api_message& message,
                                std::string* explain)
{
    return decode_with<capture_stop_message>(message, explain, read_stop);
}

template <>
result<api_message>
encode_as<capture_stop_message>(const capture_stop_message& body,
                                std::uint64_t number)
{
    return encode_with(body, number, write_stop);
}

template <>
result<capture_status_message>
decode_as<capture_status_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<capture_status_message>(message, explain, read_status);
}

template <>
result<api_message>
encode_as<capture_status_message>(const capture_status_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_status);
}

template <>
result<capture_result_message>
decode_as<capture_result_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<capture_result_message>(message, explain,
                                               read_capture_result);
}

template <>
result<api_message>
encode_as<capture_result_message>(const capture_result_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_capture_result);
}

template <>
result<timeline_load_message>
decode_as<timeline_load_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<timeline_load_message>(message, explain, read_timeline);
}

template <>
result<api_message>
encode_as<timeline_load_message>(const timeline_load_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_timeline);
}

template <>
result<bundle_prepare_message>
decode_as<bundle_prepare_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<bundle_prepare_message>(message, explain, read_prepare);
}

template <>
result<api_message>
encode_as<bundle_prepare_message>(const bundle_prepare_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_prepare);
}

template <>
result<bundle_result_message>
decode_as<bundle_result_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<bundle_result_message>(message, explain, read_files);
}

template <>
result<api_message>
encode_as<bundle_result_message>(const bundle_result_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_files);
}

} // namespace vss::plugin
