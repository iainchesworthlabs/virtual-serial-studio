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

trigger_publish_message sample_trigger()
{
    trigger_publish_message body;
    body.instance = 3;
    body.kind = trigger_kind::check;
    body.when_ns = 18204113250;
    body.place = "bus-1";
    body.direction = decoder_direction::rx;
    body.sender = "meter-17";
    body.protocol = trigger_protocol{"com.example.meter-bus", "1.2.0"};
    body.frame =
        trigger_frame{48213, 10, 18203200000, 18204113250, "read reply"};
    body.field = trigger_field{"check", 7, 2};
    body.expected = "3A91";
    body.actual = "3A90";
    body.covers = trigger_covers{1, 6};
    body.cause = trigger_cause{trigger_bus_cause{decoder_fault::overlap, 9912}};
    body.request = trigger_request{48205, 8, 18201000000};
    body.text = "Check value is 3A90, expected 3A91.";
    return body;
}

const std::string trigger_head =
    R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":5,)"
    R"("place":"p","direction":"rx","text":"t")";

} // namespace

// -- trigger.publish
// -------------------------------------------------------------

TEST_CASE("plugin api: trigger.publish is written and read")
{
    const trigger_publish_message body = sample_trigger();
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob.empty());
    REQUIRE_EQ(
        header_of(written.value()),
        std::string(
            R"({"op":"trigger.publish","instance":3,"kind":"check",)"
            R"("when_ns":18204113250,"place":"bus-1","direction":"rx",)"
            R"("sender":"meter-17","protocol":{"id":"com.example.meter-bus",)"
            R"("version":"1.2.0"},"frame":{"offset":48213,"length":10,)"
            R"("t_start_ns":18203200000,"t_end_ns":18204113250,)"
            R"("message":"read reply"},"field":{"name":"check","offset":7,)"
            R"("length":2},"expected":"3A91","actual":"3A90","covers":[1,6],)"
            R"("cause":{"bus_fault":"overlap","event":9912},)"
            R"("request":{"offset":48205,"length":8,"t_end_ns":18201000000},)"
            R"("text":"Check value is 3A90, expected 3A91."})"));
}

TEST_CASE("plugin api: trigger.publish needs only what every trigger has")
{
    using M = trigger_publish_message;
    const auto read = accepted<M>(trigger_head + "}");
    REQUIRE(read.kind == trigger_kind::check);
    REQUIRE(read.sender.empty());
    REQUIRE(read.code.empty());
    REQUIRE(!read.protocol.has_value());
    REQUIRE(!read.frame.has_value());
    REQUIRE(!read.field.has_value());
    REQUIRE(read.expected.empty());
    REQUIRE(!read.covers.has_value());
    REQUIRE(!read.cause.has_value());
    REQUIRE(!read.request.has_value());
    (void)round_trip(read);
    const auto written = encode_as(read);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"trigger.publish","instance":3,)"
                           R"("kind":"check","when_ns":5,"place":"p",)"
                           R"("direction":"rx","text":"t"})"));
}

TEST_CASE("plugin api: trigger.publish reads each kind")
{
    using M = trigger_publish_message;
    const std::pair<std::string_view, trigger_kind> kinds[] = {
        {"framing", trigger_kind::framing},
        {"unknown_type", trigger_kind::unknown_type},
        {"unknown_value", trigger_kind::unknown_value},
        {"check", trigger_kind::check},
        {"length", trigger_kind::length},
        {"timeout", trigger_kind::timeout},
        {"sequence", trigger_kind::sequence},
        {"contention", trigger_kind::contention},
        {"line_error", trigger_kind::line_error},
        {"decoder_lost", trigger_kind::decoder_lost}};
    for (const auto& [name, kind] : kinds)
    {
        const auto read = accepted<M>(
            R"({"op":"trigger.publish","instance":3,"kind":")" +
            std::string(name) +
            R"(","when_ns":5,"place":"p","direction":"tx","text":"t"})");
        REQUIRE(read.kind == kind);
        (void)round_trip(read);
    }
    const auto custom =
        accepted<M>(R"({"op":"trigger.publish","instance":3,"kind":"custom",)"
                    R"("code":"meter.low-battery_1","when_ns":5,"place":"p",)"
                    R"("direction":"tx","text":"t"})");
    REQUIRE(custom.kind == trigger_kind::custom);
    REQUIRE_EQ(custom.code, std::string("meter.low-battery_1"));
    (void)round_trip(custom);
}

