//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_fixture.hpp>

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

byte_buffer blob_of(std::string_view text)
{
    byte_buffer out;
    for (const char c : text)
        out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

constexpr std::string_view good_set =
    R"({"attach":{"port":"p","role":"peer"},"rules":[{"name":"a",)"
    R"("when":{"bytes":"01 02"},"do":[{"mark":"A"}]}]})";

} // namespace

TEST_CASE("plugin api: fixture.create is written and read")
{
    fixture_create_message body;
    body.instance = 3;
    body.place = "COM12";
    body.params = R"({"model":"2000"})";
    (void)round_trip(body, 4);
    const auto written = encode_as(body, 4);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.create","id":4,"instance":3,)"
                           R"("place":"COM12","params":{"model":"2000"}})"));
    REQUIRE_EQ(accepted<fixture_create_message>(
                   R"({"op":"fixture.create","id":1,"instance":3,)"
                   R"("place":"COM12"})")
                   .params,
               std::string("{}"));
}

TEST_CASE("plugin api: fixture.create is refused when it is wrong")
{
    using message_type = fixture_create_message;
    const std::string head = R"({"op":"fixture.create","id":1,)";
    require_refused<message_type>(head + R"("place":"p"})",
                                  "fixture.create.instance: is required");
    require_refused<message_type>(
        head + R"("instance":0,"place":"p"})",
        "fixture.create.instance: must be a whole number");
    require_refused<message_type>(head + R"("instance":1})",
                                  "fixture.create.place: is required");
    require_refused<message_type>(head + R"("instance":1,"place":""})",
                                  "fixture.create.place: must not be empty");
    require_refused<message_type>(head + R"("instance":1,"place":4})",
                                  "fixture.create.place: must be a string");
    require_refused<message_type>(head + R"("instance":1,"place":"a\nb"})",
                                  "fixture.create.place: must be plain text");
    require_refused<message_type>(
        head + R"("instance":1,"place":")" + std::string(129, 'p') + R"("})",
        "fixture.create.place: is longer than 128 bytes");
    require_refused<message_type>(
        head + R"("instance":1,"place":"p","params":[]})",
        "fixture.create.params: must be a JSON object");
    require_refused<message_type>(head + R"("instance":1,"place":"p"})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"fixture.create","instance":1,"place":"p"})",
        "must have an id");
    REQUIRE_EQ(accepted<message_type>(head +
                                      R"("instance":9007199254740991,)"
                                      R"("place":")" +
                                      std::string(128, 'p') + R"("})")
                   .place.size(),
               max_api_name);
    message_type body;
    body.instance = 1;
    body.place = std::string(129, 'p');
    require_not_written(body, 1);
    body.place.clear();
    require_not_written(body, 1);
    body.place = "p";
    body.params = "[]";
    require_not_written(body, 1);
    body.params = "{}";
    require_not_written(body, 0);
}

TEST_CASE("plugin api: fixture.data keeps its bytes in the blob")
{
    fixture_data_message body;
    body.instance = 2;
    body.ts_ns = 123456789;
    body.data = bytes_of(100);
    const auto read = round_trip(body);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.data","instance":2,)"
                           R"("ts_ns":123456789})"));
}

TEST_CASE("plugin api: fixture.data has between 1 and 8 KiB and a time")
{
    using message_type = fixture_data_message;
    const std::string head = R"({"op":"fixture.data","instance":2,)";
    const std::string header = head + R"("ts_ns":0})";
    REQUIRE_EQ(accepted<message_type>(header, bytes_of(1)).data.size(),
               std::size_t{1});
    REQUIRE_EQ(
        accepted<message_type>(header, bytes_of(max_fixture_data)).data.size(),
        max_fixture_data);
    require_refused<message_type>(header, "has a blob of 0 bytes");
    require_refused<message_type>(header, "has a blob of 8193 bytes",
                                  bytes_of(max_fixture_data + 1));
    REQUIRE_EQ(accepted<message_type>(head + R"("ts_ns":9007199254740991})",
                                      bytes_of(1))
                   .ts_ns,
               max_api_number);
    require_refused<message_type>(
        head + R"("ts_ns":9007199254740992})",
        "fixture.data.ts_ns: must be a whole number from 0", bytes_of(1));
    require_refused<message_type>(head + R"("ts_ns":-1})", "fixture.data.ts_ns",
                                  bytes_of(1));
    require_refused<message_type>(head + R"("ts_ns":"5"})",
                                  "fixture.data.ts_ns", bytes_of(1));
    require_refused<message_type>(R"({"op":"fixture.data","instance":2})",
                                  "fixture.data.ts_ns: is required",
                                  bytes_of(1));
    require_refused<message_type>(
        R"({"op":"fixture.data","id":3,"instance":2,"ts_ns":1})",
        "neither an id nor a re", bytes_of(1));

    message_type body;
    body.instance = 2;
    require_not_written(body);
    body.data = bytes_of(max_fixture_data + 1);
    require_not_written(body);
    body.data = bytes_of(max_fixture_data);
    (void)round_trip(body);
    body.ts_ns = max_api_number + 1;
    require_not_written(body);
}

