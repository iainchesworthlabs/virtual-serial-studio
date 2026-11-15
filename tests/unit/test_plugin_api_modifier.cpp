//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_modifier.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

using namespace vss;
using namespace vss::plugin;
using namespace vss::plugin_api_test;

namespace
{

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

/// The text of a header, one member at a time, so that a test changes or
/// leaves out one.  An empty value leaves the member out.
std::string member_text(std::string_view key, const std::string& value)
{
    if (value.empty())
        return {};
    std::string out = ",\"";
    out.append(key);
    out.append("\":");
    out.append(value);
    return out;
}

struct create_text
{
    std::string instance = "3";
    std::string place = R"("link-9")";
    std::string direction = R"("a_to_b")";
    std::string params;
    std::string budget = "2000";
    std::string bytes = "4096";
    std::string us = "200";
    std::string timeout = R"("fail_open")";
    std::string flush;
    std::string id = R"(,"id":1)";

    std::string str() const
    {
        std::string out = R"({"op":"modifier.create")" + id;
        out += member_text("instance", instance);
        out += member_text("place", place);
        out += member_text("direction", direction);
        out += member_text("params", params);
        out += member_text("budget_us", budget);
        out += member_text("max_batch_bytes", bytes);
        out += member_text("max_batch_us", us);
        out += member_text("on_timeout", timeout);
        out += member_text("flush_on", flush);
        out += "}";
        return out;
    }
};

struct batch_text
{
    std::string instance = "3";
    std::string seq = "17";
    std::string ts = "1000";
    std::string chunks = R"([{"offset":0,"length":8,"ts_ns":1000}])";

    std::string str() const
    {
        std::string out = R"({"op":"modifier.batch")";
        out += member_text("instance", instance);
        out += member_text("seq", seq);
        out += member_text("ts_ns", ts);
        out += member_text("chunks", chunks);
        out += "}";
        return out;
    }
};

struct result_text
{
    std::string instance = "3";
    std::string seq = "17";
    std::string actions = R"([{"action":"pass"}])";

    std::string str() const
    {
        std::string out = R"({"op":"modifier.result")";
        out += member_text("instance", instance);
        out += member_text("seq", seq);
        out += member_text("actions", actions);
        out += "}";
        return out;
    }
};

/// A batch of one chunk with `bytes` bytes.
std::string chunk_text(std::size_t offset, std::size_t length, std::uint64_t ts)
{
    return "{\"offset\":" + std::to_string(offset) +
           ",\"length\":" + std::to_string(length) +
           ",\"ts_ns\":" + std::to_string(ts) + "}";
}

modifier_create_message create_body()
{
    modifier_create_message body;
    body.instance = 3;
    body.place = "link-9";
    body.direction = modifier_direction::b_to_a;
    body.params = R"({"rate":9600})";
    body.budget_us = 2000;
    body.max_batch_bytes = 4096;
    body.max_batch_us = 200;
    body.on_timeout = timeout_action::fail_closed;
    body.flush_on = 0x0a;
    return body;
}

modifier_batch_message batch_body()
{
    modifier_batch_message body;
    body.instance = 3;
    body.seq = 17;
    body.ts_ns = 1000;
    body.chunks = {{0, 4, 1000}, {4, 2, 1500}, {10, 3, 1500}};
    body.data = bytes_of(13);
    return body;
}

modifier_action action_of(modifier_action_kind kind)
{
    modifier_action out;
    out.kind = kind;
    return out;
}

modifier_action range_of(modifier_action_kind kind, std::uint64_t offset,
                         std::uint64_t length)
{
    modifier_action out = action_of(kind);
    out.offset = offset;
    out.length = length;
    return out;
}

modifier_action delay_of(std::uint64_t us)
{
    modifier_action out = action_of(modifier_action_kind::delay_us);
    out.us = us;
    return out;
}

modifier_action split_of(std::uint64_t parts, std::uint64_t gap_us)
{
    modifier_action out = action_of(modifier_action_kind::split);
    out.parts = parts;
    out.gap_us = gap_us;
    return out;
}

modifier_result_message result_body()
{
    modifier_result_message body;
    body.instance = 3;
    body.seq = 17;
    body.data = bytes_of(10);
    body.actions = {action_of(modifier_action_kind::pass),
                    action_of(modifier_action_kind::drop),
                    range_of(modifier_action_kind::replace, 0, 4),
                    delay_of(250),
                    split_of(3, 100),
                    range_of(modifier_action_kind::insert_before, 4, 2),
                    range_of(modifier_action_kind::insert_after, 6, 4)};
    modifier_action fault = action_of(modifier_action_kind::fault);
    fault.fault = line_fault::break_signal;
    body.actions.push_back(fault);
    return body;
}

} // namespace