TEST_CASE("plugin api: the kind and code of trigger.publish go together")
{
    using M = trigger_publish_message;
    const std::string rest =
        R"("when_ns":5,"place":"p","direction":"rx","text":"t"})";
    const std::string head = R"({"op":"trigger.publish","instance":3,)";
    require_refused<M>(head + rest, "trigger.publish.kind: is required");
    require_refused<M>(head + R"("kind":"made_up",)" + rest,
                       "trigger.publish.kind: must be framing, unknown_type, "
                       "unknown_value, check, length, timeout, sequence, "
                       "contention, line_error, decoder_lost or custom");
    require_refused<M>(head + R"("kind":"Check",)" + rest,
                       "trigger.publish.kind: must be framing");
    require_refused<M>(head + R"("kind":4,)" + rest,
                       "trigger.publish.kind: must be framing");
    // A code only with `custom`.
    require_refused<M>(head + R"("kind":"custom",)" + rest,
                       "trigger.publish.code: is required");
    require_refused<M>(head + R"("kind":"check","code":"x",)" + rest,
                       "trigger.publish.code: must be left out unless kind is "
                       "custom");
    require_refused<M>(head + R"("kind":"check","code":"",)" + rest,
                       "trigger.publish.code: must be left out");
    require_refused<M>(head + R"("kind":"custom","code":"",)" + rest,
                       "trigger.publish.code: must be 1 to 64 lower-case "
                       "letters, digits, dots, hyphens and underscores");
    require_refused<M>(head + R"("kind":"custom","code":"Low",)" + rest,
                       "trigger.publish.code: must be 1 to 64 lower-case");
    require_refused<M>(head + R"("kind":"custom","code":"a b",)" + rest,
                       "trigger.publish.code: must be 1 to 64 lower-case");
    require_refused<M>(head + R"("kind":"custom","code":5,)" + rest,
                       "trigger.publish.code: must be a string");
    (void)accepted<M>(head + R"("kind":"custom","code":")" +
                      repeated('c', max_decoder_name) + "\"," + rest);
    require_refused<M>(head + R"("kind":"custom","code":")" +
                           repeated('c', max_decoder_name + 1) + "\"," + rest,
                       "trigger.publish.code: must be 1 to 64 lower-case");

    trigger_publish_message body = sample_trigger();
    body.kind = trigger_kind::custom;
    require_not_written(body);
    body.code = "low-battery";
    (void)round_trip(body);
    body.kind = trigger_kind::check;
    require_not_written(body);
    body.code.clear();
    (void)round_trip(body);
    body.kind = trigger_kind::custom;
    body.code = "Low";
    require_not_written(body);
}

TEST_CASE("plugin api: the host stamps a trigger, and the plugin does not")
{
    using M = trigger_publish_message;
    // `from` and the time are not members: they are passed over.
    const auto read =
        accepted<M>(trigger_head +
                    R"(,"from":{"plugin":"x","instance":9},"published_ns":4})");
    REQUIRE(read == accepted<M>(trigger_head + "}"));
    // `id` is the id of the frame, and a notification has none.
    require_refused<M>(
        R"({"op":"trigger.publish","id":4127,"instance":3,"kind":"check",)"
        R"("when_ns":5,"place":"p","direction":"rx","text":"t"})",
        "neither an id nor a re");
    // A trigger has no bytes.
    require_refused<M>(trigger_head + "}", "has no bytes", {1});
    require_refused<M>(trigger_head + "}", "has no bytes", bytes_of(4096));
}