TEST_CASE("plugin api: fixture.line is written and read")
{
    fixture_line_message body;
    body.instance = 5;
    body.ts_ns = 77;
    body.dtr = true;
    body.rts = false;
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.line","instance":5,"ts_ns":77,)"
                           R"("dtr":true,"rts":false})"));
    using message_type = fixture_line_message;
    const std::string head = R"({"op":"fixture.line","instance":5,"ts_ns":1,)";
    require_refused<message_type>(head + R"("rts":true})",
                                  "fixture.line.dtr: is required");
    require_refused<message_type>(head + R"("dtr":true})",
                                  "fixture.line.rts: is required");
    require_refused<message_type>(head + R"("dtr":1,"rts":true})",
                                  "fixture.line.dtr: must be true or false");
    require_refused<message_type>(head + R"("dtr":true,"rts":"yes"})",
                                  "fixture.line.rts: must be true or false");
    require_refused<message_type>(head + R"("dtr":true,"rts":true})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"fixture.line","instance":5,"dtr":true,)"
        R"("rts":true})",
        "fixture.line.ts_ns: is required");
}

TEST_CASE("plugin api: fixture.reply is written and read")
{
    fixture_reply_message body;
    body.instance = 4;
    body.data = {0x31, 0x0d, 0x0a};
    const auto now = round_trip(body);
    REQUIRE(!now.after_us.has_value());
    auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.reply","instance":4})"));
    body.after_us = 1500;
    const auto later = round_trip(body);
    REQUIRE(later.after_us.has_value());
    REQUIRE_EQ(*later.after_us, std::uint64_t{1500});
    written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.reply","instance":4,)"
                           R"("after_us":1500})"));
    // A delay of 0 is not the same as none, and it is kept.
    body.after_us = 0;
    const auto zero = round_trip(body);
    REQUIRE(zero.after_us.has_value());
}

TEST_CASE("plugin api: fixture.reply has the limits of its members")
{
    using message_type = fixture_reply_message;
    const std::string head = R"({"op":"fixture.reply","instance":4)";
    const std::string header = head + "}";
    REQUIRE_EQ(
        accepted<message_type>(header, bytes_of(max_fixture_reply)).data.size(),
        max_fixture_reply);
    REQUIRE_EQ(accepted<message_type>(header, bytes_of(1)).data.size(),
               std::size_t{1});
    require_refused<message_type>(header, "has a blob of 0 bytes");
    require_refused<message_type>(header, "has a blob of 4097 bytes",
                                  bytes_of(max_fixture_reply + 1));
    const byte_buffer one = bytes_of(1);
    const auto edge =
        accepted<message_type>(head + R"(,"after_us":60000000})", one).after_us;
    REQUIRE(edge.has_value());
    REQUIRE_EQ(*edge, max_api_delay_us);
    require_refused<message_type>(
        head + R"(,"after_us":60000001})",
        "fixture.reply.after_us: must be a whole number from 0", one);
    require_refused<message_type>(head + R"(,"after_us":-1})",
                                  "fixture.reply.after_us", one);
    require_refused<message_type>(head + R"(,"after_us":"1"})",
                                  "fixture.reply.after_us", one);
    require_refused<message_type>(head + R"(,"after_us":1.5})",
                                  "fixture.reply.after_us", one);
    message_type body;
    body.instance = 4;
    require_not_written(body);
    body.data = bytes_of(max_fixture_reply + 1);
    require_not_written(body);
    body.data = one;
    body.after_us = max_api_delay_us + 1;
    require_not_written(body);
    body.after_us = max_api_delay_us;
    (void)round_trip(body);
}

