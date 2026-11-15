//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_decoder.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace vss::plugin_api_test;

namespace
{

std::string repeated(char c, std::size_t count)
{
    return std::string(count, c);
}

byte_buffer bytes_of(std::size_t count)
{
    byte_buffer out(count);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = static_cast<std::uint8_t>(i * 7);
    return out;
}

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

std::string number_text(std::uint64_t value)
{
    return std::to_string(value);
}

// -- decoder.data, built from the members of each item
// ---------------------------

using member_list = std::vector<std::pair<std::string_view, std::string_view>>;

struct item_case
{
    std::string_view kind;
    member_list members;
    /// The bytes that go with it in the blob.
    std::size_t blob = 0;
    /// Members that may be left out.
    std::vector<std::string_view> optional;
};

/// The item as JSON.  `key` is left out when `value` is empty, and given
/// `value` when it is not.
std::string item_text(const item_case& one, std::string_view key = {},
                      std::string_view value = {})
{
    std::string out = R"({"item":")";
    out.append(one.kind);
    out.append("\"");
    for (const auto& [name, text] : one.members)
    {
        if (name == key && value.empty())
            continue;
        out.append(",\"");
        out.append(name);
        out.append("\":");
        out.append(name == key ? value : text);
    }
    out.append("}");
    return out;
}

std::string stream(std::string_view items, std::uint64_t seq = 1)
{
    std::string out = R"({"op":"decoder.data","seq":)";
    out.append(number_text(seq));
    out.append(R"(,"items":[)");
    out.append(items);
    out.append("]}");
    return out;
}

const std::vector<item_case>& item_cases()
{
    static const std::vector<item_case> cases{
        {"data",
         {{"place", R"("p")"},
          {"direction", R"("rx")"},
          {"offset", "0"},
          {"t_ns", "1"},
          {"end_ns", "2"},
          {"length", "2"}},
         2,
         {"end_ns"}},
        {"line",
         {{"place", R"("p")"},
          {"t_ns", "1"},
          {"lines", R"({"dtr":true})"},
          {"changed", R"(["dtr"])"}},
         0,
         {}},
        {"line_error",
         {{"place", R"("p")"}, {"t_ns", "1"}, {"kind", R"("parity")"}},
         0,
         {}},
        {"bus_fault",
         {{"link", R"("l")"},
          {"sender", R"("s")"},
          {"t_ns", "1"},
          {"fault", R"("overlap")"},
          {"offset", "0"},
          {"length", "2"}},
         0,
         {}},
        {"rule",
         {{"place", R"("p")"},
          {"t_ns", "1"},
          {"rule", R"("r")"},
          {"action", R"("a")"}},
         0,
         {}},
        {"time", {{"t_ns", "1"}}, 0, {}},
        {"lost",
         {{"place", R"("p")"},
          {"direction", R"("tx")"},
          {"from_offset", "0"},
          {"bytes", "5"},
          {"from_ns", "1"},
          {"to_ns", "2"}},
         0,
         {}}};
    return cases;
}

const item_case& item_named(std::string_view kind)
{
    for (const item_case& one : item_cases())
    {
        if (one.kind == kind)
            return one;
    }
    throw vss_test::failure("no such item case");
}

/// `kind` with `key` set to `value`, as a whole message that has the blob it
/// needs.
void require_item_refused(std::string_view kind, std::string_view key,
                          std::string_view value, std::string_view fragment)
{
    const item_case& one = item_named(kind);
    require_refused<decoder_data_message>(stream(item_text(one, key, value)),
                                          fragment, bytes_of(one.blob));
}

decoder_data_message sample_stream()
{
    decoder_data_message body;
    body.seq = 12;
    decoder_data_item first;
    first.place = "bus-1";
    first.direction = decoder_direction::rx;
    first.offset = 48213;
    first.t_ns = 18203200000;
    first.end_ns = 18204113250;
    first.length = 2;
    body.items.emplace_back(first);
    decoder_line_item line;
    line.place = "bus-1";
    line.t_ns = 5;
    line.lines[static_cast<std::size_t>(decoder_line::dtr)] = true;
    line.lines[static_cast<std::size_t>(decoder_line::cts)] = false;
    line.changed = {decoder_line::dtr, decoder_line::cts};
    body.items.emplace_back(line);
    body.items.emplace_back(
        decoder_line_error_item{"bus-1", 6, decoder_line_error::line_break});
    body.items.emplace_back(decoder_bus_fault_item{
        "link-1", "meter-17", 7, decoder_fault::too_soon, 48215, 3});
    body.items.emplace_back(decoder_rule_item{"bus-1", 8, "reply", "send"});
    body.items.emplace_back(decoder_time_item{9});
    decoder_data_item second;
    second.place = "bus-1";
    second.direction = decoder_direction::tx;
    second.offset = 0;
    second.t_ns = 10;
    second.length = 3;
    body.items.emplace_back(second);
    body.items.emplace_back(
        decoder_lost_item{"bus-1", decoder_direction::tx, 3, 4096, 11, 12});
    body.data = bytes_of(5);
    return body;
}

decoder_frame_message sample_frame()
{
    decoder_frame_message body;
    body.instance = 3;
    body.place = "bus-1";
    body.direction = decoder_direction::rx;
    body.offset = 48213;
    body.length = 10;
    body.t_start_ns = 18203200000;
    body.t_end_ns = 18204113250;
    body.message = "read reply";
    body.summary = "read reply, 3 values";
    body.fields = {{"address", 0, 1, "5"}, {"check", 7, 2, "3A91"}};
    return body;
}