TEST_CASE("plugin api: trigger.publish needs its members")
{
    using M = trigger_publish_message;
    const auto without = [](std::string_view member)
    {
        std::string text = trigger_head + "}";
        const std::size_t at = text.find(member);
        REQUIRE(at != std::string::npos);
        text.erase(at, member.size());
        return text;
    };
    require_refused<M>(without(R"("instance":3,)"),
                       "trigger.publish.instance: is required");
    require_refused<M>(without(R"("when_ns":5,)"),
                       "trigger.publish.when_ns: is required");
    require_refused<M>(without(R"("place":"p",)"),
                       "trigger.publish.place: is required");
    require_refused<M>(without(R"("direction":"rx",)"),
                       "trigger.publish.direction: is required");
    require_refused<M>(without(R"(,"text":"t")"),
                       "trigger.publish.text: is required");
    require_refused<M>(without(R"("direction":"rx",)") + "",
                       "trigger.publish.direction");
    require_refused<M>(
        R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":5,)"
        R"("place":"p","direction":"up","text":"t"})",
        "trigger.publish.direction: must be rx or tx");
    require_refused<M>(
        R"({"op":"trigger.publish","instance":0,"kind":"check","when_ns":5,)"
        R"("place":"p","direction":"rx","text":"t"})",
        "trigger.publish.instance: must be a whole number");
    require_refused<M>(
        R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":-1,)"
        R"("place":"p","direction":"rx","text":"t"})",
        "trigger.publish.when_ns: must be a whole number");
    require_refused<M>(
        R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":)" +
            number_text(max_api_number + 1) +
            R"(,"place":"p","direction":"rx","text":"t"})",
        "trigger.publish.when_ns: must be a whole number");
    (void)accepted<M>(
        R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":)" +
        number_text(max_api_number) +
        R"(,"place":"p","direction":"rx","text":"t"})");
}

TEST_CASE("plugin api: the text and names of trigger.publish are limited")
{
    using M = trigger_publish_message;
    const std::string head =
        R"({"op":"trigger.publish","instance":3,"kind":"check","when_ns":5,)"
        R"("direction":"rx",)";
    const auto with = [&](std::string_view place, std::string_view text,
                          std::string_view more = {})
    {
        return head + R"("place":")" + std::string(place) + R"(","text":")" +
               std::string(text) + "\"" + std::string(more) + "}";
    };
    REQUIRE_EQ(accepted<M>(with(repeated('p', max_api_name),
                                repeated('t', max_api_text)))
                   .text.size(),
               max_api_text);
    require_refused<M>(with("p", repeated('t', max_api_text + 1)),
                       "trigger.publish.text: is longer than 200 bytes");
    require_refused<M>(with("p", ""),
                       "trigger.publish.text: must not be empty");
    require_refused<M>(with("p", R"(a\nb)"),
                       "trigger.publish.text: must be plain text");
    require_refused<M>(with("p", "a\\u202eb"),
                       "trigger.publish.text: must be plain text");
    require_refused<M>(with(repeated('p', max_api_name + 1), "t"),
                       "trigger.publish.place: is longer than 128 bytes");
    require_refused<M>(with("", "t"),
                       "trigger.publish.place: must not be empty");
    require_refused<M>(with("a\\u0001b", "t"),
                       "trigger.publish.place: must be plain text");
    // The sender is a name, and may be left out.
    REQUIRE_EQ(accepted<M>(with("p", "t", R"(,"sender":"meter-17")")).sender,
               std::string("meter-17"));
    require_refused<M>(with("p", "t", R"(,"sender":"")"),
                       "trigger.publish.sender: must not be empty");
    require_refused<M>(
        with("p", "t",
             ",\"sender\":\"" + repeated('s', max_api_name + 1) + "\""),
        "trigger.publish.sender: is longer than 128 bytes");
    require_refused<M>(with("p", "t", R"(,"sender":3)"),
                       "trigger.publish.sender: must be a string");

    trigger_publish_message body = sample_trigger();
    body.text.clear();
    require_not_written(body);
    body.text = repeated('t', max_api_text + 1);
    require_not_written(body);
    body.text = "t";
    body.place = repeated('p', max_api_name + 1);
    require_not_written(body);
    body.place = "p";
    body.sender = repeated('s', max_api_name + 1);
    require_not_written(body);
    body.sender.clear();
    (void)round_trip(body);
    body.when_ns = max_api_number + 1;
    require_not_written(body);
    body.when_ns = 5;
    body.instance = 0;
    require_not_written(body);
    body.instance = 1;
    require_not_written(body, 3);
}