TEST_CASE("plugin api: fixture.line.set needs a line, and may have both")
{
    fixture_line_set_message body;
    body.instance = 6;
    body.dtr = true;
    (void)round_trip(body);
    auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.line.set","instance":6,)"
                           R"("dtr":true})"));
    body.dtr.reset();
    body.rts = false;
    (void)round_trip(body);
    body.dtr = false;
    body.rts = true;
    body.after_us = 250;
    (void)round_trip(body);
    written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"fixture.line.set","instance":6,)"
                           R"("dtr":false,"rts":true,"after_us":250})"));

    using message_type = fixture_line_set_message;
    const std::string head = R"({"op":"fixture.line.set","instance":6)";
    require_refused<message_type>(head + "}", "needs dtr or rts");
    require_refused<message_type>(head + R"(,"after_us":5})",
                                  "needs dtr or rts");
    require_refused<message_type>(
        head + R"(,"dtr":"on"})",
        "fixture.line.set.dtr: must be true or false");
    require_refused<message_type>(
        head + R"(,"rts":0})", "fixture.line.set.rts: must be true or false");
    require_refused<message_type>(
        head + R"(,"dtr":true,"after_us":60000001})",
        "fixture.line.set.after_us: must be a whole number");
    REQUIRE_EQ(
        accepted<message_type>(head + R"(,"dtr":true,"after_us":60000000})")
            .after_us.value_or(0),
        max_api_delay_us);
    require_refused<message_type>(head + R"(,"dtr":true})", "has no bytes",
                                  {1});

    message_type none;
    none.instance = 6;
    require_not_written(none);
    none.after_us = 5;
    require_not_written(none);
    none.dtr = true;
    none.after_us = max_api_delay_us + 1;
    require_not_written(none);
}

TEST_CASE("plugin api: fixture.fault has the spellings of the rule parser")
{
    struct spelling
    {
        std::string_view text;
        line_fault fault;
    };
    const spelling all[] = {{"parity", line_fault::parity},
                            {"framing", line_fault::framing},
                            {"overrun", line_fault::overrun},
                            {"break", line_fault::break_signal},
                            {"hang_up", line_fault::hang_up},
                            {"disconnect", line_fault::disconnect}};
    for (const spelling& one : all)
    {
        REQUIRE_EQ(std::string(to_string(one.fault)), std::string(one.text));
        fixture_fault_message body;
        body.instance = 1;
        body.fault = one.fault;
        (void)round_trip(body);
        const auto written = encode_as(body);
        REQUIRE(written.has_value());
        REQUIRE_EQ(header_of(written.value()),
                   R"({"op":"fixture.fault","instance":1,"fault":")" +
                       std::string(one.text) + R"("})");
    }
    using message_type = fixture_fault_message;
    const std::string head = R"({"op":"fixture.fault","instance":1)";
    require_refused<message_type>(head + "}",
                                  "fixture.fault.fault: is required");
    require_refused<message_type>(head + R"(,"fault":"hangup"})",
                                  "fixture.fault.fault: must be \"parity\"");
    require_refused<message_type>(head + R"(,"fault":"Parity"})",
                                  "fixture.fault.fault: must be");
    require_refused<message_type>(head + R"(,"fault":3})",
                                  "fixture.fault.fault: must be");
    require_refused<message_type>(head + R"(,"fault":"parity"})",
                                  "has no bytes", {1});
    fixture_fault_message bad;
    bad.instance = 1;
    bad.fault = static_cast<line_fault>(99);
    require_not_written(bad);
}

TEST_CASE("plugin api: rules.set keeps the rule set in the blob")
{
    rules_set_message body;
    body.instance = 2;
    body.data = blob_of(good_set);
    const auto read = round_trip(body, 7);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body, 7);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"rules.set","id":7,"instance":2})"));
    require_refused<rules_set_message>(R"({"op":"rules.set","instance":2})",
                                       "must have an id", blob_of(good_set));
    require_refused<rules_set_message>(R"({"op":"rules.set","id":1})",
                                       "rules.set.instance: is required",
                                       blob_of(good_set));
}

