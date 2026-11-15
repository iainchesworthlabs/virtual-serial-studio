//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_decoder.hpp>

#include "api_codec.hpp"

#include <array>
#include <utility>
#include <variant>

namespace vss::plugin
{

namespace
{

using namespace detail;

constexpr std::array direction_names{std::pair{"rx"sv, decoder_direction::rx},
                                     std::pair{"tx"sv, decoder_direction::tx}};

constexpr std::array line_names{std::pair{"dtr"sv, decoder_line::dtr},
                                std::pair{"rts"sv, decoder_line::rts},
                                std::pair{"cts"sv, decoder_line::cts},
                                std::pair{"dsr"sv, decoder_line::dsr},
                                std::pair{"dcd"sv, decoder_line::dcd},
                                std::pair{"ri"sv, decoder_line::ri}};

constexpr std::array line_error_names{
    std::pair{"parity"sv, decoder_line_error::parity},
    std::pair{"framing"sv, decoder_line_error::framing},
    std::pair{"overrun"sv, decoder_line_error::overrun},
    std::pair{"break"sv, decoder_line_error::line_break}};

constexpr std::array fault_names{
    std::pair{"overlap"sv, decoder_fault::overlap},
    std::pair{"too_soon"sv, decoder_fault::too_soon},
    std::pair{"line_held"sv, decoder_fault::line_held},
    std::pair{"transmitter_off"sv, decoder_fault::transmitter_off},
    std::pair{"overrun"sv, decoder_fault::overrun}};

constexpr std::array trigger_kind_names{
    std::pair{"framing"sv, trigger_kind::framing},
    std::pair{"unknown_type"sv, trigger_kind::unknown_type},
    std::pair{"unknown_value"sv, trigger_kind::unknown_value},
    std::pair{"check"sv, trigger_kind::check},
    std::pair{"length"sv, trigger_kind::length},
    std::pair{"timeout"sv, trigger_kind::timeout},
    std::pair{"sequence"sv, trigger_kind::sequence},
    std::pair{"contention"sv, trigger_kind::contention},
    std::pair{"line_error"sv, trigger_kind::line_error},
    std::pair{"decoder_lost"sv, trigger_kind::decoder_lost},
    std::pair{"custom"sv, trigger_kind::custom}};

enum class item_kind
{
    data,
    line,
    line_error,
    bus_fault,
    rule,
    time,
    lost
};

constexpr std::array item_names{
    std::pair{"data"sv, item_kind::data},
    std::pair{"line"sv, item_kind::line},
    std::pair{"line_error"sv, item_kind::line_error},
    std::pair{"bus_fault"sv, item_kind::bus_fault},
    std::pair{"rule"sv, item_kind::rule},
    std::pair{"time"sv, item_kind::time},
    std::pair{"lost"sv, item_kind::lost}};

constexpr auto direction_what = "rx or tx"sv;
constexpr auto fault_what =
    "overlap, too_soon, line_held, transmitter_off or overrun"sv;

// -- the members that come again and again ------------------------------------

std::uint64_t instance_of(reading& state, const json::object& object,
                          std::string_view where)
{
    return number_of(state, object, where, "instance", 1, max_api_number, true)
        .value_or(0);
}

/// A required whole number from `least` to `max_api_number`.
std::uint64_t whole_of(reading& state, const json::object& object,
                       std::string_view where, std::string_view key,
                       std::uint64_t least = 0)
{
    return number_of(state, object, where, key, least, max_api_number, true)
        .value_or(0);
}

/// A required whole number from 1 to `max_decoder_chunk`.
std::uint64_t length_of(reading& state, const json::object& object,
                        std::string_view where, std::string_view key)
{
    return number_of(state, object, where, key, 1, max_decoder_chunk, true)
        .value_or(0);
}

std::string name_member(reading& state, const json::object& object,
                        std::string_view where, std::string_view key)
{
    return text_of(state, object, where, key, max_api_name, true, true);
}

std::string label_member(reading& state, const json::object& object,
                         std::string_view where, std::string_view key)
{
    return text_of(state, object, where, key, max_decoder_name, true, true);
}

decoder_direction direction_member(reading& state, const json::object& object,
                                   std::string_view where)
{
    return choice_of(state, object, where, "direction", direction_names,
                     direction_what, true)
        .value_or(decoder_direction::rx);
}

decoder_fault fault_member(reading& state, const json::object& object,
                           std::string_view where, std::string_view key)
{
    return choice_of(state, object, where, key, fault_names, fault_what, true)
        .value_or(decoder_fault::overlap);
}

/// `later` must not be before `earlier`.
void not_before(reading& state, std::string_view where, std::string_view key,
                std::uint64_t later, std::uint64_t earlier,
                std::string_view earlier_key)
{
    if (later >= earlier)
        return;
    std::string what = "must not be before ";
    what.append(earlier_key);
    state.fail(member(where, key), what);
}

std::optional<std::uint64_t> whole_value(const json::value& value) noexcept
{
    if (value.is_uint64())
        return value.get_uint64();
    if (value.is_int64() && value.get_int64() >= 0)
        return static_cast<std::uint64_t>(value.get_int64());
    return std::nullopt;
}

// -- the items
// -------------------------------------------------------------------

decoder_data_item read_data_item(reading& state, const json::object& object,
                                 std::string_view at)
{
    decoder_data_item out;
    out.place = name_member(state, object, at, "place");
    out.direction = direction_member(state, object, at);
    out.offset = whole_of(state, object, at, "offset");
    out.t_ns = whole_of(state, object, at, "t_ns");
    out.end_ns =
        number_of(state, object, at, "end_ns", 0, max_api_number, false);
    if (out.end_ns.has_value())
        not_before(state, at, "end_ns", *out.end_ns, out.t_ns, "t_ns");
    out.length = length_of(state, object, at, "length");
    return out;
}

std::optional<decoder_line> line_from(const json::value& value) noexcept
{
    if (!value.is_string())
        return std::nullopt;
    const std::string_view text = view_of(value.get_string());
    for (const auto& [name, one] : line_names)
    {
        if (name == text)
            return one;
    }
    return std::nullopt;
}

void read_changed_lines(reading& state, const json::object& object,
                        std::string_view at, decoder_line_item& out)
{
    const json::array* const list =
        list_of(state, object, at, "changed", max_decoder_lines, true);
    if (list == nullptr)
        return;
    const std::string where = member(at, "changed");
    std::array<bool, max_decoder_lines> seen{};
    out.changed.reserve(list->size());
    for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
    {
        const std::string here = entry(where, i);
        const std::optional<decoder_line> line = line_from((*list)[i]);
        if (!line.has_value())
        {
            state.fail(here, "must be dtr, rts, cts, dsr, dcd or ri");
            return;
        }
        const auto index = static_cast<std::size_t>(*line);
        if (seen[index])
            state.fail(here, "names a line twice");
        else if (!out.lines[index].has_value())
            state.fail(here, "is not a line that lines gives");
        seen[index] = true;
        out.changed.push_back(*line);
    }
}

decoder_line_item read_line_item(reading& state, const json::object& object,
                                 std::string_view at)
{
    decoder_line_item out;
    out.place = name_member(state, object, at, "place");
    out.t_ns = whole_of(state, object, at, "t_ns");
    const json::object* const lines =
        object_of(state, object, at, "lines", true);
    if (lines != nullptr)
    {
        const std::string where = member(at, "lines");
        for (std::size_t i = 0; i < line_names.size(); ++i)
            out.lines[i] =
                bool_of(state, *lines, where, line_names[i].first, false);
    }
    read_changed_lines(state, object, at, out);
    return out;
}

decoder_line_error_item read_line_error_item(reading& state,
                                             const json::object& object,
                                             std::string_view at)
{
    decoder_line_error_item out;
    out.place = name_member(state, object, at, "place");
    out.t_ns = whole_of(state, object, at, "t_ns");
    out.kind = choice_of(state, object, at, "kind", line_error_names,
                         "parity, framing, overrun or break", true)
                   .value_or(decoder_line_error::parity);
    return out;
}

decoder_bus_fault_item read_bus_fault_item(reading& state,
                                           const json::object& object,
                                           std::string_view at)
{
    decoder_bus_fault_item out;
    out.link = name_member(state, object, at, "link");
    out.sender = name_member(state, object, at, "sender");
    out.t_ns = whole_of(state, object, at, "t_ns");
    out.fault = fault_member(state, object, at, "fault");
    out.offset = whole_of(state, object, at, "offset");
    out.length = length_of(state, object, at, "length");
    return out;
}

decoder_rule_item read_rule_item(reading& state, const json::object& object,
                                 std::string_view at)
{
    decoder_rule_item out;
    out.place = name_member(state, object, at, "place");
    out.t_ns = whole_of(state, object, at, "t_ns");
    out.rule = label_member(state, object, at, "rule");
    out.action = label_member(state, object, at, "action");
    return out;
}

decoder_time_item read_time_item(reading& state, const json::object& object,
                                 std::string_view at)
{
    decoder_time_item out;
    out.t_ns = whole_of(state, object, at, "t_ns");
    return out;
}

decoder_lost_item read_lost_item(reading& state, const json::object& object,
                                 std::string_view at)
{
    decoder_lost_item out;
    out.place = name_member(state, object, at, "place");
    out.direction = direction_member(state, object, at);
    out.from_offset = whole_of(state, object, at, "from_offset");
    out.bytes = whole_of(state, object, at, "bytes", 1);
    out.from_ns = whole_of(state, object, at, "from_ns");
    out.to_ns = whole_of(state, object, at, "to_ns");
    not_before(state, at, "to_ns", out.to_ns, out.from_ns, "from_ns");
    return out;
}

/// The item in `value`, added to `items`.  The lengths of its bytes are added
/// to `bytes`.
void read_item(reading& state, const json::value& value, std::string_view at,
               std::vector<decoder_item>& items, std::uint64_t& bytes)
{
    const json::object* const object = entry_object(state, value, at);
    if (object == nullptr)
        return;
    const std::optional<item_kind> kind = choice_of(
        state, *object, at, "item", item_names,
        "data, line, line_error, bus_fault, rule, time or lost", true);
    if (!kind.has_value())
        return;
    switch (*kind)
    {
    case item_kind::data:
    {
        decoder_data_item item = read_data_item(state, *object, at);
        bytes += item.length;
        items.emplace_back(std::move(item));
        break;
    }
    case item_kind::line:
        items.emplace_back(read_line_item(state, *object, at));
        break;
    case item_kind::line_error:
        items.emplace_back(read_line_error_item(state, *object, at));
        break;
    case item_kind::bus_fault:
        items.emplace_back(read_bus_fault_item(state, *object, at));
        break;
    case item_kind::rule:
        items.emplace_back(read_rule_item(state, *object, at));
        break;
    case item_kind::time:
        items.emplace_back(read_time_item(state, *object, at));
        break;
    case item_kind::lost:
        items.emplace_back(read_lost_item(state, *object, at));
        break;
    }
}

json::object start_item(std::string_view kind)
{
    json::object out;
    out.emplace("item", text_value(kind));
    return out;
}

json::object write_item(const decoder_data_item& item)
{
    json::object out = start_item("data");
    out.emplace("place", text_value(item.place));
    out.emplace("direction",
                text_value(name_of(direction_names, item.direction)));
    out.emplace("offset", item.offset);
    out.emplace("t_ns", item.t_ns);
    if (item.end_ns.has_value())
        out.emplace("end_ns", *item.end_ns);
    out.emplace("length", item.length);
    return out;
}

json::object write_item(const decoder_line_item& item)
{
    json::object out = start_item("line");
    out.emplace("place", text_value(item.place));
    out.emplace("t_ns", item.t_ns);
    json::object lines;
    for (std::size_t i = 0; i < line_names.size(); ++i)
    {
        if (item.lines[i].has_value())
            lines.emplace(line_names[i].first, *item.lines[i]);
    }
    out.emplace("lines", std::move(lines));
    json::array changed;
    for (const decoder_line line : item.changed)
        changed.emplace_back(text_value(name_of(line_names, line)));
    out.emplace("changed", std::move(changed));
    return out;
}

json::object write_item(const decoder_line_error_item& item)
{
    json::object out = start_item("line_error");
    out.emplace("place", text_value(item.place));
    out.emplace("t_ns", item.t_ns);
    out.emplace("kind", text_value(name_of(line_error_names, item.kind)));
    return out;
}

json::object write_item(const decoder_bus_fault_item& item)
{
    json::object out = start_item("bus_fault");
    out.emplace("link", text_value(item.link));
    out.emplace("sender", text_value(item.sender));
    out.emplace("t_ns", item.t_ns);
    out.emplace("fault", text_value(name_of(fault_names, item.fault)));
    out.emplace("offset", item.offset);
    out.emplace("length", item.length);
    return out;
}

json::object write_item(const decoder_rule_item& item)
{
    json::object out = start_item("rule");
    out.emplace("place", text_value(item.place));
    out.emplace("t_ns", item.t_ns);
    out.emplace("rule", text_value(item.rule));
    out.emplace("action", text_value(item.action));
    return out;
}

json::object write_item(const decoder_time_item& item)
{
    json::object out = start_item("time");
    out.emplace("t_ns", item.t_ns);
    return out;
}

json::object write_item(const decoder_lost_item& item)
{
    json::object out = start_item("lost");
    out.emplace("place", text_value(item.place));
    out.emplace("direction",
                text_value(name_of(direction_names, item.direction)));
    out.emplace("from_offset", item.from_offset);
    out.emplace("bytes", item.bytes);
    out.emplace("from_ns", item.from_ns);
    out.emplace("to_ns", item.to_ns);
    return out;
}

// -- decoder.create
// -------------------------------------------------------------

void read_place(reading& state, const json::value& value, std::string_view at,
                std::vector<decoder_place>& places)
{
    const json::object* const object = entry_object(state, value, at);
    if (object == nullptr)
        return;
    decoder_place out;
    out.place = name_member(state, *object, at, "place");
    out.rate = number_of(state, *object, at, "rate", 1, max_place_rate, false)
                   .value_or(0);
    out.format =
        text_of(state, *object, at, "format", max_place_format, false, true);
    out.bus = bool_of(state, *object, at, "bus", false).value_or(false);
    for (const decoder_place& before : places)
    {
        if (before.place == out.place)
        {
            state.fail(member(at, "place"), "names a place twice");
            break;
        }
    }
    places.push_back(std::move(out));
}

void read_create(reading& state, const json::object& object,
                 decoder_create_message& out)
{
    constexpr auto where = decoder_create_message::op;
    out.instance = instance_of(state, object, where);
    const json::array* const list =
        list_of(state, object, where, "places", max_decoder_places, true);
    if (list != nullptr)
    {
        const std::string at = member(where, "places");
        if (list->empty())
            state.fail(at, "has no entries");
        out.places.reserve(list->size());
        for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
            read_place(state, (*list)[i], entry(at, i), out.places);
    }
    out.params = object_text_of(state, object, where, "params", false);
}

bool write_create(json::object& header, const decoder_create_message& body)
{
    if (body.places.empty() || body.places.size() > max_decoder_places)
        return false;
    header.emplace("instance", body.instance);
    json::array places;
    for (const decoder_place& place : body.places)
    {
        json::object one;
        one.emplace("place", text_value(place.place));
        if (place.rate != 0)
            one.emplace("rate", place.rate);
        put_optional(one, "format", place.format);
        if (place.bus)
            one.emplace("bus", true);
        places.emplace_back(std::move(one));
    }
    header.emplace("places", std::move(places));
    return put_object_text(header, "params", body.params);
}

// -- decoder.data
// ---------------------------------------------------------------

void read_data(reading& state, const json::object& object,
               decoder_data_message& out)
{
    constexpr auto where = decoder_data_message::op;
    out.seq = whole_of(state, object, where, "seq", 1);
    blob_between(state, where, out.data, 0, max_decoder_blob);
    const json::array* const list =
        list_of(state, object, where, "items", max_decoder_items, true);
    if (list == nullptr)
        return;
    const std::string at = member(where, "items");
    if (list->empty())
    {
        state.fail(at, "has no entries");
        return;
    }
    out.items.reserve(list->size());
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
        read_item(state, (*list)[i], entry(at, i), out.items, bytes);
    if (!state.failed() && bytes != out.data.size())
        state.fail(at, "have data lengths that do not add up to the blob");
}

bool write_data(json::object& header, const decoder_data_message& body)
{
    if (body.items.empty() || body.items.size() > max_decoder_items)
        return false;
    header.emplace("seq", body.seq);
    json::array items;
    items.reserve(body.items.size());
    for (const decoder_item& item : body.items)
    {
        items.emplace_back(
            std::visit([](const auto& one) { return write_item(one); }, item));
    }
    header.emplace("items", std::move(items));
    return true;
}

// -- decoder.ack
// ----------------------------------------------------------------

void read_ack(reading& state, const json::object& object,
              decoder_ack_message& out)
{
    out.seq = whole_of(state, object, decoder_ack_message::op, "seq", 1);
}

bool write_ack(json::object& header, const decoder_ack_message& body)
{
    header.emplace("seq", body.seq);
    return true;
}

// -- decoder.frame
// --------------------------------------------------------------

void read_field(reading& state, const json::value& value, std::string_view at,
                std::uint64_t frame_length, std::vector<decoder_field>& fields)
{
    const json::object* const object = entry_object(state, value, at);
    if (object == nullptr)
        return;
    decoder_field out;
    out.name = label_member(state, *object, at, "name");
    out.offset = whole_of(state, *object, at, "offset");
    out.length =
        number_of(state, *object, at, "length", 0, max_decoder_chunk, true)
            .value_or(0);
    out.value = text_of(state, *object, at, "value", max_api_text, true, false);
    if (!state.failed() &&
        (out.offset > frame_length || out.length > frame_length - out.offset))
        state.fail(at, "does not lie within the frame");
    fields.push_back(std::move(out));
}

void read_frame(reading& state, const json::object& object,
                decoder_frame_message& out)
{
    constexpr auto where = decoder_frame_message::op;
    out.instance = instance_of(state, object, where);
    out.place = name_member(state, object, where, "place");
    out.direction = direction_member(state, object, where);
    out.offset = whole_of(state, object, where, "offset");
    out.length = length_of(state, object, where, "length");
    out.t_start_ns = whole_of(state, object, where, "t_start_ns");
    out.t_end_ns = whole_of(state, object, where, "t_end_ns");
    not_before(state, where, "t_end_ns", out.t_end_ns, out.t_start_ns,
               "t_start_ns");
    out.message = label_member(state, object, where, "message");
    out.summary =
        text_of(state, object, where, "summary", max_api_text, false, false);
    const json::array* const list =
        list_of(state, object, where, "fields", max_decoder_fields, false);
    if (list == nullptr)
        return;
    const std::string at = member(where, "fields");
    out.fields.reserve(list->size());
    for (std::size_t i = 0; i < list->size() && !state.failed(); ++i)
        read_field(state, (*list)[i], entry(at, i), out.length, out.fields);
}

bool write_frame(json::object& header, const decoder_frame_message& body)
{
    if (body.fields.size() > max_decoder_fields)
        return false;
    header.emplace("instance", body.instance);
    header.emplace("place", text_value(body.place));
    header.emplace("direction",
                   text_value(name_of(direction_names, body.direction)));
    header.emplace("offset", body.offset);
    header.emplace("length", body.length);
    header.emplace("t_start_ns", body.t_start_ns);
    header.emplace("t_end_ns", body.t_end_ns);
    header.emplace("message", text_value(body.message));
    put_optional(header, "summary", body.summary);
    json::array fields;
    for (const decoder_field& field : body.fields)
    {
        json::object one;
        one.emplace("name", text_value(field.name));
        one.emplace("offset", field.offset);
        one.emplace("length", field.length);
        one.emplace("value", text_value(field.value));
        fields.emplace_back(std::move(one));
    }
    header.emplace("fields", std::move(fields));
    return true;
}

// -- trigger.publish
// ------------------------------------------------------------

void read_trigger_protocol(reading& state, const json::object& object,
                           std::string_view where,
                           std::optional<trigger_protocol>& out)
{
    const json::object* const one =
        object_of(state, object, where, "protocol", false);
    if (one == nullptr)
        return;
    const std::string at = member(where, "protocol");
    trigger_protocol value;
    value.id = text_of(state, *one, at, "id", max_protocol_id, true, true);
    value.version =
        text_of(state, *one, at, "version", max_protocol_version, true, true);
    out = std::move(value);
}

void read_trigger_frame(reading& state, const json::object& object,
                        std::string_view where,
                        std::optional<trigger_frame>& out)
{
    const json::object* const one =
        object_of(state, object, where, "frame", false);
    if (one == nullptr)
        return;
    const std::string at = member(where, "frame");
    trigger_frame value;
    value.offset = whole_of(state, *one, at, "offset");
    value.length = length_of(state, *one, at, "length");
    value.t_start_ns = whole_of(state, *one, at, "t_start_ns");
    value.t_end_ns = whole_of(state, *one, at, "t_end_ns");
    not_before(state, at, "t_end_ns", value.t_end_ns, value.t_start_ns,
               "t_start_ns");
    value.message =
        text_of(state, *one, at, "message", max_decoder_name, true, false);
    out = std::move(value);
}

void read_trigger_field(reading& state, const json::object& object,
                        std::string_view where,
                        std::optional<trigger_field>& out)
{
    const json::object* const one =
        object_of(state, object, where, "field", false);
    if (one == nullptr)
        return;
    const std::string at = member(where, "field");
    trigger_field value;
    value.name = label_member(state, *one, at, "name");
    value.offset = whole_of(state, *one, at, "offset");
    value.length = length_of(state, *one, at, "length");
    out = std::move(value);
}

void read_trigger_covers(reading& state, const json::object& object,
                         std::string_view where,
                         std::optional<trigger_covers>& out)
{
    const json::array* const list =
        list_of(state, object, where, "covers", 2, false);
    if (list == nullptr)
        return;
    const std::string at = member(where, "covers");
    if (list->size() != 2)
    {
        state.fail(at, "must have exactly 2 entries");
        return;
    }
    std::array<std::uint64_t, 2> numbers{};
    for (std::size_t i = 0; i < numbers.size(); ++i)
    {
        const std::optional<std::uint64_t> number = whole_value((*list)[i]);
        if (!number.has_value() || *number > max_api_number)
        {
            state.fail(entry(at, i), "must be a whole number");
            return;
        }
        numbers[i] = *number;
    }
    if (numbers[1] < numbers[0])
    {
        state.fail(entry(at, 1), "must not be before the first entry");
        return;
    }
    out = trigger_covers{numbers[0], numbers[1]};
}

void read_trigger_cause(reading& state, const json::object& object,
                        std::string_view where,
                        std::optional<trigger_cause>& out)
{
    const json::object* const one =
        object_of(state, object, where, "cause", false);
    if (one == nullptr)
        return;
    const std::string at = member(where, "cause");
    const bool bus = one->contains("bus_fault");
    const bool own = one->contains("own_action");
    if (bus && own)
    {
        state.fail(at, "must have only one of bus_fault and own_action");
    }
    else if (bus)
    {
        trigger_bus_cause value;
        value.fault = fault_member(state, *one, at, "bus_fault");
        value.event = whole_of(state, *one, at, "event");
        out = trigger_cause{value};
    }
    else if (own)
    {
        trigger_own_action value;
        value.action = whole_of(state, *one, at, "own_action");
        out = trigger_cause{value};
    }
    else
    {
        state.fail(at, "must have bus_fault or own_action");
    }
}

void read_trigger_request(reading& state, const json::object& object,
                          std::string_view where,
                          std::optional<trigger_request>& out)
{
    const json::object* const one =
        object_of(state, object, where, "request", false);
    if (one == nullptr)
        return;
    const std::string at = member(where, "request");
    trigger_request value;
    value.offset = whole_of(state, *one, at, "offset");
    value.length = length_of(state, *one, at, "length");
    value.t_end_ns = whole_of(state, *one, at, "t_end_ns");
    out = value;
}

void read_publish(reading& state, const json::object& object,
                  trigger_publish_message& out)
{
    constexpr auto where = trigger_publish_message::op;
    out.instance = instance_of(state, object, where);
    out.kind = choice_of(state, object, where, "kind", trigger_kind_names,
                         "framing, unknown_type, unknown_value, check, "
                         "length, timeout, sequence, contention, line_error, "
                         "decoder_lost or custom",
                         true)
                   .value_or(trigger_kind::framing);
    if (out.kind == trigger_kind::custom)
        out.code =
            token_of(state, object, where, "code", max_decoder_name, true);
    else if (object.contains("code"))
        state.fail(member(where, "code"),
                   "must be left out unless kind is custom");
    out.when_ns = whole_of(state, object, where, "when_ns");
    out.place = name_member(state, object, where, "place");
    out.direction = direction_member(state, object, where);
    out.sender =
        text_of(state, object, where, "sender", max_api_name, false, true);
    read_trigger_protocol(state, object, where, out.protocol);
    read_trigger_frame(state, object, where, out.frame);
    read_trigger_field(state, object, where, out.field);
    out.expected =
        text_of(state, object, where, "expected", max_api_name, false, true);
    out.actual =
        text_of(state, object, where, "actual", max_api_name, false, true);
    if (object.contains("expected") != object.contains("actual"))
        state.fail(
            member(where, object.contains("expected") ? "actual" : "expected"),
            "must be given when the other is");
    read_trigger_covers(state, object, where, out.covers);
    read_trigger_cause(state, object, where, out.cause);
    read_trigger_request(state, object, where, out.request);
    out.text = text_of(state, object, where, "text", max_api_text, true, true);
}

void write_trigger_options(json::object& header,
                           const trigger_publish_message& body)
{
    if (body.protocol.has_value())
    {
        json::object one;
        one.emplace("id", text_value(body.protocol->id));
        one.emplace("version", text_value(body.protocol->version));
        header.emplace("protocol", std::move(one));
    }
    if (body.frame.has_value())
    {
        json::object one;
        one.emplace("offset", body.frame->offset);
        one.emplace("length", body.frame->length);
        one.emplace("t_start_ns", body.frame->t_start_ns);
        one.emplace("t_end_ns", body.frame->t_end_ns);
        one.emplace("message", text_value(body.frame->message));
        header.emplace("frame", std::move(one));
    }
    if (body.field.has_value())
    {
        json::object one;
        one.emplace("name", text_value(body.field->name));
        one.emplace("offset", body.field->offset);
        one.emplace("length", body.field->length);
        header.emplace("field", std::move(one));
    }
    put_optional(header, "expected", body.expected);
    put_optional(header, "actual", body.actual);
    if (body.covers.has_value())
    {
        json::array one;
        one.emplace_back(body.covers->first);
        one.emplace_back(body.covers->last);
        header.emplace("covers", std::move(one));
    }
    if (body.cause.has_value())
    {
        json::object one;
        if (const auto* const bus =
                std::get_if<trigger_bus_cause>(&*body.cause))
        {
            one.emplace("bus_fault",
                        text_value(name_of(fault_names, bus->fault)));
            one.emplace("event", bus->event);
        }
        else if (const auto* const own =
                     std::get_if<trigger_own_action>(&*body.cause))
        {
            one.emplace("own_action", own->action);
        }
        header.emplace("cause", std::move(one));
    }
    if (body.request.has_value())
    {
        json::object one;
        one.emplace("offset", body.request->offset);
        one.emplace("length", body.request->length);
        one.emplace("t_end_ns", body.request->t_end_ns);
        header.emplace("request", std::move(one));
    }
}

bool write_publish(json::object& header, const trigger_publish_message& body)
{
    header.emplace("instance", body.instance);
    header.emplace("kind", text_value(name_of(trigger_kind_names, body.kind)));
    if (body.kind == trigger_kind::custom || !body.code.empty())
        header.emplace("code", text_value(body.code));
    header.emplace("when_ns", body.when_ns);
    header.emplace("place", text_value(body.place));
    header.emplace("direction",
                   text_value(name_of(direction_names, body.direction)));
    put_optional(header, "sender", body.sender);
    write_trigger_options(header, body);
    header.emplace("text", text_value(body.text));
    return true;
}

// -- decoder.stop
// ---------------------------------------------------------------

void read_stop(reading& state, const json::object& object,
               decoder_stop_message& out)
{
    out.instance = instance_of(state, object, decoder_stop_message::op);
}

bool write_stop(json::object& header, const decoder_stop_message& body)
{
    header.emplace("instance", body.instance);
    return true;
}

} // namespace

template <>
result<decoder_create_message>
decode_as<decoder_create_message>(const api_message& message,
                                  std::string* explain)
{
    return decode_with<decoder_create_message>(message, explain, read_create);
}

template <>
result<api_message>
encode_as<decoder_create_message>(const decoder_create_message& body,
                                  std::uint64_t number)
{
    return encode_with(body, number, write_create);
}

template <>
result<decoder_data_message>
decode_as<decoder_data_message>(const api_message& message,
                                std::string* explain)
{
    return decode_with<decoder_data_message>(message, explain, read_data);
}

template <>
result<api_message>
encode_as<decoder_data_message>(const decoder_data_message& body,
                                std::uint64_t number)
{
    return encode_with(body, number, write_data);
}

template <>
result<decoder_ack_message>
decode_as<decoder_ack_message>(const api_message& message, std::string* explain)
{
    return decode_with<decoder_ack_message>(message, explain, read_ack);
}

template <>
result<api_message>
encode_as<decoder_ack_message>(const decoder_ack_message& body,
                               std::uint64_t number)
{
    return encode_with(body, number, write_ack);
}

template <>
result<decoder_frame_message>
decode_as<decoder_frame_message>(const api_message& message,
                                 std::string* explain)
{
    return decode_with<decoder_frame_message>(message, explain, read_frame);
}

template <>
result<api_message>
encode_as<decoder_frame_message>(const decoder_frame_message& body,
                                 std::uint64_t number)
{
    return encode_with(body, number, write_frame);
}

template <>
result<trigger_publish_message>
decode_as<trigger_publish_message>(const api_message& message,
                                   std::string* explain)
{
    return decode_with<trigger_publish_message>(message, explain, read_publish);
}

template <>
result<api_message>
encode_as<trigger_publish_message>(const trigger_publish_message& body,
                                   std::uint64_t number)
{
    return encode_with(body, number, write_publish);
}

template <>
result<decoder_stop_message>
decode_as<decoder_stop_message>(const api_message& message,
                                std::string* explain)
{
    return decode_with<decoder_stop_message>(message, explain, read_stop);
}

template <>
result<api_message>
encode_as<decoder_stop_message>(const decoder_stop_message& body,
                                std::uint64_t number)
{
    return encode_with(body, number, write_stop);
}

} // namespace vss::plugin