TEST_CASE("plugin api: the protocol of a trigger is checked")
{
    using M = trigger_publish_message;
    const auto with = [](std::string_view protocol)
    { return trigger_head + R"(,"protocol":)" + std::string(protocol) + "}"; };
    (void)accepted<M>(with(R"({"id":"a","version":"1"})"));
    require_refused<M>(with("[]"), "trigger.publish.protocol: must be an "
                                   "object");
    require_refused<M>(with(R"({"version":"1"})"),
                       "trigger.publish.protocol.id: is required");
    require_refused<M>(with(R"({"id":"a"})"),
                       "trigger.publish.protocol.version: is required");
    require_refused<M>(with(R"({"id":"","version":"1"})"),
                       "trigger.publish.protocol.id: must not be empty");
    require_refused<M>(with(R"({"id":"a","version":""})"),
                       "trigger.publish.protocol.version: must not be empty");
    REQUIRE_EQ(accepted<M>(with("{\"id\":\"" + repeated('i', max_protocol_id) +
                                "\",\"version\":\"" +
                                repeated('v', max_protocol_version) + "\"}"))
                   .protocol.has_value(),
               true);
    require_refused<M>(with("{\"id\":\"" + repeated('i', max_protocol_id + 1) +
                            "\",\"version\":\"1\"}"),
                       "trigger.publish.protocol.id: is longer than 128 bytes");
    require_refused<M>(with("{\"id\":\"a\",\"version\":\"" +
                            repeated('v', max_protocol_version + 1) + "\"}"),
                       "trigger.publish.protocol.version: is longer than 64 "
                       "bytes");
    require_refused<M>(with(R"({"id":"a\n","version":"1"})"),
                       "trigger.publish.protocol.id: must be plain text");

    trigger_publish_message body = sample_trigger();
    body.protocol = trigger_protocol{"", "1"};
    require_not_written(body);
    body.protocol =
        trigger_protocol{"a", repeated('v', max_protocol_version + 1)};
    require_not_written(body);
}

TEST_CASE("plugin api: the frame of a trigger is checked")
{
    using M = trigger_publish_message;
    const auto with = [](std::string_view frame)
    { return trigger_head + R"(,"frame":)" + std::string(frame) + "}"; };
    const std::string good =
        R"({"offset":0,"length":10,"t_start_ns":1,"t_end_ns":2,"message":"m"})";
    (void)accepted<M>(with(good));
    require_refused<M>(with("5"), "trigger.publish.frame: must be an object");
    require_refused<M>(
        with(R"({"length":10,"t_start_ns":1,"t_end_ns":2,"message":"m"})"),
        "trigger.publish.frame.offset: is required");
    require_refused<M>(
        with(R"({"offset":0,"t_start_ns":1,"t_end_ns":2,"message":"m"})"),
        "trigger.publish.frame.length: is required");
    require_refused<M>(
        with(R"({"offset":0,"length":0,"t_start_ns":1,"t_end_ns":2,)"
             R"("message":"m"})"),
        "trigger.publish.frame.length: must be a whole number from 1 to "
        "65536");
    require_refused<M>(
        with(R"({"offset":0,"length":65537,"t_start_ns":1,"t_end_ns":2,)"
             R"("message":"m"})"),
        "trigger.publish.frame.length: must be a whole number from 1 to "
        "65536");
    (void)accepted<M>(
        with(R"({"offset":0,"length":65536,"t_start_ns":1,"t_end_ns":2,)"
             R"("message":"m"})"));
    require_refused<M>(
        with(R"({"offset":0,"length":1,"t_end_ns":2,"message":"m"})"),
        "trigger.publish.frame.t_start_ns: is required");
    require_refused<M>(
        with(R"({"offset":0,"length":1,"t_start_ns":1,"message":"m"})"),
        "trigger.publish.frame.t_end_ns: is required");
    require_refused<M>(
        with(R"({"offset":0,"length":1,"t_start_ns":3,"t_end_ns":2,)"
             R"("message":"m"})"),
        "trigger.publish.frame.t_end_ns: must not be before t_start_ns");
    (void)accepted<M>(
        with(R"({"offset":0,"length":1,"t_start_ns":2,"t_end_ns":2,)"
             R"("message":"m"})"));
    require_refused<M>(
        with(R"({"offset":0,"length":1,"t_start_ns":1,"t_end_ns":2})"),
        "trigger.publish.frame.message: is required");
    // The message of a frame may be empty (a frame that is not known).
    REQUIRE(accepted<M>(with(R"({"offset":0,"length":1,"t_start_ns":1,)"
                             R"("t_end_ns":2,"message":""})"))
                .frame.has_value());
    require_refused<M>(
        with("{\"offset\":0,\"length\":1,\"t_start_ns\":1,\"t_end_ns\":2,"
             "\"message\":\"" +
             repeated('m', max_decoder_name + 1) + "\"}"),
        "trigger.publish.frame.message: is longer than 64 bytes");
    require_refused<M>(
        with(R"({"offset":0,"length":1,"t_start_ns":1,"t_end_ns":2,)"
             "\"message\":\"a\\u202eb\"}"),
        "trigger.publish.frame.message: must be plain text");

    trigger_publish_message body = sample_trigger();
    body.frame = trigger_frame{0, 0, 1, 2, "m"};
    require_not_written(body);
    body.frame = trigger_frame{0, 1, 3, 2, "m"};
    require_not_written(body);
    body.frame = trigger_frame{0, 1, 2, 3, repeated('m', max_decoder_name + 1)};
    require_not_written(body);
    body.frame = trigger_frame{0, 1, 2, 3, ""};
    (void)round_trip(body);
}