TEST_CASE("plugin api: rules.set refuses a blob that is not one JSON object")
{
    using message_type = rules_set_message;
    const std::string header = R"({"op":"rules.set","id":1,"instance":2})";
    require_refused<message_type>(header, "has a blob of 0 bytes");
    require_refused<message_type>(header,
                                  "rules.set.data: ", blob_of("not json"));
    require_refused<message_type>(header,
                                  "rules.set.data: ", blob_of("{\"a\":"));
    require_refused<message_type>(header, "rules.set.data: ", blob_of("{} {}"));
    require_refused<message_type>(header, "rules.set.data: ", blob_of("{} x"));
    require_refused<message_type>(
        header, "rules.set.data: must be a JSON object", blob_of("[]"));
    require_refused<message_type>(
        header, "rules.set.data: must be a JSON object", blob_of("7"));
    require_refused<message_type>(
        header, "rules.set.data: must be a JSON object", blob_of("\"{}\""));
    require_refused<message_type>(
        header, "rules.set.data: ", blob_of(R"({"a":1,"b":2,"a":3})"));
    require_refused<message_type>(
        header, "rules.set.data: ", blob_of(R"({"x":{"a":1,"a":1}})"));
    REQUIRE_EQ(accepted<message_type>(header, blob_of("{}")).data.size(),
               std::size_t{2});
    REQUIRE_EQ(accepted<message_type>(header, blob_of(" \n{ }\t")).data.size(),
               std::size_t{6});
}

TEST_CASE("plugin api: rules.set refuses text that is not UTF-8")
{
    using message_type = rules_set_message;
    const std::string header = R"({"op":"rules.set","id":1,"instance":2})";
    byte_buffer bad = blob_of(R"({"name":"a)");
    bad.push_back(0xff);
    for (const char c : std::string_view(R"("})"))
        bad.push_back(static_cast<std::uint8_t>(c));
    require_refused<message_type>(header, "rules.set.data: ", bad);
    byte_buffer cut = blob_of(R"({"name":"a)");
    cut.push_back(0xc3);
    for (const char c : std::string_view(R"("})"))
        cut.push_back(static_cast<std::uint8_t>(c));
    require_refused<message_type>(header, "rules.set.data: ", cut);
    byte_buffer good = blob_of(R"({"name":"a)");
    good.push_back(0xc3);
    good.push_back(0xa9);
    for (const char c : std::string_view(R"("})"))
        good.push_back(static_cast<std::uint8_t>(c));
    REQUIRE_EQ(accepted<message_type>(header, good).data.size(), good.size());
}

TEST_CASE("plugin api: rules.set has a depth and a size limit")
{
    using message_type = rules_set_message;
    const std::string header = R"({"op":"rules.set","id":1,"instance":2})";
    // The deepest document that the rule parser reads is 16 deep.
    const auto nested = [](std::size_t depth)
    {
        std::string text;
        for (std::size_t i = 0; i + 1 < depth; ++i)
            text += R"({"a":)";
        text += "{}";
        for (std::size_t i = 0; i + 1 < depth; ++i)
            text += '}';
        return text;
    };
    REQUIRE_EQ(accepted<message_type>(header, blob_of(nested(16))).data.size(),
               nested(16).size());
    require_refused<message_type>(header,
                                  "rules.set.data: ", blob_of(nested(17)));
    // The size: padded with white space to the edge, and one past it.
    std::string padded = "{}";
    padded.resize(max_api_blob, ' ');
    REQUIRE_EQ(accepted<message_type>(header, blob_of(padded)).data.size(),
               max_api_blob);
    require_refused<message_type>(header, "has a blob of 262145 bytes",
                                  blob_of(padded + " "));
    message_type body;
    body.instance = 2;
    require_not_written(body, 1);
    body.data = blob_of("[]");
    require_not_written(body, 1);
    body.data = blob_of("{}");
    (void)round_trip(body, 1);
    require_not_written(body, 0);
}