TEST_CASE("plugin api: modifier.create is written and read")
{
    const modifier_create_message body = create_body();
    const auto read = round_trip(body, 5);
    REQUIRE(read.flush_on.has_value());
    REQUIRE_EQ(static_cast<int>(read.flush_on.value_or(0)), 10);
    const auto written = encode_as(body, 5);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"modifier.create","id":5,"instance":3,)"
                           R"("place":"link-9","direction":"b_to_a",)"
                           R"("params":{"rate":9600},"budget_us":2000,)"
                           R"("max_batch_bytes":4096,"max_batch_us":200,)"
                           R"("on_timeout":"fail_closed","flush_on":10})"));
    const auto plain = accepted<modifier_create_message>(create_text{}.str());
    REQUIRE(!plain.flush_on.has_value());
    REQUIRE_EQ(plain.params, std::string("{}"));
    REQUIRE(plain.direction == modifier_direction::a_to_b);
    REQUIRE(plain.on_timeout == timeout_action::fail_open);
    // Without a flush byte nothing is written for it.
    modifier_create_message none = create_body();
    none.flush_on.reset();
    const auto short_form = encode_as(none, 5);
    REQUIRE(short_form.has_value());
    REQUIRE(header_of(short_form.value()).find("flush_on") ==
            std::string::npos);
    REQUIRE_EQ(std::string(to_string(modifier_direction::a_to_b)),
               std::string("a_to_b"));
    REQUIRE_EQ(std::string(to_string(modifier_direction::b_to_a)),
               std::string("b_to_a"));
    REQUIRE_EQ(std::string(to_string(timeout_action::fail_open)),
               std::string("fail_open"));
    REQUIRE_EQ(std::string(to_string(timeout_action::fail_closed)),
               std::string("fail_closed"));
}

TEST_CASE("plugin api: modifier.create is refused when it is wrong")
{
    using message_type = modifier_create_message;
    const std::string op = "modifier.create.";
    create_text text;
    text.instance.clear();
    require_refused<message_type>(text.str(), op + "instance: is required");
    text = {};
    text.instance = "0";
    require_refused<message_type>(text.str(),
                                  op + "instance: must be a whole number");
    text = {};
    text.place.clear();
    require_refused<message_type>(text.str(), op + "place: is required");
    text = {};
    text.place = "9";
    require_refused<message_type>(text.str(), op + "place: must be a string");
    text.place = R"("")";
    require_refused<message_type>(text.str(), op + "place: must not be empty");
    text = {};
    text.direction.clear();
    require_refused<message_type>(text.str(), op + "direction: is required");
    text = {};
    text.direction = R"("both")";
    require_refused<message_type>(text.str(),
                                  op + "direction: must be \"a_to_b\"");
    text = {};
    text.direction = "1";
    require_refused<message_type>(text.str(), op + "direction: must be");
    text = {};
    text.budget.clear();
    require_refused<message_type>(text.str(), op + "budget_us: is required");
    text = {};
    text.bytes.clear();
    require_refused<message_type>(text.str(),
                                  op + "max_batch_bytes: is required");
    text = {};
    text.us.clear();
    require_refused<message_type>(text.str(), op + "max_batch_us: is required");
    text = {};
    text.timeout.clear();
    require_refused<message_type>(text.str(), op + "on_timeout: is required");
    text = {};
    text.timeout = R"("fail_shut")";
    require_refused<message_type>(text.str(),
                                  op + "on_timeout: must be \"fail_open\"");
    text = {};
    text.params = R"("x")";
    require_refused<message_type>(text.str(),
                                  op + "params: must be a JSON object");
    text = {};
    text.flush = R"("\n")";
    require_refused<message_type>(text.str(),
                                  op + "flush_on: must be a whole number");
    text = {};
    require_refused<message_type>(text.str(), "has no bytes", {1});
    text.id.clear();
    require_refused<message_type>(text.str(), "must have an id");
}