const std::string frame_head =
    R"({"op":"decoder.frame","instance":3,"place":"p","direction":"rx",)"
    R"("offset":0,"length":10,"t_start_ns":1,"t_end_ns":2,"message":"m")";

} // namespace

// -- decoder.create
// --------------------------------------------------------------

TEST_CASE("plugin api: decoder.create is written and read")
{
    decoder_create_message body;
    body.instance = 3;
    body.places = {{"bus-1", 115200, "8N1", true}, {"port-2", 0, "", false}};
    body.params = R"({"silence":"1750us"})";
    (void)round_trip(body, 8);
    const auto written = encode_as(body, 8);
    REQUIRE(written.has_value());
    REQUIRE_EQ(
        header_of(written.value()),
        std::string(R"({"op":"decoder.create","id":8,"instance":3,"places":)"
                    R"([{"place":"bus-1","rate":115200,"format":"8N1",)"
                    R"("bus":true},{"place":"port-2"}],)"
                    R"("params":{"silence":"1750us"}})"));
}

TEST_CASE("plugin api: decoder.create has defaults for what it leaves out")
{
    const auto read = accepted<decoder_create_message>(
        R"({"op":"decoder.create","id":1,"instance":2,)"
        R"("places":[{"place":"a"}]})");
    REQUIRE_EQ(read.places.size(), std::size_t{1});
    const decoder_place expected{"a", 0, "", false};
    REQUIRE(read.places[0] == expected);
    REQUIRE_EQ(read.params, std::string("{}"));
}

TEST_CASE("plugin api: decoder.create is refused when it is wrong")
{
    using M = decoder_create_message;
    const std::string head = R"({"op":"decoder.create","id":1,"instance":2,)";
    require_refused<M>(R"({"op":"decoder.create","id":1,"places":[]})",
                       "decoder.create.instance: is required");
    require_refused<M>(head + R"("places":[]})",
                       "decoder.create.places: has no entries");
    require_refused<M>(head + R"("x":1})",
                       "decoder.create.places: is required");
    require_refused<M>(head + R"("places":{}})",
                       "decoder.create.places: must be a list");
    require_refused<M>(head + R"("places":[7]})",
                       "decoder.create.places[0]: must be an object");
    require_refused<M>(head + R"("places":[{}]})",
                       "decoder.create.places[0].place: is required");
    require_refused<M>(head + R"("places":[{"place":""}]})",
                       "decoder.create.places[0].place: must not be empty");
    require_refused<M>(head + R"("places":[{"place":4}]})",
                       "decoder.create.places[0].place: must be a string");
    require_refused<M>(head + "\"places\":[{\"place\":\"a\\nb\"}]}",
                       "decoder.create.places[0].place: must be plain text");
    require_refused<M>(head + "\"places\":[{\"place\":\"a\\u202eb\"}]}",
                       "decoder.create.places[0].place: must be plain text");
    require_refused<M>(head + R"("places":[{"place":"a"},{"place":"a"}]})",
                       "decoder.create.places[1].place: names a place twice");
    require_refused<M>(head + R"("places":[{"place":"a","rate":0}]})",
                       "decoder.create.places[0].rate: must be a whole number "
                       "from 1 to 100000000");
    require_refused<M>(head + R"("places":[{"place":"a","rate":"9600"}]})",
                       "decoder.create.places[0].rate: must be a whole");
    require_refused<M>(head + R"("places":[{"place":"a","rate":9600.5}]})",
                       "decoder.create.places[0].rate: must be a whole");
    require_refused<M>(head + R"("places":[{"place":"a","format":""}]})",
                       "decoder.create.places[0].format: must not be empty");
    require_refused<M>(head + R"("places":[{"place":"a","format":7}]})",
                       "decoder.create.places[0].format: must be a string");
    require_refused<M>(head + R"("places":[{"place":"a","bus":"yes"}]})",
                       "decoder.create.places[0].bus: must be true or false");
    require_refused<M>(head + R"("places":[{"place":"a"}],"params":[]})",
                       "decoder.create.params: must be a JSON object");
    require_refused<M>(head + R"("places":[{"place":"a"}]})", "has no bytes",
                       {1});
    require_refused<M>(R"({"op":"decoder.create","instance":2,)"
                       R"("places":[{"place":"a"}]})",
                       "must have an id");
    require_refused<M>(R"({"op":"decoder.create","re":1,"instance":2,)"
                       R"("places":[{"place":"a"}]})",
                       "must have an id and no re");
}