TEST_CASE("plugin api: rules.result says that a rule set was accepted")
{
    rules_result_message body;
    body.accepted = true;
    (void)round_trip(body, 7);
    const auto written = encode_as(body, 7);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"rules.result","re":7,"accepted":true})"));

    using message_type = rules_result_message;
    const std::string head = R"({"op":"rules.result","re":7,"accepted":true)";
    require_refused<message_type>(head + R"(,"rule":0})",
                                  "rules.result.rule: must be absent");
    require_refused<message_type>(head + R"(,"path":"rules[0]"})",
                                  "rules.result.path: must be absent");
    require_refused<message_type>(head + R"(,"reason":"fine"})",
                                  "rules.result.reason: must be absent");
    require_refused<message_type>(R"({"op":"rules.result","re":7})",
                                  "rules.result.accepted: is required");
    require_refused<message_type>(
        R"({"op":"rules.result","re":7,"accepted":"yes"})",
        "rules.result.accepted: must be true or false");
    require_refused<message_type>(
        R"({"op":"rules.result","id":7,"accepted":true})", "must have a re");
    require_refused<message_type>(R"({"op":"rules.result","accepted":true})",
                                  "must have a re");
    require_refused<message_type>(head + "}", "has no bytes", {1});

    body.rule = 1;
    require_not_written(body, 7);
    body.rule.reset();
    body.path = "rules[1]";
    require_not_written(body, 7);
    body.path.clear();
    body.reason = "why";
    require_not_written(body, 7);
    body.reason.clear();
    require_not_written(body, 0);
}

TEST_CASE("plugin api: rules.result says where the first problem is")
{
    rules_result_message body;
    body.accepted = false;
    body.rule = 2;
    body.path = "rules[2].do[0].delay";
    body.reason = "min_us is above max_us";
    const auto read = round_trip(body, 9);
    REQUIRE(read.rule.has_value());
    REQUIRE_EQ(*read.rule, std::uint64_t{2});
    const auto written = encode_as(body, 9);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"rules.result","re":9,"accepted":false,)"
                           R"("rule":2,"path":"rules[2].do[0].delay",)"
                           R"("reason":"min_us is above max_us"})"));

    // A problem that is not in a rule has no rule and may have no path.
    rules_result_message outside;
    outside.reason = "attach is missing";
    const auto plain = round_trip(outside, 9);
    REQUIRE(!plain.rule.has_value());
    REQUIRE(plain.path.empty());
    const auto short_form = encode_as(outside, 9);
    REQUIRE(short_form.has_value());
    REQUIRE_EQ(header_of(short_form.value()),
               std::string(R"({"op":"rules.result","re":9,"accepted":false,)"
                           R"("reason":"attach is missing"})"));
}

TEST_CASE("plugin api: rules.result has the limits of its members")
{
    using message_type = rules_result_message;
    const std::string head = R"({"op":"rules.result","re":9,"accepted":false,)";
    REQUIRE_EQ(accepted<message_type>(head + R"("rule":255,"reason":"x"})")
                   .rule.value_or(0),
               std::uint64_t{255});
    require_refused<message_type>(
        head + R"("rule":256,"reason":"x"})",
        "rules.result.rule: must be a whole number from 0 to 255");
    require_refused<message_type>(head + R"("rule":-1,"reason":"x"})",
                                  "rules.result.rule");
    require_refused<message_type>(head + R"("rule":"1","reason":"x"})",
                                  "rules.result.rule");
    REQUIRE_EQ(accepted<message_type>(head + R"("path":")" +
                                      repeated('p', max_rules_path) +
                                      R"(","reason":"x"})")
                   .path.size(),
               max_rules_path);
    require_refused<message_type>(
        head + R"("path":")" + repeated('p', max_rules_path + 1) +
            R"(","reason":"x"})",
        "rules.result.path: is longer than 256 bytes");
    require_refused<message_type>(head + R"("path":"a\nb","reason":"x"})",
                                  "rules.result.path: must be plain text");
    require_refused<message_type>(head + R"("path":7,"reason":"x"})",
                                  "rules.result.path: must be a string");
    REQUIRE_EQ(accepted<message_type>(head + R"("reason":")" +
                                      repeated('r', max_api_text) + R"("})")
                   .reason.size(),
               max_api_text);
    require_refused<message_type>(
        head + R"("reason":")" + repeated('r', max_api_text + 1) + R"("})",
        "rules.result.reason: is longer than 200 bytes");
    require_refused<message_type>(head + R"("reason":""})",
                                  "rules.result.reason: must not be empty");
    require_refused<message_type>(head + R"("rule":1})",
                                  "rules.result.reason: is required");
    require_refused<message_type>(head + "\"reason\":\"a\xe2\x80\x8f"
                                         "b\"}",
                                  "rules.result.reason: must be plain text");
    require_refused<message_type>(head + R"("reason":"a\tb"})",
                                  "rules.result.reason: must be plain text");
    require_refused<message_type>(head + R"("reason":5})",
                                  "rules.result.reason: must be a string");

    message_type body;
    body.reason = "x";
    (void)round_trip(body, 9);
    body.reason.clear();
    require_not_written(body, 9);
    body.reason = repeated('r', max_api_text + 1);
    require_not_written(body, 9);
    body.reason = "x";
    body.rule = max_rule_count;
    require_not_written(body, 9);
    body.rule = max_rule_count - 1;
    (void)round_trip(body, 9);
    body.rule.reset();
    body.path = repeated('p', max_rules_path + 1);
    require_not_written(body, 9);
    body.path = "line\n";
    require_not_written(body, 9);
}