TEST_CASE("plugin api: modifier.create has the limits of its members")
{
    using message_type = modifier_create_message;
    const std::string op = "modifier.create.";
    create_text text;
    text.budget = "1";
    REQUIRE_EQ(accepted<message_type>(text.str()).budget_us, std::uint64_t{1});
    text.budget = "1000000";
    REQUIRE_EQ(accepted<message_type>(text.str()).budget_us,
               max_modifier_budget_us);
    text.budget = "0";
    require_refused<message_type>(text.str(),
                                  op + "budget_us: must be a whole number");
    text.budget = "1000001";
    require_refused<message_type>(text.str(),
                                  op + "budget_us: must be a whole number");
    text = {};
    text.bytes = "1";
    REQUIRE_EQ(accepted<message_type>(text.str()).max_batch_bytes,
               std::uint64_t{1});
    text.bytes = "65536";
    REQUIRE_EQ(accepted<message_type>(text.str()).max_batch_bytes,
               max_modifier_batch);
    text.bytes = "0";
    require_refused<message_type>(text.str(),
                                  op + "max_batch_bytes: must be a whole");
    text.bytes = "65537";
    require_refused<message_type>(text.str(),
                                  op + "max_batch_bytes: must be a whole");
    text = {};
    text.us = "1";
    REQUIRE_EQ(accepted<message_type>(text.str()).max_batch_us,
               std::uint64_t{1});
    text.us = "1000000";
    REQUIRE_EQ(accepted<message_type>(text.str()).max_batch_us,
               max_modifier_batch_us);
    text.us = "0";
    require_refused<message_type>(text.str(),
                                  op + "max_batch_us: must be a whole");
    text.us = "1000001";
    require_refused<message_type>(text.str(),
                                  op + "max_batch_us: must be a whole");
    text = {};
    text.place = "\"" + std::string(max_api_name, 'p') + "\"";
    REQUIRE_EQ(accepted<message_type>(text.str()).place.size(), max_api_name);
    text.place = "\"" + std::string(max_api_name + 1, 'p') + "\"";
    require_refused<message_type>(text.str(),
                                  op + "place: is longer than 128 bytes");
    text = {};
    text.flush = "0";
    REQUIRE_EQ(static_cast<int>(
                   accepted<message_type>(text.str()).flush_on.value_or(9)),
               0);
    text.flush = "255";
    REQUIRE_EQ(static_cast<int>(
                   accepted<message_type>(text.str()).flush_on.value_or(9)),
               255);
    text.flush = "256";
    require_refused<message_type>(
        text.str(), op + "flush_on: must be a whole number from 0 to 255");
    text.flush = "-1";
    require_refused<message_type>(text.str(), op + "flush_on");

    message_type body = create_body();
    body.budget_us = 0;
    require_not_written(body, 5);
    body.budget_us = max_modifier_budget_us + 1;
    require_not_written(body, 5);
    body = create_body();
    body.max_batch_bytes = 0;
    require_not_written(body, 5);
    body.max_batch_bytes = max_modifier_batch + 1;
    require_not_written(body, 5);
    body = create_body();
    body.max_batch_us = 0;
    require_not_written(body, 5);
    body.max_batch_us = max_modifier_batch_us + 1;
    require_not_written(body, 5);
    body = create_body();
    body.place.clear();
    require_not_written(body, 5);
    body = create_body();
    body.params = "[]";
    require_not_written(body, 5);
    body = create_body();
    body.flush_on = 255;
    (void)round_trip(body, 5);
    require_not_written(body, 0);
}

TEST_CASE("plugin api: modifier.batch keeps its bytes in the blob")
{
    const modifier_batch_message body = batch_body();
    const auto read = round_trip(body);
    REQUIRE(read.data == body.data);
    REQUIRE_EQ(read.chunks.size(), std::size_t{3});
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"modifier.batch","instance":3,"seq":17,)"
                           R"("ts_ns":1000,"chunks":[)"
                           R"({"offset":0,"length":4,"ts_ns":1000},)"
                           R"({"offset":4,"length":2,"ts_ns":1500},)"
                           R"({"offset":10,"length":3,"ts_ns":1500}]})"));
}

TEST_CASE("plugin api: modifier.batch is refused when its members are wrong")
{
    using message_type = modifier_batch_message;
    const std::string op = "modifier.batch.";
    const byte_buffer data = bytes_of(8);
    REQUIRE_EQ(accepted<message_type>(batch_text{}.str(), data).chunks.size(),
               std::size_t{1});
    batch_text text;
    text.instance.clear();
    require_refused<message_type>(text.str(), op + "instance: is required",
                                  data);
    text = {};
    text.seq.clear();
    require_refused<message_type>(text.str(), op + "seq: is required", data);
    text.seq = "0";
    require_refused<message_type>(
        text.str(), op + "seq: must be a whole number from 1", data);
    text.seq = "9007199254740992";
    require_refused<message_type>(text.str(),
                                  op + "seq: must be a whole number", data);
    text.seq = "9007199254740991";
    REQUIRE_EQ(accepted<message_type>(text.str(), data).seq, max_api_number);
    text = {};
    text.ts.clear();
    require_refused<message_type>(text.str(), op + "ts_ns: is required", data);
    text.ts = "-1";
    require_refused<message_type>(text.str(),
                                  op + "ts_ns: must be a whole number", data);
    text = {};
    text.chunks.clear();
    require_refused<message_type>(text.str(), op + "chunks: is required", data);
    text.chunks = "{}";
    require_refused<message_type>(text.str(), op + "chunks: must be a list",
                                  data);
    text.chunks = "[]";
    require_refused<message_type>(
        text.str(), op + "chunks: must have at least 1 entry", data);
    text = {};
    require_refused<message_type>(text.str(), "has a blob of 0 bytes");
    require_refused<message_type>(text.str(), "has a blob of 65537 bytes",
                                  bytes_of(max_modifier_batch + 1));
    REQUIRE_EQ(accepted<message_type>(text.str(), bytes_of(max_modifier_batch))
                   .data.size(),
               max_modifier_batch);
    require_refused<message_type>(
        R"({"op":"modifier.batch","id":2,"instance":3,"seq":1,"ts_ns":1,)"
        R"("chunks":[{"offset":0,"length":1,"ts_ns":1}]})",
        "neither an id nor a re", data);
}