TEST_CASE("plugin api: decoder.create has the limits of its members")
{
    using M = decoder_create_message;
    const std::string head = R"({"op":"decoder.create","id":1,"instance":2,)";
    // The instance.
    REQUIRE_EQ(accepted<M>(R"({"op":"decoder.create","id":1,"instance":)" +
                           number_text(max_api_number) +
                           R"(,"places":[{"place":"a"}]})")
                   .instance,
               max_api_number);
    require_refused<M>(R"({"op":"decoder.create","id":1,"instance":0,)"
                       R"("places":[{"place":"a"}]})",
                       "decoder.create.instance: must be a whole number");
    require_refused<M>(R"({"op":"decoder.create","id":1,"instance":)" +
                           number_text(max_api_number + 1) +
                           R"(,"places":[{"place":"a"}]})",
                       "decoder.create.instance: must be a whole number");
    // The rate.
    REQUIRE_EQ(accepted<M>(head + R"("places":[{"place":"a","rate":1}]})")
                   .places[0]
                   .rate,
               std::uint64_t{1});
    REQUIRE_EQ(
        accepted<M>(head + R"("places":[{"place":"a","rate":100000000}]})")
            .places[0]
            .rate,
        max_place_rate);
    require_refused<M>(head + R"("places":[{"place":"a","rate":100000001}]})",
                       "decoder.create.places[0].rate: must be a whole number");
    // The names.
    REQUIRE_EQ(accepted<M>(head + R"("places":[{"place":")" +
                           repeated('p', max_api_name) + R"("}]})")
                   .places[0]
                   .place.size(),
               max_api_name);
    require_refused<M>(head + R"("places":[{"place":")" +
                           repeated('p', max_api_name + 1) + R"("}]})",
                       "decoder.create.places[0].place: is longer than "
                       "128 bytes");
    REQUIRE_EQ(accepted<M>(head + R"("places":[{"place":"a","format":")" +
                           repeated('f', max_place_format) + R"("}]})")
                   .places[0]
                   .format.size(),
               max_place_format);
    require_refused<M>(head + R"("places":[{"place":"a","format":")" +
                           repeated('f', max_place_format + 1) + R"("}]})",
                       "decoder.create.places[0].format: is longer than "
                       "16 bytes");
    // The number of places.
    const auto places_of = [](std::size_t count)
    {
        std::string many;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i != 0)
                many += ',';
            many += R"({"place":"p)" + number_text(i) + R"("})";
        }
        return many;
    };
    REQUIRE_EQ(accepted<M>(head + R"("places":[)" +
                           places_of(max_decoder_places) + "]}")
                   .places.size(),
               max_decoder_places);
    require_refused<M>(head + R"("places":[)" +
                           places_of(max_decoder_places + 1) + "]}",
                       "decoder.create.places: has more than 64 entries");

    M body;
    body.instance = 2;
    require_not_written(body);
    body.places.assign(max_decoder_places + 1,
                       decoder_place{"a", 0, "", false});
    require_not_written(body);
    body.places.clear();
    for (std::size_t i = 0; i < max_decoder_places; ++i)
        body.places.push_back({"p" + number_text(i), 0, "", false});
    (void)round_trip(body, 1);
    body.places.back() = body.places.front();
    require_not_written(body, 1);
    body.places.back() = {"x", max_place_rate + 1, "", false};
    require_not_written(body, 1);
    body.places.back() = {"x", 0, repeated('f', max_place_format + 1), false};
    require_not_written(body, 1);
    body.places.back() = {"", 0, "", false};
    require_not_written(body, 1);
    body.places.back() = {"x", 0, "", false};
    body.instance = 0;
    require_not_written(body, 1);
    body.instance = 2;
    body.params = "[]";
    require_not_written(body, 1);
    body.params = "{}";
    require_not_written(body, 0);
}

// -- decoder.data
// ----------------------------------------------------------------

TEST_CASE("plugin api: decoder.data is written and read with all its items")
{
    const decoder_data_message body = sample_stream();
    const decoder_data_message read = round_trip(body);
    REQUIRE(read.data == body.data);
    REQUIRE_EQ(read.items.size(), std::size_t{8});
    REQUIRE_EQ(std::get<decoder_data_item>(read.items[0]).length,
               std::uint64_t{2});
    REQUIRE(std::get<decoder_line_item>(read.items[1]).changed.size() == 2);
    REQUIRE(std::get<decoder_lost_item>(read.items[7]).bytes == 4096);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
}

TEST_CASE("plugin api: decoder.data has the header of the design")
{
    decoder_data_message body;
    body.seq = 4;
    decoder_data_item one;
    one.place = "p";
    one.direction = decoder_direction::tx;
    one.offset = 7;
    one.t_ns = 100;
    one.length = 3;
    body.items.emplace_back(one);
    one.end_ns = 150;
    one.length = 1;
    body.items.emplace_back(one);
    decoder_line_item line;
    line.place = "p";
    line.t_ns = 160;
    line.lines[static_cast<std::size_t>(decoder_line::rts)] = true;
    line.lines[static_cast<std::size_t>(decoder_line::ri)] = false;
    line.changed = {decoder_line::rts};
    body.items.emplace_back(line);
    body.items.emplace_back(
        decoder_line_error_item{"p", 170, decoder_line_error::framing});
    body.items.emplace_back(
        decoder_bus_fault_item{"l", "s", 180, decoder_fault::line_held, 11, 4});
    body.items.emplace_back(decoder_rule_item{"p", 190, "r", "a"});
    body.items.emplace_back(decoder_time_item{200});
    body.items.emplace_back(
        decoder_lost_item{"p", decoder_direction::rx, 1, 2, 3, 4});
    body.data = bytes_of(4);
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(
        header_of(written.value()),
        std::string(
            R"({"op":"decoder.data","seq":4,"items":[)"
            R"({"item":"data","place":"p","direction":"tx","offset":7,)"
            R"("t_ns":100,"length":3},)"
            R"({"item":"data","place":"p","direction":"tx","offset":7,)"
            R"("t_ns":100,"end_ns":150,"length":1},)"
            R"({"item":"line","place":"p","t_ns":160,)"
            R"("lines":{"rts":true,"ri":false},"changed":["rts"]},)"
            R"({"item":"line_error","place":"p","t_ns":170,)"
            R"("kind":"framing"},)"
            R"({"item":"bus_fault","link":"l","sender":"s","t_ns":180,)"
            R"("fault":"line_held","offset":11,"length":4},)"
            R"({"item":"rule","place":"p","t_ns":190,"rule":"r",)"
            R"("action":"a"},)"
            R"({"item":"time","t_ns":200},)"
            R"({"item":"lost","place":"p","direction":"rx","from_offset":1,)"
            R"("bytes":2,"from_ns":3,"to_ns":4}]})"));
}