TEST_CASE("plugin api: the field and the request of a trigger are checked")
{
    using M = trigger_publish_message;
    const auto field = [](std::string_view text)
    { return trigger_head + R"(,"field":)" + std::string(text) + "}"; };
    const auto request = [](std::string_view text)
    { return trigger_head + R"(,"request":)" + std::string(text) + "}"; };
    (void)accepted<M>(field(R"({"name":"check","offset":7,"length":2})"));
    require_refused<M>(field("[]"), "trigger.publish.field: must be an object");
    require_refused<M>(field(R"({"offset":7,"length":2})"),
                       "trigger.publish.field.name: is required");
    require_refused<M>(field(R"({"name":"","offset":7,"length":2})"),
                       "trigger.publish.field.name: must not be empty");
    require_refused<M>(field("{\"name\":\"" +
                             repeated('n', max_decoder_name + 1) +
                             "\",\"offset\":7,\"length\":2}"),
                       "trigger.publish.field.name: is longer than 64 bytes");
    require_refused<M>(field(R"({"name":"c","length":2})"),
                       "trigger.publish.field.offset: is required");
    require_refused<M>(field(R"({"name":"c","offset":7})"),
                       "trigger.publish.field.length: is required");
    require_refused<M>(field(R"({"name":"c","offset":7,"length":0})"),
                       "trigger.publish.field.length: must be a whole number "
                       "from 1 to 65536");
    require_refused<M>(field(R"({"name":"c","offset":7,"length":65537})"),
                       "trigger.publish.field.length: must be a whole number "
                       "from 1 to 65536");
    (void)accepted<M>(field(R"({"name":"c","offset":7,"length":65536})"));

    (void)accepted<M>(request(R"({"offset":4,"length":8,"t_end_ns":9})"));
    require_refused<M>(request("true"),
                       "trigger.publish.request: must be an object");
    require_refused<M>(request(R"({"length":8,"t_end_ns":9})"),
                       "trigger.publish.request.offset: is required");
    require_refused<M>(request(R"({"offset":4,"t_end_ns":9})"),
                       "trigger.publish.request.length: is required");
    require_refused<M>(request(R"({"offset":4,"length":0,"t_end_ns":9})"),
                       "trigger.publish.request.length: must be a whole "
                       "number from 1 to 65536");
    require_refused<M>(request(R"({"offset":4,"length":65537,"t_end_ns":9})"),
                       "trigger.publish.request.length: must be a whole "
                       "number from 1 to 65536");
    require_refused<M>(request(R"({"offset":4,"length":8})"),
                       "trigger.publish.request.t_end_ns: is required");
    require_refused<M>(request(R"({"offset":4,"length":8,"t_end_ns":"9"})"),
                       "trigger.publish.request.t_end_ns: must be a whole");

    trigger_publish_message body = sample_trigger();
    body.field = trigger_field{"c", 1, 0};
    require_not_written(body);
    body.field = trigger_field{"", 1, 1};
    require_not_written(body);
    body.field = trigger_field{"c", 1, 65537};
    require_not_written(body);
    body.field = trigger_field{"c", 1, 1};
    body.request = trigger_request{1, 0, 1};
    require_not_written(body);
    body.request = trigger_request{1, 1, max_api_number + 1};
    require_not_written(body);
    body.request = trigger_request{1, 65536, max_api_number};
    (void)round_trip(body);
}