TEST_CASE("plugin api: rules.event names a rule and the action that fired")
{
    rules_event_message body;
    body.instance = 3;
    body.ts_ns = 5'000'000;
    body.rule = "idn";
    body.action = "reply";
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(
        header_of(written.value()),
        std::string(R"({"op":"rules.event","instance":3,)"
                    R"("ts_ns":5000000,"rule":"idn","action":"reply"})"));

    using message_type = rules_event_message;
    const std::string head = R"({"op":"rules.event","instance":3,"ts_ns":1,)";
    REQUIRE_EQ(accepted<message_type>(head + R"("rule":")" +
                                      repeated('r', max_rule_name) +
                                      R"(","action":"mark"})")
                   .rule.size(),
               max_rule_name);
    REQUIRE_EQ(accepted<message_type>(head + R"("rule":"a","action":")" +
                                      repeated('x', max_rule_name) + R"("})")
                   .action.size(),
               max_rule_name);
    require_refused<message_type>(head + R"("rule":"a","action":")" +
                                      repeated('x', max_rule_name + 1) +
                                      R"("})",
                                  "rules.event.action: is longer than 64");
    require_refused<message_type>(head + R"("rule":"a","action":0})",
                                  "rules.event.action: must be a string");
    require_refused<message_type>(head + R"("rule":"a","action":""})",
                                  "rules.event.action: must not be empty");
    require_refused<message_type>(head + R"("rule":"a","action":"a\nb"})",
                                  "rules.event.action: must be plain text");
    require_refused<message_type>(head + R"("rule":"a"})",
                                  "rules.event.action: is required");
    require_refused<message_type>(head + R"("action":"mark"})",
                                  "rules.event.rule: is required");
    require_refused<message_type>(head + R"("rule":"","action":"mark"})",
                                  "rules.event.rule: must not be empty");
    require_refused<message_type>(head + R"("rule":")" +
                                      repeated('r', max_rule_name + 1) +
                                      R"(","action":"mark"})",
                                  "rules.event.rule: is longer than 64 bytes");
    require_refused<message_type>(head + R"("rule":"a\nb","action":"mark"})",
                                  "rules.event.rule: must be plain text");
    require_refused<message_type>(head + R"("rule":4,"action":"mark"})",
                                  "rules.event.rule: must be a string");
    require_refused<message_type>(head + R"("rule":"a","action":"mark"})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"rules.event","instance":3,"rule":"a",)"
        R"("action":"mark"})",
        "rules.event.ts_ns: is required");
    body.rule.clear();
    require_not_written(body);
    body.rule = repeated('r', max_rule_name + 1);
    require_not_written(body);
    body.rule = "a";
    body.action.clear();
    require_not_written(body);
}

TEST_CASE("plugin api: check_rule_set accepts a good rule set")
{
    rules_set_message body;
    body.instance = 2;
    body.data = blob_of(good_set);
    const rules_result_message result = check_rule_set(body);
    REQUIRE(result.accepted);
    REQUIRE(!result.rule.has_value());
    REQUIRE(result.path.empty());
    REQUIRE(result.reason.empty());
    // What it says is a message that can be written as an answer.
    (void)round_trip(result, 3);
}