TEST_CASE("plugin api: the chunks of a modifier.batch are checked one by one")
{
    using message_type = modifier_batch_message;
    const std::string op = "modifier.batch.chunks[";
    const byte_buffer data = bytes_of(8);
    batch_text text;
    text.chunks = "[3]";
    require_refused<message_type>(text.str(), op + "0]: must be an object",
                                  data);
    text.chunks = R"([{"length":8,"ts_ns":1000}])";
    require_refused<message_type>(text.str(), op + "0].offset: is required",
                                  data);
    text.chunks = R"([{"offset":0,"ts_ns":1000}])";
    require_refused<message_type>(text.str(), op + "0].length: is required",
                                  data);
    text.chunks = R"([{"offset":0,"length":8}])";
    require_refused<message_type>(text.str(), op + "0].ts_ns: is required",
                                  data);
    text.chunks = R"([{"offset":-1,"length":8,"ts_ns":1000}])";
    require_refused<message_type>(text.str(), op + "0].offset: must be a whole",
                                  data);
    text.chunks = R"([{"offset":0,"length":0,"ts_ns":1000}])";
    require_refused<message_type>(
        text.str(), op + "0].length: must be a whole number from 1", data);
    text.chunks = R"([{"offset":"0","length":8,"ts_ns":1000}])";
    require_refused<message_type>(text.str(), op + "0].offset: must be a whole",
                                  data);
    text.chunks = R"([{"offset":0,"length":8.5,"ts_ns":1000}])";
    require_refused<message_type>(text.str(), op + "0].length: must be a whole",
                                  data);
    // Inside the blob: the last byte, and one past it.
    text.chunks = "[" + chunk_text(7, 1, 1000) + "]";
    REQUIRE_EQ(accepted<message_type>(text.str(), data).chunks.size(),
               std::size_t{1});
    text.chunks = "[" + chunk_text(7, 2, 1000) + "]";
    require_refused<message_type>(text.str(), op + "0]: is not inside the blob",
                                  data);
    text.chunks = "[" + chunk_text(8, 1, 1000) + "]";
    require_refused<message_type>(text.str(), op + "0]: is not inside the blob",
                                  data);
    text.chunks = "[" + chunk_text(0, 9, 1000) + "]";
    require_refused<message_type>(text.str(), op + "0]: is not inside the blob",
                                  data);
    // A range that would wrap if the sum were taken first.
    text.chunks = "[" + chunk_text(65536, 65536, 1000) + "]";
    require_refused<message_type>(text.str(), op + "0]: is not inside the blob",
                                  data);
    // In order, and not overlapping.
    text.chunks =
        "[" + chunk_text(0, 4, 1000) + "," + chunk_text(4, 4, 1000) + "]";
    REQUIRE_EQ(accepted<message_type>(text.str(), data).chunks.size(),
               std::size_t{2});
    text.chunks =
        "[" + chunk_text(0, 4, 1000) + "," + chunk_text(3, 4, 1000) + "]";
    require_refused<message_type>(
        text.str(), op + "1]: is not after the chunk before it", data);
    text.chunks =
        "[" + chunk_text(4, 4, 1000) + "," + chunk_text(0, 4, 1000) + "]";
    require_refused<message_type>(
        text.str(), op + "1]: is not after the chunk before it", data);
    // A gap between chunks is allowed.
    text.chunks =
        "[" + chunk_text(0, 2, 1000) + "," + chunk_text(6, 2, 1000) + "]";
    REQUIRE_EQ(accepted<message_type>(text.str(), data).chunks.size(),
               std::size_t{2});
    // Times do not go back, and the first is the time of the batch.
    text.chunks = "[" + chunk_text(0, 2, 1001) + "]";
    require_refused<message_type>(
        text.str(), op + "0].ts_ns: of the first chunk must be the ts_ns",
        data);
    text.chunks = "[" + chunk_text(0, 2, 999) + "]";
    require_refused<message_type>(
        text.str(), op + "0].ts_ns: of the first chunk must be the ts_ns",
        data);
    text.chunks =
        "[" + chunk_text(0, 2, 1000) + "," + chunk_text(2, 2, 999) + "]";
    require_refused<message_type>(
        text.str(), op + "1].ts_ns: must not be before the chunk before it",
        data);
    text.chunks = "[" + chunk_text(0, 2, 1000) + "," + chunk_text(2, 2, 1000) +
                  "," + chunk_text(4, 2, 2000) + "]";
    REQUIRE_EQ(accepted<message_type>(text.str(), data).chunks.size(),
               std::size_t{3});
}