TEST_CASE("plugin api: the check value of a trigger comes in a pair")
{
    using M = trigger_publish_message;
    const auto with = [](std::string_view more)
    { return trigger_head + "," + std::string(more) + "}"; };
    (void)accepted<M>(with(R"("expected":"3A91","actual":"3A90")"));
    REQUIRE_EQ(
        accepted<M>(with(R"("expected":"3A91","actual":"3A90")")).expected,
        std::string("3A91"));
    require_refused<M>(with(R"("expected":"3A91")"),
                       "trigger.publish.actual: must be given when the other "
                       "is");
    require_refused<M>(with(R"("actual":"3A90")"),
                       "trigger.publish.expected: must be given when the other "
                       "is");
    require_refused<M>(with(R"("expected":"","actual":"3A90")"),
                       "trigger.publish.expected: must not be empty");
    require_refused<M>(with(R"("expected":"3A91","actual":"")"),
                       "trigger.publish.actual: must not be empty");
    require_refused<M>(with(R"("expected":4,"actual":"3A90")"),
                       "trigger.publish.expected: must be a string");
    require_refused<M>(with(R"("expected":"3A91","actual":"a\nb")"),
                       "trigger.publish.actual: must be plain text");
    (void)accepted<M>(with("\"expected\":\"" + repeated('a', max_api_name) +
                           "\",\"actual\":\"" + repeated('b', max_api_name) +
                           "\""));
    require_refused<M>(with("\"expected\":\"" +
                            repeated('a', max_api_name + 1) +
                            "\",\"actual\":\"1\""),
                       "trigger.publish.expected: is longer than 128 bytes");
    require_refused<M>(with("\"expected\":\"1\",\"actual\":\"" +
                            repeated('b', max_api_name + 1) + "\""),
                       "trigger.publish.actual: is longer than 128 bytes");

    trigger_publish_message body = sample_trigger();
    body.actual.clear();
    require_not_written(body);
    body.actual = "1";
    body.expected.clear();
    require_not_written(body);
    body.expected = repeated('a', max_api_name + 1);
    require_not_written(body);
    body.expected.clear();
    body.actual.clear();
    (void)round_trip(body);
}

TEST_CASE("plugin api: the range that a check value covers is a pair")
{
    using M = trigger_publish_message;
    const auto with = [](std::string_view covers)
    { return trigger_head + R"(,"covers":)" + std::string(covers) + "}"; };
    REQUIRE(accepted<M>(with("[1,6]")).covers ==
            std::optional<trigger_covers>(trigger_covers{1, 6}));
    REQUIRE(accepted<M>(with("[4,4]")).covers ==
            std::optional<trigger_covers>(trigger_covers{4, 4}));
    REQUIRE(
        accepted<M>(with("[0," + number_text(max_api_number) + "]")).covers ==
        std::optional<trigger_covers>(trigger_covers{0, max_api_number}));
    require_refused<M>(with("[6,1]"),
                       "trigger.publish.covers[1]: must not be before the "
                       "first entry");
    require_refused<M>(with("[]"),
                       "trigger.publish.covers: must have exactly 2 entries");
    require_refused<M>(with("[1]"),
                       "trigger.publish.covers: must have exactly 2 entries");
    require_refused<M>(with("[1,2,3]"),
                       "trigger.publish.covers: has more than 2 entries");
    require_refused<M>(with("{}"), "trigger.publish.covers: must be a list");
    require_refused<M>(with(R"(["1",2])"),
                       "trigger.publish.covers[0]: must be a whole number");
    require_refused<M>(with("[1,-2]"),
                       "trigger.publish.covers[1]: must be a whole number");
    require_refused<M>(with("[1.5,2]"),
                       "trigger.publish.covers[0]: must be a whole number");
    require_refused<M>(with("[1," + number_text(max_api_number + 1) + "]"),
                       "trigger.publish.covers[1]: must be a whole number");

    trigger_publish_message body = sample_trigger();
    body.covers = trigger_covers{6, 1};
    require_not_written(body);
    body.covers = trigger_covers{1, max_api_number + 1};
    require_not_written(body);
    body.covers = trigger_covers{1, 1};
    (void)round_trip(body);
}