TEST_CASE("plugin api: check_rule_set says where the first problem is")
{
    rules_set_message body;
    body.instance = 2;
    // The second rule has a key that no rule has.
    body.data =
        blob_of(R"({"attach":{"port":"p","role":"peer"},"rules":[)"
                R"({"name":"a","when":{"bytes":"01 02"},"do":[{"mark":"A"}]},)"
                R"({"name":"b","when":{"bytes":"03"},"do":[{"mark":"B"}],)"
                R"("bogus":1}]})");
    const rules_result_message result = check_rule_set(body);
    REQUIRE(!result.accepted);
    REQUIRE(result.rule.has_value());
    REQUIRE_EQ(*result.rule, std::uint64_t{1});
    REQUIRE(result.path.starts_with("rules[1]"));
    REQUIRE(!result.reason.empty());
    REQUIRE(result.reason.size() <= max_api_text);
    (void)round_trip(result, 3);

    // A problem outside every rule has no rule number.
    body.data = blob_of(
        R"({"attach":{"port":"p","role":"peer"},"bogus":1,"rules":[)"
        R"({"name":"a","when":{"bytes":"01 02"},"do":[{"mark":"A"}]}]})");
    const rules_result_message outside = check_rule_set(body);
    REQUIRE(!outside.accepted);
    REQUIRE(!outside.rule.has_value());
    REQUIRE(!outside.reason.empty());
    (void)round_trip(outside, 3);

    // A set with nothing in it that the compiler can use.
    body.data = blob_of("{}");
    const rules_result_message empty = check_rule_set(body);
    REQUIRE(!empty.accepted);
    REQUIRE(!empty.reason.empty());
    (void)round_trip(empty, 3);
}

TEST_CASE("plugin api: check_rule_set keeps its answer within the limits")
{
    rules_set_message body;
    body.instance = 2;
    // A key that is long and not plain is quoted in the problem.  The answer
    // has only printable ASCII and fits the limits of a rules.result.
    body.data = blob_of(R"({"attach":{"port":"p","role":"peer"},"rules":[],)"
                        R"("k\u0001)" +
                        repeated('k', 400) + R"(":1})");
    const rules_result_message result = check_rule_set(body);
    REQUIRE(!result.accepted);
    REQUIRE(result.path.size() <= max_rules_path);
    REQUIRE(result.reason.size() <= max_api_text);
    for (const char c : result.path + result.reason)
        REQUIRE(c >= 0x20 && c < 0x7f);
    (void)round_trip(result, 3);
}

TEST_CASE("plugin api: the rule limits of the messages are the rule model's")
{
    // api_fixture.cpp has static_asserts for the same; this names them.
    REQUIRE_EQ(max_fixture_reply, std::size_t{4096});
    REQUIRE_EQ(max_rule_name, std::size_t{64});
    REQUIRE_EQ(max_rule_actions, std::size_t{16});
    REQUIRE_EQ(max_rule_count, std::size_t{256});
}

TEST_CASE("plugin api: a rules.set detail never repeats what the plugin sent")
{
    using message_type = rules_set_message;
    const std::string head = R"({"op":"rules.set","id":1,"instance":2})";
    // A key twice is named only as a key twice, not by its name.
    require_detail_without<message_type>(
        head, "hostile_key_7f3a",
        blob_of(R"({"hostile_key_7f3a":1,"hostile_key_7f3a":2})"));
    require_refused<message_type>(
        head, "rules.set.data: has a key given twice in one object",
        blob_of(R"({"hostile_key_7f3a":1,"hostile_key_7f3a":2})"));
    require_detail_without<message_type>(head, "hostile_key_7f3a",
                                         blob_of(R"({"hostile_key_7f3a":)"));
}

TEST_CASE("plugin api: a fixture fault has one key for its kind")
{
    // The key is `fault` here and in a modifier action.
    require_refused<fixture_fault_message>(
        R"({"op":"fixture.fault","instance":1,"fault_kind":"parity"})",
        "fixture.fault.fault: is required");
}