TEST_CASE("plugin api: decoder.data reads every kind of item from its text")
{
    for (const item_case& one : item_cases())
    {
        const auto read = accepted<decoder_data_message>(stream(item_text(one)),
                                                         bytes_of(one.blob));
        REQUIRE_EQ(read.items.size(), std::size_t{1});
        REQUIRE_EQ(read.items[0].index(),
                   static_cast<std::size_t>(&one - item_cases().data()));
        (void)round_trip(read);
    }
}

TEST_CASE("plugin api: an item of decoder.data needs each of its members")
{
    for (const item_case& one : item_cases())
    {
        for (const auto& member : one.members)
        {
            bool may_leave_out = false;
            for (const std::string_view name : one.optional)
                may_leave_out = may_leave_out || name == member.first;
            std::string where = "decoder.data.items[0].";
            where.append(member.first);
            if (may_leave_out)
            {
                (void)accepted<decoder_data_message>(
                    stream(item_text(one, member.first)), bytes_of(one.blob));
                continue;
            }
            require_item_refused(one.kind, member.first, {},
                                 where + ": is required");
            // A list where any other kind of value is wanted.
            if (member.first != "changed")
                require_item_refused(one.kind, member.first, "[]",
                                     where + ": must");
            require_item_refused(one.kind, member.first, "null",
                                 where + ": must");
        }
    }
}

TEST_CASE("plugin api: an item of decoder.data is checked as a whole")
{
    using M = decoder_data_message;
    require_refused<M>(stream(R"({"item":"time","t_ns":1})", 0),
                       "decoder.data.seq: must be a whole number from 1");
    REQUIRE_EQ(
        accepted<M>(stream(R"({"item":"time","t_ns":1})", max_api_number)).seq,
        max_api_number);
    require_refused<M>(
        stream(R"({"item":"time","t_ns":1})", max_api_number + 1),
        "decoder.data.seq: must be a whole number from 1");
    require_refused<M>(R"({"op":"decoder.data","items":[{"item":"time",)"
                       R"("t_ns":1}]})",
                       "decoder.data.seq: is required");
    require_refused<M>(R"({"op":"decoder.data","seq":1})",
                       "decoder.data.items: is required");
    require_refused<M>(R"({"op":"decoder.data","seq":1,"items":{}})",
                       "decoder.data.items: must be a list");
    require_refused<M>(stream(""), "decoder.data.items: has no entries");
    require_refused<M>(stream("7"), "decoder.data.items[0]: must be an object");
    require_refused<M>(stream(R"({"t_ns":1})"),
                       "decoder.data.items[0].item: is required");
    require_refused<M>(stream(R"({"item":1})"),
                       "decoder.data.items[0].item: must be data, line");
    // A closed list.
    require_refused<M>(stream(R"({"item":"frame","t_ns":1})"),
                       "decoder.data.items[0].item: must be data, line, "
                       "line_error, bus_fault, rule, time or lost");
    require_refused<M>(stream(R"({"item":"Time","t_ns":1})"),
                       "decoder.data.items[0].item: must be data");
    // The entry that is wrong is the one named.
    require_refused<M>(stream(R"({"item":"time","t_ns":1},{"item":"time"})"),
                       "decoder.data.items[1].t_ns: is required");
    // A member this version does not know is passed over.
    (void)accepted<M>(stream(R"({"item":"time","t_ns":1,"colour":"red"})"));
}

TEST_CASE("plugin api: the blob of decoder.data is the bytes of its data items")
{
    using M = decoder_data_message;
    const item_case& data = item_named("data");
    const std::string two = item_text(data);
    // No data item, no bytes.
    (void)accepted<M>(stream(R"({"item":"time","t_ns":1})"));
    require_refused<M>(stream(R"({"item":"time","t_ns":1})"),
                       "decoder.data.items: have data lengths that do not add "
                       "up to the blob",
                       bytes_of(1));
    // The lengths must add up to the size, exactly.
    require_refused<M>(stream(two),
                       "decoder.data.items: have data lengths that do not add "
                       "up to the blob");
    require_refused<M>(stream(two), "do not add up to the blob", bytes_of(1));
    require_refused<M>(stream(two), "do not add up to the blob", bytes_of(3));
    require_refused<M>(stream(two + "," + two), "do not add up", bytes_of(3));
    (void)accepted<M>(stream(two + "," + two), bytes_of(4));
    (void)accepted<M>(stream(two + R"(,{"item":"time","t_ns":1},)" + two),
                      bytes_of(4));
    // Four chunks of 64 KiB fill the blob.
    const std::string big =
        item_text(data, "length", number_text(max_decoder_chunk));
    (void)accepted<M>(stream(big + "," + big + "," + big + "," + big),
                      bytes_of(max_decoder_blob));
    require_refused<M>(stream(big + "," + big + "," + big + "," + big),
                       "has a blob of 262145 bytes",
                       bytes_of(max_decoder_blob + 1));
    require_refused<M>(
        stream(big + "," + big + "," + big + "," + big + "," + item_text(data)),
        "has a blob of 262146 bytes", bytes_of(max_decoder_blob + 2));
    // A chunk is 1 to 64 KiB.
    require_item_refused("data", "length", "0",
                         "decoder.data.items[0].length: must be a whole "
                         "number from 1 to 65536");
    require_item_refused("data", "length", "65537",
                         "decoder.data.items[0].length: must be a whole "
                         "number from 1 to 65536");
    // A header may not carry bytes.
    (void)accepted<M>(
        stream(item_text(data)).replace(1, 0, R"("data":"0102",)"),
        bytes_of(2));
}