TEST_CASE("plugin api: the cause of a trigger is one of two")
{
    using M = trigger_publish_message;
    const auto with = [](std::string_view cause)
    { return trigger_head + R"(,"cause":)" + std::string(cause) + "}"; };
    REQUIRE(
        accepted<M>(with(R"({"bus_fault":"overlap","event":9912})")).cause ==
        std::optional<trigger_cause>(
            trigger_cause{trigger_bus_cause{decoder_fault::overlap, 9912}}));
    REQUIRE(
        accepted<M>(with(R"({"own_action":17})")).cause ==
        std::optional<trigger_cause>(trigger_cause{trigger_own_action{17}}));
    for (const std::string_view fault :
         {"overlap", "too_soon", "line_held", "transmitter_off", "overrun"})
    {
        (void)round_trip(accepted<M>(with(
            "{\"bus_fault\":\"" + std::string(fault) + "\",\"event\":0}")));
    }
    require_refused<M>(with("[]"), "trigger.publish.cause: must be an object");
    require_refused<M>(with("{}"),
                       "trigger.publish.cause: must have bus_fault or "
                       "own_action");
    require_refused<M>(with(R"({"event":4})"),
                       "trigger.publish.cause: must have bus_fault or "
                       "own_action");
    require_refused<M>(
        with(R"({"bus_fault":"overlap","event":1,"own_action":2})"),
        "trigger.publish.cause: must have only one of bus_fault and "
        "own_action");
    require_refused<M>(with(R"({"bus_fault":"overlap"})"),
                       "trigger.publish.cause.event: is required");
    require_refused<M>(with(R"({"bus_fault":"overlap","event":-1})"),
                       "trigger.publish.cause.event: must be a whole number");
    require_refused<M>(with(R"({"bus_fault":"overlap","event":)" +
                            number_text(max_api_number + 1) + "}"),
                       "trigger.publish.cause.event: must be a whole number");
    require_refused<M>(with(R"({"bus_fault":"jam","event":1})"),
                       "trigger.publish.cause.bus_fault: must be overlap, "
                       "too_soon, line_held, transmitter_off or overrun");
    require_refused<M>(with(R"({"bus_fault":3,"event":1})"),
                       "trigger.publish.cause.bus_fault: must be overlap");
    require_refused<M>(with(R"({"own_action":"x"})"),
                       "trigger.publish.cause.own_action: must be a whole "
                       "number");
    require_refused<M>(with(R"({"own_action":1.5})"),
                       "trigger.publish.cause.own_action: must be a whole");
    (void)accepted<M>(
        with(R"({"own_action":)" + number_text(max_api_number) + "}"));

    trigger_publish_message body = sample_trigger();
    body.cause = trigger_cause{trigger_own_action{max_api_number + 1}};
    require_not_written(body);
    body.cause = trigger_cause{
        trigger_bus_cause{decoder_fault::overrun, max_api_number + 1}};
    require_not_written(body);
    body.cause = trigger_cause{trigger_own_action{3}};
    (void)round_trip(body);
    body.cause = trigger_cause{
        trigger_bus_cause{decoder_fault::transmitter_off, max_api_number}};
    (void)round_trip(body);
}

TEST_CASE("plugin api: a trigger with every optional member gives them back")
{
    // Each optional member on its own, and all of them.
    const trigger_publish_message full = sample_trigger();
    trigger_publish_message none = full;
    none.sender.clear();
    none.protocol.reset();
    none.frame.reset();
    none.field.reset();
    none.expected.clear();
    none.actual.clear();
    none.covers.reset();
    none.cause.reset();
    none.request.reset();
    (void)round_trip(none);
    (void)round_trip(full);

    trigger_publish_message one = none;
    one.protocol = full.protocol;
    (void)round_trip(one);
    one = none;
    one.frame = full.frame;
    (void)round_trip(one);
    one = none;
    one.field = full.field;
    (void)round_trip(one);
    one = none;
    one.expected = full.expected;
    one.actual = full.actual;
    (void)round_trip(one);
    one = none;
    one.covers = full.covers;
    (void)round_trip(one);
    one = none;
    one.cause = full.cause;
    (void)round_trip(one);
    one = none;
    one.request = full.request;
    (void)round_trip(one);
    one = none;
    one.sender = "meter-17";
    (void)round_trip(one);
}