TEST_CASE("plugin api: modifier.batch has between 1 and 256 chunks")
{
    using message_type = modifier_batch_message;
    const byte_buffer data = bytes_of(max_modifier_chunks + 10);
    const auto list = [](std::size_t count)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i != 0)
                out += ",";
            out += chunk_text(i, 1, 1000);
        }
        return out + "]";
    };
    batch_text text;
    text.chunks = list(max_modifier_chunks);
    REQUIRE_EQ(accepted<message_type>(text.str(), data).chunks.size(),
               max_modifier_chunks);
    text.chunks = list(max_modifier_chunks + 1);
    require_refused<message_type>(
        text.str(), "modifier.batch.chunks: has more than 256", data);
    // A list that is long and whose entries are all wrong is refused for its
    // length, before any entry is read.
    std::string bad = "[";
    for (std::size_t i = 0; i < max_modifier_chunks + 1; ++i)
        bad += i == 0 ? "7" : ",7";
    text.chunks = bad + "]";
    require_refused<message_type>(
        text.str(), "modifier.batch.chunks: has more than 256", data);

    modifier_batch_message body = batch_body();
    body.chunks.assign(max_modifier_chunks, {});
    for (std::size_t i = 0; i < max_modifier_chunks; ++i)
        body.chunks[i] = {i, 1, 1000};
    body.data = data;
    (void)round_trip(body);
    body.chunks.push_back({max_modifier_chunks, 1, 1000});
    require_not_written(body);
    body.chunks.clear();
    require_not_written(body);
    body = batch_body();
    body.data.clear();
    require_not_written(body);
    body = batch_body();
    body.ts_ns = 999;
    require_not_written(body);
    body = batch_body();
    body.chunks[1].offset = 3;
    require_not_written(body);
    body = batch_body();
    body.chunks[2].length = 4;
    require_not_written(body);
    body = batch_body();
    body.seq = 0;
    require_not_written(body);
    require_not_written(batch_body(), 1);
}

TEST_CASE("plugin api: modifier.result is written and read")
{
    const modifier_result_message body = result_body();
    const auto read = round_trip(body);
    REQUIRE(read.data == body.data);
    REQUIRE_EQ(read.actions.size(), body.actions.size());
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(
        header_of(written.value()),
        std::string(
            R"({"op":"modifier.result","instance":3,"seq":17,"actions":[)"
            R"({"action":"pass"},{"action":"drop"},)"
            R"({"action":"replace","offset":0,"length":4},)"
            R"({"action":"delay_us","us":250},)"
            R"({"action":"split","parts":3,"gap_us":100},)"
            R"({"action":"insert_before","offset":4,"length":2},)"
            R"({"action":"insert_after","offset":6,"length":4},)"
            R"({"action":"fault","fault":"break"}]})"));
    REQUIRE_EQ(std::string(to_string(modifier_action_kind::insert_before)),
               std::string("insert_before"));
    REQUIRE_EQ(std::string(to_string(modifier_action_kind::delay_us)),
               std::string("delay_us"));
}

TEST_CASE("plugin api: modifier.result has no bytes when no action needs them")
{
    modifier_result_message body;
    body.instance = 3;
    body.seq = 1;
    body.actions = {action_of(modifier_action_kind::pass)};
    const auto read = round_trip(body);
    REQUIRE(read.data.empty());
    // The blob may be four times the largest batch and 1 KiB.
    REQUIRE_EQ(max_modifier_result_blob, std::size_t{263168});
    body.data = bytes_of(max_modifier_result_blob);
    body.actions = {
        range_of(modifier_action_kind::replace, 0, max_modifier_result_blob)};
    (void)round_trip(body);
    body.data = bytes_of(max_modifier_result_blob + 1);
    body.actions = {action_of(modifier_action_kind::pass)};
    require_not_written(body);
    require_refused<modifier_result_message>(
        result_text{}.str(), "has a blob of 263169 bytes",
        bytes_of(max_modifier_result_blob + 1));
}