TEST_CASE("plugin api: decoder.data has the limits of its items")
{
    using M = decoder_data_message;
    // The number of items.
    std::string items;
    for (std::size_t i = 0; i < max_decoder_items; ++i)
    {
        if (i != 0)
            items += ',';
        items += R"({"item":"time","t_ns":1})";
    }
    REQUIRE_EQ(accepted<M>(stream(items)).items.size(), max_decoder_items);
    require_refused<M>(stream(items + R"(,{"item":"time","t_ns":1})"),
                       "decoder.data.items: has more than 256 entries");
    M body = accepted<M>(stream(items));
    (void)round_trip(body);
    body.items.emplace_back(decoder_time_item{1});
    require_not_written(body);
    body.items.clear();
    require_not_written(body);
    body.items.emplace_back(decoder_time_item{1});
    body.seq = 0;
    require_not_written(body);
    body.seq = 1;
    (void)round_trip(body);
    body.data = bytes_of(1);
    require_not_written(body);

    // The numbers.
    (void)accepted<M>(stream(R"({"item":"time","t_ns":)" +
                             number_text(max_api_number) + "}"));
    require_refused<M>(stream(R"({"item":"time","t_ns":)" +
                              number_text(max_api_number + 1) + "}"),
                       "decoder.data.items[0].t_ns: must be a whole number");
    require_item_refused("time", "t_ns", "-1",
                         "decoder.data.items[0].t_ns: must be a whole number");
    require_item_refused("time", "t_ns", "1.5",
                         "decoder.data.items[0].t_ns: must be a whole number");
    require_item_refused("time", "t_ns", R"("1")",
                         "decoder.data.items[0].t_ns: must be a whole number");
    require_item_refused("lost", "bytes", "0",
                         "decoder.data.items[0].bytes: must be a whole number "
                         "from 1");
    (void)accepted<M>(stream(
        item_text(item_named("lost"), "bytes", number_text(max_api_number))));
    require_item_refused("lost", "bytes", number_text(max_api_number + 1),
                         "decoder.data.items[0].bytes: must be a whole number");
}

TEST_CASE("plugin api: the times of an item of decoder.data are in order")
{
    using M = decoder_data_message;
    const item_case& data = item_named("data");
    // end_ns is not before t_ns.
    (void)accepted<M>(stream(item_text(data, "end_ns", "1")), bytes_of(2));
    require_item_refused("data", "end_ns", "0",
                         "decoder.data.items[0].end_ns: must not be before "
                         "t_ns");
    // The same for a lost span.
    const item_case& lost = item_named("lost");
    (void)accepted<M>(stream(item_text(lost, "to_ns", "1")));
    require_item_refused("lost", "to_ns", "0",
                         "decoder.data.items[0].to_ns: must not be before "
                         "from_ns");

    decoder_data_item one;
    one.place = "p";
    one.t_ns = 5;
    one.end_ns = 4;
    one.length = 1;
    decoder_data_message body;
    body.seq = 1;
    body.items.emplace_back(one);
    body.data = bytes_of(1);
    require_not_written(body);
    body.items[0] = decoder_lost_item{"p", decoder_direction::rx, 0, 1, 5, 4};
    body.data.clear();
    require_not_written(body);
}

TEST_CASE("plugin api: the names and kinds of the items are checked")
{
    using M = decoder_data_message;
    require_item_refused("data", "place", R"("")",
                         "decoder.data.items[0].place: must not be empty");
    require_item_refused("data", "place",
                         "\"" + repeated('p', max_api_name + 1) + "\"",
                         "decoder.data.items[0].place: is longer than 128 "
                         "bytes");
    (void)accepted<M>(
        stream(item_text(item_named("data"), "place",
                         "\"" + repeated('p', max_api_name) + "\"")),
        bytes_of(2));
    require_item_refused("data", "place", "\"a\\tb\"",
                         "decoder.data.items[0].place: must be plain text");
    require_item_refused("data", "direction", R"("both")",
                         "decoder.data.items[0].direction: must be rx or tx");
    require_item_refused("lost", "direction", R"("RX")",
                         "decoder.data.items[0].direction: must be rx or tx");
    require_item_refused("line_error", "kind", R"("noise")",
                         "decoder.data.items[0].kind: must be parity, framing, "
                         "overrun or break");
    for (const std::string_view kind :
         {"parity", "framing", "overrun", "break"})
    {
        (void)accepted<M>(stream(item_text(item_named("line_error"), "kind",
                                           "\"" + std::string(kind) + "\"")));
    }
    require_item_refused("bus_fault", "fault", R"("collision")",
                         "decoder.data.items[0].fault: must be overlap, "
                         "too_soon, line_held, transmitter_off or overrun");
    for (const std::string_view fault :
         {"overlap", "too_soon", "line_held", "transmitter_off", "overrun"})
    {
        (void)accepted<M>(stream(item_text(item_named("bus_fault"), "fault",
                                           "\"" + std::string(fault) + "\"")));
    }
    require_item_refused("bus_fault", "length", "0",
                         "decoder.data.items[0].length: must be a whole number "
                         "from 1 to 65536");
    require_item_refused("bus_fault", "link", R"("")",
                         "decoder.data.items[0].link: must not be empty");
    require_item_refused("bus_fault", "sender", R"("")",
                         "decoder.data.items[0].sender: must not be empty");
    // A rule and its action are plain names of at most 64 bytes.
    require_item_refused("rule", "rule",
                         "\"" + repeated('r', max_decoder_name + 1) + "\"",
                         "decoder.data.items[0].rule: is longer than 64 bytes");
    require_item_refused("rule", "action",
                         "\"" + repeated('r', max_decoder_name + 1) + "\"",
                         "decoder.data.items[0].action: is longer than 64 "
                         "bytes");
    (void)accepted<M>(
        stream(item_text(item_named("rule"), "rule",
                         "\"" + repeated('r', max_decoder_name) + "\"")));
    require_item_refused("rule", "rule", R"("")",
                         "decoder.data.items[0].rule: must not be empty");
    require_item_refused("rule", "action", "\"a\\u202eb\"",
                         "decoder.data.items[0].action: must be plain text");
    require_item_refused("rule", "rule", "\"a\\nb\"",
                         "decoder.data.items[0].rule: must be plain text");
}