TEST_CASE("plugin api: modifier.result is refused when its members are wrong")
{
    using message_type = modifier_result_message;
    const std::string op = "modifier.result.";
    result_text text;
    REQUIRE_EQ(accepted<message_type>(text.str()).actions.size(),
               std::size_t{1});
    text.instance.clear();
    require_refused<message_type>(text.str(), op + "instance: is required");
    text = {};
    text.seq.clear();
    require_refused<message_type>(text.str(), op + "seq: is required");
    text.seq = "0";
    require_refused<message_type>(text.str(),
                                  op + "seq: must be a whole number from 1");
    text = {};
    text.actions.clear();
    require_refused<message_type>(text.str(), op + "actions: is required");
    text.actions = "5";
    require_refused<message_type>(text.str(), op + "actions: must be a list");
    text.actions = "[]";
    require_refused<message_type>(text.str(),
                                  op + "actions: must have at least 1 entry");
    text.actions = "[7]";
    require_refused<message_type>(text.str(),
                                  op + "actions[0]: must be an object");
    text.actions = "[{}]";
    require_refused<message_type>(text.str(),
                                  op + "actions[0].action: is required");
    text.actions = R"([{"action":"pass"},{"action":"explode"}])";
    require_refused<message_type>(text.str(),
                                  op + "actions[1].action: must be \"pass\"");
    text.actions = R"([{"action":4}])";
    require_refused<message_type>(text.str(),
                                  op + "actions[0].action: must be");
    text.actions = R"([{"action":"Pass"}])";
    require_refused<message_type>(text.str(),
                                  op + "actions[0].action: must be");
    text = {};
    require_refused<message_type>(
        R"({"op":"modifier.result","id":3,"instance":3,"seq":1,)"
        R"("actions":[{"action":"pass"}]})",
        "neither an id nor a re");
}

TEST_CASE("plugin api: the actions of a modifier.result have their members")
{
    using message_type = modifier_result_message;
    const std::string op = "modifier.result.actions[0].";
    const byte_buffer data = bytes_of(10);
    result_text text;
    const auto one = [&](std::string_view action)
    {
        text.actions = "[" + std::string(action) + "]";
        return text.str();
    };
    // Range actions.
    REQUIRE_EQ(accepted<message_type>(
                   one(R"({"action":"replace","offset":0,"length":0})"), data)
                   .actions[0]
                   .length,
               std::uint64_t{0});
    REQUIRE_EQ(accepted<message_type>(
                   one(R"({"action":"replace","offset":10,"length":0})"), data)
                   .actions[0]
                   .offset,
               std::uint64_t{10});
    require_refused<message_type>(one(R"({"action":"replace","offset":0})"),
                                  op + "length: is required", data);
    require_refused<message_type>(one(R"({"action":"replace","length":1})"),
                                  op + "offset: is required", data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":0,"length":-1})"),
        op + "length: must be a whole number", data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":0,"length":11})"),
        "modifier.result.actions[0]: has a range that is not "
        "inside the result blob",
        data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":11,"length":0})"),
        "has a range that is not inside the result blob", data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":6,"length":5})"),
        "has a range that is not inside the result blob", data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":0,"length":1})"),
        "has a range that is not inside the result blob");
    for (const std::string_view kind : {"insert_before", "insert_after"})
    {
        const std::string head = R"({"action":")" + std::string(kind) + "\"";
        REQUIRE_EQ(accepted<message_type>(
                       one(head + R"(,"offset":9,"length":1})"), data)
                       .actions[0]
                       .length,
                   std::uint64_t{1});
        require_refused<message_type>(
            one(head + R"(,"offset":0,"length":0})"),
            op + "length: must be a whole number from 1", data);
        require_refused<message_type>(
            one(head + R"(,"offset":9,"length":2})"),
            "has a range that is not inside the result blob", data);
        require_refused<message_type>(one(head + R"(,"length":1})"),
                                      op + "offset: is required", data);
    }
    // A range that is too large for the number is refused before it is added.
    require_refused<message_type>(
        one(R"({"action":"replace","offset":263168,"length":)"
            R"(263168})"),
        "has a range that is not inside the result blob", data);
    require_refused<message_type>(
        one(R"({"action":"replace","offset":263169,"length":0})"),
        op + "offset: must be a whole number", data);
    // Delay.
    REQUIRE_EQ(accepted<message_type>(one(R"({"action":"delay_us","us":1})"))
                   .actions[0]
                   .us,
               std::uint64_t{1});
    REQUIRE_EQ(
        accepted<message_type>(one(R"({"action":"delay_us","us":60000000})"))
            .actions[0]
            .us,
        max_api_delay_us);
    require_refused<message_type>(one(R"({"action":"delay_us","us":0})"),
                                  op + "us: must be a whole number from 1");
    require_refused<message_type>(one(R"({"action":"delay_us","us":60000001})"),
                                  op + "us: must be a whole number");
    require_refused<message_type>(one(R"({"action":"delay_us"})"),
                                  op + "us: is required");
    // Split.
    REQUIRE_EQ(accepted<message_type>(
                   one(R"({"action":"split","parts":2,"gap_us":0})"))
                   .actions[0]
                   .parts,
               std::uint64_t{2});
    REQUIRE_EQ(accepted<message_type>(
                   one(R"({"action":"split","parts":16,"gap_us":60000000})"))
                   .actions[0]
                   .gap_us,
               max_api_delay_us);
    require_refused<message_type>(
        one(R"({"action":"split","parts":1,"gap_us":0})"),
        op + "parts: must be a whole number from 2 to 16");
    require_refused<message_type>(
        one(R"({"action":"split","parts":17,"gap_us":0})"),
        op + "parts: must be a whole number from 2 to 16");
    require_refused<message_type>(
        one(R"({"action":"split","parts":2,"gap_us":60000001})"),
        op + "gap_us: must be a whole number");
    require_refused<message_type>(one(R"({"action":"split","gap_us":1})"),
                                  op + "parts: is required");
    require_refused<message_type>(one(R"({"action":"split","parts":2})"),
                                  op + "gap_us: is required");
    // Fault.
    REQUIRE(
        accepted<message_type>(one(R"({"action":"fault","fault":"hang_up"})"))
            .actions[0]
            .fault == line_fault::hang_up);
    require_refused<message_type>(one(R"({"action":"fault"})"),
                                  op + "fault: is required");
    require_refused<message_type>(one(R"({"action":"fault","fault":"smoke"})"),
                                  op + "fault: must be \"parity\"");
    // Pass and drop need nothing, and what else they carry is passed over.
    REQUIRE(accepted<message_type>(one(R"({"action":"pass","us":5})"))
                .actions[0]
                .kind == modifier_action_kind::pass);
    REQUIRE(
        accepted<message_type>(one(R"({"action":"drop"})")).actions[0].kind ==
        modifier_action_kind::drop);
}