TEST_CASE("plugin api: the lines of a line item are checked")
{
    using M = decoder_data_message;
    const item_case& line = item_named("line");
    // Every line, and any of them left out.
    const auto all = accepted<M>(stream(item_text(
        line, "lines",
        R"({"dtr":true,"rts":false,"cts":true,"dsr":false,"dcd":true,"ri":false})")));
    const auto& all_lines = std::get<decoder_line_item>(all.items[0]);
    for (std::size_t i = 0; i < max_decoder_lines; ++i)
        REQUIRE(all_lines.lines[i] == std::optional<bool>(i % 2 == 0));
    (void)round_trip(all);
    const auto some =
        accepted<M>(stream(item_text(line, "lines", R"({"dtr":true,"x":1})")));
    REQUIRE(std::get<decoder_line_item>(some.items[0]).lines[1] ==
            std::optional<bool>());
    (void)accepted<M>(stream(
        R"({"item":"line","place":"p","t_ns":1,"lines":{},"changed":[]})"));
    require_item_refused("line", "lines", R"({"dtr":1})",
                         "decoder.data.items[0].lines.dtr: must be true or "
                         "false");
    require_item_refused("line", "lines", "[]",
                         "decoder.data.items[0].lines: must be an object");

    // `changed` names lines that `lines` gives, none twice, at most six.
    require_item_refused("line", "changed", R"(["tx"])",
                         "decoder.data.items[0].changed[0]: must be dtr, rts, "
                         "cts, dsr, dcd or ri");
    require_item_refused("line", "changed", "[7]",
                         "decoder.data.items[0].changed[0]: must be dtr");
    require_item_refused("line", "changed", R"(["rts"])",
                         "decoder.data.items[0].changed[0]: is not a line that "
                         "lines gives");
    require_item_refused("line", "changed", R"(["dtr","dtr"])",
                         "decoder.data.items[0].changed[1]: names a line "
                         "twice");
    require_item_refused("line", "changed", R"({})",
                         "decoder.data.items[0].changed: must be a list");
    require_item_refused(
        "line", "changed", R"(["dtr","rts","cts","dsr","dcd","ri","dtr"])",
        "decoder.data.items[0].changed: has more than 6 entries");

    decoder_line_item one;
    one.place = "p";
    decoder_data_message body;
    body.seq = 1;
    body.items.emplace_back(one);
    (void)round_trip(body);
    one.changed = {decoder_line::dtr};
    body.items[0] = one;
    require_not_written(body);
    one.lines[0] = true;
    body.items[0] = one;
    (void)round_trip(body);
    one.changed = {decoder_line::dtr, decoder_line::dtr};
    body.items[0] = one;
    require_not_written(body);
}

TEST_CASE("plugin api: decoder.data is refused with the wrong role or no seq")
{
    using M = decoder_data_message;
    require_refused<M>(R"({"op":"decoder.data","id":3,"seq":1,"items":)"
                       R"([{"item":"time","t_ns":1}]})",
                       "neither an id nor a re");
    require_refused<M>(R"({"op":"decoder.ack","seq":1})",
                       "is not the message in this frame");
}

// -- decoder.ack and decoder.stop
// ------------------------------------------------

TEST_CASE("plugin api: decoder.ack is written and read")
{
    using M = decoder_ack_message;
    M body;
    body.seq = 77;
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"decoder.ack","seq":77})"));
    REQUIRE_EQ(accepted<M>(R"({"op":"decoder.ack","seq":1})").seq,
               std::uint64_t{1});
    REQUIRE_EQ(accepted<M>(R"({"op":"decoder.ack","seq":)" +
                           number_text(max_api_number) + "}")
                   .seq,
               max_api_number);
    require_refused<M>(R"({"op":"decoder.ack"})",
                       "decoder.ack.seq: is required");
    require_refused<M>(R"({"op":"decoder.ack","seq":0})",
                       "decoder.ack.seq: must be a whole number from 1");
    require_refused<M>(R"({"op":"decoder.ack","seq":)" +
                           number_text(max_api_number + 1) + "}",
                       "decoder.ack.seq: must be a whole number from 1");
    require_refused<M>(R"({"op":"decoder.ack","seq":"1"})",
                       "decoder.ack.seq: must be a whole number");
    require_refused<M>(R"({"op":"decoder.ack","seq":1})", "has no bytes", {1});
    require_refused<M>(R"({"op":"decoder.ack","id":2,"seq":1})",
                       "neither an id nor a re");
    body.seq = 0;
    require_not_written(body);
    body.seq = max_api_number + 1;
    require_not_written(body);
    body.seq = 1;
    require_not_written(body, 4);
}

TEST_CASE("plugin api: decoder.stop is written and read")
{
    using M = decoder_stop_message;
    M body;
    body.instance = 8;
    (void)round_trip(body, 2);
    const auto written = encode_as(body, 2);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"decoder.stop","id":2,"instance":8})"));
    require_refused<M>(R"({"op":"decoder.stop","id":2})",
                       "decoder.stop.instance: is required");
    require_refused<M>(R"({"op":"decoder.stop","id":2,"instance":0})",
                       "decoder.stop.instance: must be a whole number");
    require_refused<M>(R"({"op":"decoder.stop","id":2,"instance":8})",
                       "has no bytes", {1});
    require_refused<M>(R"({"op":"decoder.stop","instance":8})",
                       "must have an id");
    body.instance = 0;
    require_not_written(body, 2);
}

// -- decoder.frame
// ---------------------------------------------------------------

TEST_CASE("plugin api: decoder.frame is written and read")
{
    const decoder_frame_message body = sample_frame();
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(
                   R"({"op":"decoder.frame","instance":3,"place":"bus-1",)"
                   R"("direction":"rx","offset":48213,"length":10,)"
                   R"("t_start_ns":18203200000,"t_end_ns":18204113250,)"
                   R"("message":"read reply","summary":"read reply, 3 values",)"
                   R"("fields":[{"name":"address","offset":0,"length":1,)"
                   R"("value":"5"},{"name":"check","offset":7,"length":2,)"
                   R"("value":"3A91"}]})"));
}

TEST_CASE("plugin api: decoder.frame has defaults for what it leaves out")
{
    const auto read = accepted<decoder_frame_message>(frame_head + "}");
    REQUIRE(read.summary.empty());
    REQUIRE(read.fields.empty());
    decoder_frame_message body = read;
    (void)round_trip(body);
    body.fields = {{"x", 0, 0, ""}};
    (void)round_trip(body);
}

TEST_CASE("plugin api: decoder.frame is refused when it is wrong")
{
    using M = decoder_frame_message;
    const auto bad = [](std::string_view from, std::string_view to)
    {
        std::string text = frame_head;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text + "}";
    };
    require_refused<M>(bad(R"("instance":3,)", ""),
                       "decoder.frame.instance: is required");
    require_refused<M>(bad(R"("place":"p",)", ""),
                       "decoder.frame.place: is required");
    require_refused<M>(bad(R"("direction":"rx",)", ""),
                       "decoder.frame.direction: is required");
    require_refused<M>(bad(R"("direction":"rx")", R"("direction":"in")"),
                       "decoder.frame.direction: must be rx or tx");
    require_refused<M>(bad(R"("offset":0,)", ""),
                       "decoder.frame.offset: is required");
    require_refused<M>(bad(R"("length":10,)", ""),
                       "decoder.frame.length: is required");
    require_refused<M>(bad(R"("length":10)", R"("length":0)"),
                       "decoder.frame.length: must be a whole number from 1 "
                       "to 65536");
    require_refused<M>(bad(R"("length":10)", R"("length":65537)"),
                       "decoder.frame.length: must be a whole number from 1 "
                       "to 65536");
    require_refused<M>(bad(R"("t_start_ns":1,)", ""),
                       "decoder.frame.t_start_ns: is required");
    require_refused<M>(bad(R"("t_end_ns":2)", R"("t_end_ns":0)"),
                       "decoder.frame.t_end_ns: must not be before "
                       "t_start_ns");
    require_refused<M>(bad(R"("t_end_ns":2,)", ""),
                       "decoder.frame.t_end_ns: is required");
    require_refused<M>(bad(R"(,"message":"m")", ""),
                       "decoder.frame.message: is required");
    require_refused<M>(bad(R"("message":"m")", R"("message":"")"),
                       "decoder.frame.message: must not be empty");
    require_refused<M>(bad(R"("message":"m")", R"("message":5)"),
                       "decoder.frame.message: must be a string");
    require_refused<M>(bad(R"("message":"m")", "\"message\":\"a\\nb\""),
                       "decoder.frame.message: must be plain text");
    require_refused<M>(
        bad(R"("message":"m")", "\"message\":\"" + repeated('m', 65) + "\""),
        "decoder.frame.message: is longer than 64 bytes");
    require_refused<M>(
        bad(R"("message":"m")", "\"message\":\"m\",\"summary\":\"a\\u202eb\""),
        "decoder.frame.summary: must be plain text");
    require_refused<M>(bad(R"("message":"m")", R"("message":"m","summary":7)"),
                       "decoder.frame.summary: must be a string");
    require_refused<M>(bad(R"("message":"m")", R"("message":"m","fields":{})"),
                       "decoder.frame.fields: must be a list");
    require_refused<M>(bad(R"("message":"m")", R"("message":"m","fields":[1])"),
                       "decoder.frame.fields[0]: must be an object");
    require_refused<M>(frame_head + "}", "has no bytes", {1});
    require_refused<M>(
        bad(R"({"op":"decoder.frame",)", R"({"op":"decoder.frame","id":1,)"),
        "neither an id nor a re");
}