TEST_CASE("plugin api: modifier.result has between 1 and 256 actions")
{
    using message_type = modifier_result_message;
    const auto list = [](std::size_t count)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < count; ++i)
            out += i == 0 ? R"({"action":"pass"})" : R"(,{"action":"pass"})";
        return out + "]";
    };
    result_text text;
    text.actions = list(max_modifier_chunks);
    REQUIRE_EQ(accepted<message_type>(text.str()).actions.size(),
               max_modifier_chunks);
    text.actions = list(max_modifier_chunks + 1);
    require_refused<message_type>(text.str(),
                                  "modifier.result.actions: has more than 256");
    std::string bad = "[";
    for (std::size_t i = 0; i < max_modifier_chunks + 1; ++i)
        bad += i == 0 ? "7" : ",7";
    text.actions = bad + "]";
    require_refused<message_type>(text.str(),
                                  "modifier.result.actions: has more than 256");

    message_type body = result_body();
    body.actions.assign(max_modifier_chunks,
                        action_of(modifier_action_kind::pass));
    (void)round_trip(body);
    body.actions.push_back(action_of(modifier_action_kind::pass));
    require_not_written(body);
    body.actions.clear();
    require_not_written(body);
    body = result_body();
    body.seq = 0;
    require_not_written(body);
    body = result_body();
    body.actions[2].length = 11;
    require_not_written(body);
    body = result_body();
    body.actions[3].us = 0;
    require_not_written(body);
    body = result_body();
    body.actions[4].parts = 1;
    require_not_written(body);
    body = result_body();
    body.actions[4].gap_us = max_api_delay_us + 1;
    require_not_written(body);
    body = result_body();
    body.actions[5].length = 0;
    require_not_written(body);
    body = result_body();
    body.actions[0].kind = static_cast<modifier_action_kind>(99);
    require_not_written(body);
    require_not_written(result_body(), 1);
}

TEST_CASE("plugin api: modifier.stop is written and read")
{
    modifier_stop_message body;
    body.instance = 8;
    (void)round_trip(body, 2);
    const auto written = encode_as(body, 2);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"modifier.stop","id":2,"instance":8})"));
    require_refused<modifier_stop_message>(R"({"op":"modifier.stop","id":2})",
                                           "modifier.stop.instance: is");
    require_refused<modifier_stop_message>(
        R"({"op":"modifier.stop","id":2,"instance":8})", "has no bytes", {1});
    require_refused<modifier_stop_message>(
        R"({"op":"modifier.stop","instance":8})", "must have an id");
    require_not_written(body, 0);
    body.instance = 0;
    require_not_written(body, 2);
}