TEST_CASE("plugin api: decoder.frame has the limits of its members")
{
    using M = decoder_frame_message;
    const auto with = [](std::string_view more)
    { return frame_head + "," + std::string(more) + "}"; };
    const std::string text_edge = repeated('s', max_api_text);
    REQUIRE_EQ(
        accepted<M>(with("\"summary\":\"" + text_edge + "\"")).summary.size(),
        max_api_text);
    require_refused<M>(with("\"summary\":\"" + text_edge + "s\""),
                       "decoder.frame.summary: is longer than 200 bytes");
    REQUIRE(accepted<M>(with(R"("summary":"")")).summary.empty());
    REQUIRE_EQ(accepted<M>(std::string(R"({"op":"decoder.frame","instance":3,)"
                                       R"("place":"p","direction":"tx",)"
                                       R"("offset":)") +
                           number_text(max_api_number) +
                           R"(,"length":65536,"t_start_ns":)" +
                           number_text(max_api_number) + R"(,"t_end_ns":)" +
                           number_text(max_api_number) + R"(,"message":")" +
                           repeated('m', max_decoder_name) + R"("})")
                   .offset,
               max_api_number);

    // The fields.
    const std::string field_head =
        R"(,"fields":[{"name":"f","offset":0,"length":10,"value":"v"})";
    const auto fields = [&](std::string_view more)
    {
        return frame_head + std::string(field_head) + "," + std::string(more) +
               "]}";
    };
    REQUIRE_EQ(accepted<M>(frame_head + field_head + "]}").fields.size(),
               std::size_t{1});
    require_refused<M>(
        fields(R"({"name":"g","offset":0,"length":11,"value":""})"),
        "decoder.frame.fields[1]: does not lie within the "
        "frame");
    require_refused<M>(
        fields(R"({"name":"g","offset":10,"length":1,"value":""})"),
        "decoder.frame.fields[1]: does not lie within the "
        "frame");
    require_refused<M>(
        fields(R"({"name":"g","offset":11,"length":0,"value":""})"),
        "decoder.frame.fields[1]: does not lie within the "
        "frame");
    (void)accepted<M>(
        fields(R"({"name":"g","offset":10,"length":0,"value":""})"));
    (void)accepted<M>(
        fields(R"({"name":"g","offset":9,"length":1,"value":""})"));
    require_refused<M>(fields(R"({"offset":1,"length":1,"value":""})"),
                       "decoder.frame.fields[1].name: is required");
    require_refused<M>(
        fields(R"({"name":"","offset":1,"length":1,"value":""})"),
        "decoder.frame.fields[1].name: must not be empty");
    require_refused<M>(fields("{\"name\":\"" + repeated('n', 65) +
                              "\",\"offset\":1,\"length\":1,\"value\":\"\"}"),
                       "decoder.frame.fields[1].name: is longer than 64 bytes");
    require_refused<M>(fields(R"({"name":"g","length":1,"value":""})"),
                       "decoder.frame.fields[1].offset: is required");
    require_refused<M>(fields(R"({"name":"g","offset":1,"value":""})"),
                       "decoder.frame.fields[1].length: is required");
    require_refused<M>(
        fields(R"({"name":"g","offset":1,"length":65537,"value":""})"),
        "decoder.frame.fields[1].length: must be a whole number from 0 to "
        "65536");
    require_refused<M>(fields(R"({"name":"g","offset":1,"length":1})"),
                       "decoder.frame.fields[1].value: is required");
    require_refused<M>(
        fields("{\"name\":\"g\",\"offset\":1,\"length\":1,\"value\":\"" +
               repeated('v', max_api_text + 1) + "\"}"),
        "decoder.frame.fields[1].value: is longer than 200 bytes");
    require_refused<M>(
        fields(R"({"name":"g","offset":1,"length":1,"value":"a\u0007"})"),
        "decoder.frame.fields[1].value: must be plain text");
    require_refused<M>(
        fields(R"({"name":"g","offset":1,"length":1,"value":5})"),
        "decoder.frame.fields[1].value: must be a string");
    (void)accepted<M>(
        fields("{\"name\":\"g\",\"offset\":1,\"length\":1,\"value\":\"" +
               repeated('v', max_api_text) + "\"}"));

    // At most 64 fields.
    std::string many;
    for (std::size_t i = 0; i < max_decoder_fields; ++i)
    {
        if (i != 0)
            many += ',';
        many += R"({"name":"f","offset":0,"length":1,"value":""})";
    }
    REQUIRE_EQ(
        accepted<M>(frame_head + R"(,"fields":[)" + many + "]}").fields.size(),
        max_decoder_fields);
    require_refused<M>(
        frame_head + R"(,"fields":[)" + many +
            R"(,{"name":"f","offset":0,"length":1,"value":""}]})",
        "decoder.frame.fields: has more than 64 entries");

    // What cannot be written.
    decoder_frame_message body = sample_frame();
    body.fields.assign(max_decoder_fields, decoder_field{"f", 0, 1, ""});
    (void)round_trip(body);
    body.fields.push_back({"f", 0, 1, ""});
    require_not_written(body);
    body = sample_frame();
    body.fields = {{"f", 9, 2, ""}};
    require_not_written(body);
    body.fields = {{"", 0, 1, ""}};
    require_not_written(body);
    body.fields = {{"f", 0, 1, repeated('v', max_api_text + 1)}};
    require_not_written(body);
    body = sample_frame();
    body.length = 0;
    require_not_written(body);
    body = sample_frame();
    body.t_end_ns = body.t_start_ns - 1;
    require_not_written(body);
    body = sample_frame();
    body.message.clear();
    require_not_written(body);
    body = sample_frame();
    body.summary = "a\nb";
    require_not_written(body);
    body = sample_frame();
    body.instance = 0;
    require_not_written(body);
    body.instance = 3;
    require_not_written(body, 2);
}