TEST_CASE("plugin api: a result is within the bounds of the host")
{
    modifier_result_message body;
    body.instance = 1;
    body.seq = 1;
    body.actions = {action_of(modifier_action_kind::pass)};
    // The blob: four times the batch plus 1 KiB.
    body.data = bytes_of(4 * 100 + 1024);
    REQUIRE(modifier_result_within_bounds(100, body));
    body.data = bytes_of(4 * 100 + 1025);
    REQUIRE(!modifier_result_within_bounds(100, body));
    REQUIRE(modifier_result_within_bounds(101, body));
    body.data.clear();
    REQUIRE(modifier_result_within_bounds(0, body));
    body.data = bytes_of(1024);
    REQUIRE(modifier_result_within_bounds(0, body));
    body.data = bytes_of(1025);
    REQUIRE(!modifier_result_within_bounds(0, body));
    // A batch so large that four times it would wrap.
    REQUIRE(modifier_result_within_bounds(static_cast<std::size_t>(-1), body));
    body.data.clear();

    // Actions may use the same range of the result blob again and again: what
    // they send on is bounded, and not only the blob.
    body.data = bytes_of(2048);
    body.actions.assign(256,
                        range_of(modifier_action_kind::insert_after, 0, 2048));
    REQUIRE(!modifier_result_within_bounds(256, body));
    // 4 x 256 + 1024 bytes are allowed in all, and the batch itself is 256.
    body.actions.assign(1,
                        range_of(modifier_action_kind::insert_after, 0, 1792));
    REQUIRE(modifier_result_within_bounds(256, body));
    body.actions.assign(1,
                        range_of(modifier_action_kind::insert_after, 0, 1793));
    REQUIRE(!modifier_result_within_bounds(256, body));
    body.actions = {range_of(modifier_action_kind::replace, 0, 1000),
                    range_of(modifier_action_kind::insert_before, 0, 793)};
    REQUIRE(!modifier_result_within_bounds(256, body));
    body.actions = {range_of(modifier_action_kind::replace, 0, 1000),
                    range_of(modifier_action_kind::insert_before, 0, 792)};
    REQUIRE(modifier_result_within_bounds(256, body));
    // A length that no decoded message has does not wrap the sum.
    body.actions = {
        range_of(modifier_action_kind::insert_after, 0, ~std::uint64_t{0}),
        range_of(modifier_action_kind::insert_after, 0, 2)};
    REQUIRE(!modifier_result_within_bounds(256, body));
    // A batch that large has no bound to break, however it is counted.
    REQUIRE(modifier_result_within_bounds(static_cast<std::size_t>(-1), body));
    body.data.clear();

    // The delays and the gaps of the splits: at most one minute in all.
    body.data.clear();
    body.actions = {delay_of(max_api_delay_us)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {delay_of(max_api_delay_us), delay_of(1)};
    REQUIRE(!modifier_result_within_bounds(10, body));
    body.actions = {delay_of(30'000'000), delay_of(30'000'000)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {delay_of(30'000'000), delay_of(30'000'001)};
    REQUIRE(!modifier_result_within_bounds(10, body));
    // A split with n parts has n - 1 gaps.
    body.actions = {split_of(3, 30'000'000)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {split_of(3, 30'000'001)};
    REQUIRE(!modifier_result_within_bounds(10, body));
    body.actions = {split_of(2, max_api_delay_us)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {split_of(16, 4'000'000)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {split_of(16, 4'000'001)};
    REQUIRE(!modifier_result_within_bounds(10, body));
    body.actions = {split_of(2, 59'999'999), delay_of(1)};
    REQUIRE(modifier_result_within_bounds(10, body));
    body.actions = {split_of(2, 59'999'999), delay_of(2)};
    REQUIRE(!modifier_result_within_bounds(10, body));
    // Values that a decoded message never has do not wrap the sum.
    body.actions = {split_of(16, ~std::uint64_t{0})};
    REQUIRE(!modifier_result_within_bounds(10, body));
    body.actions = {delay_of(~std::uint64_t{0})};
    REQUIRE(!modifier_result_within_bounds(10, body));
    body.actions = {split_of(0, ~std::uint64_t{0})};
    REQUIRE(modifier_result_within_bounds(10, body));
    // Other actions add no delay.
    body.actions = {action_of(modifier_action_kind::drop),
                    range_of(modifier_action_kind::replace, 0, 0)};
    REQUIRE(modifier_result_within_bounds(10, body));
    // The result of a message that was read, which has the most the format
    // allows, is within the bounds when the batch was large enough.
    const modifier_result_message full = result_body();
    REQUIRE(modifier_result_within_bounds(4, full));
}

TEST_CASE("plugin api: a modifier result is written only as it is read")
{
    // A member that a kind of action does not use is not read, so an action
    // that has one is not what would be read back, and is not written.
    modifier_result_message body;
    body.instance = 1;
    body.seq = 1;
    body.actions = {action_of(modifier_action_kind::pass)};
    (void)round_trip(body);
    body.actions[0].offset = 4;
    require_not_written(body);
    body.actions = {action_of(modifier_action_kind::drop)};
    body.actions[0].us = 9;
    require_not_written(body);
}

TEST_CASE("plugin api: the largest modifier.result is written")
{
    // The most that 64 KiB of batch allows: a blob of four times that and
    // 1 KiB, and 256 actions.
    modifier_result_message body;
    body.instance = max_api_number;
    body.seq = max_api_number;
    body.data = bytes_of(max_modifier_result_blob);
    body.actions.assign(max_modifier_chunks,
                        range_of(modifier_action_kind::replace, 0, 1));
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(std::get<other_message>(written.value().body).header.size() <
            max_header_size);
    REQUIRE(modifier_result_within_bounds(max_modifier_batch, body));
}
