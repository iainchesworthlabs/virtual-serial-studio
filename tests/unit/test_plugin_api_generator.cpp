//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_generator.hpp>

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

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

} // namespace

TEST_CASE("plugin api: generator.start is written and read")
{
    generator_start_message body;
    body.instance = 3;
    body.place = "COM41";
    body.params = R"({"rate":9600})";
    (void)round_trip(body, 8);
    const auto written = encode_as(body, 8);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"generator.start","id":8,"instance":3,)"
                           R"("place":"COM41","params":{"rate":9600}})"));
}

TEST_CASE("plugin api: generator.start is refused when it is wrong")
{
    using message_type = generator_start_message;
    const std::string head = R"({"op":"generator.start","id":1,)";
    require_refused<message_type>(head + R"("place":"p"})",
                                  "generator.start.instance: is required");
    require_refused<message_type>(head + R"("instance":1})",
                                  "generator.start.place: is required");
    require_refused<message_type>(head + R"("instance":1,"place":""})",
                                  "generator.start.place: must not be empty");
    require_refused<message_type>(head + R"("instance":1,"place":1.5})",
                                  "generator.start.place: must be a string");
    require_refused<message_type>(head + R"("instance":1,"place":4})",
                                  "generator.start.place: must be a string");
    require_refused<message_type>(
        head + R"("instance":1,"place":")" + std::string(129, 'p') + R"("})",
        "generator.start.place: is longer than 128 bytes");
    REQUIRE_EQ(accepted<message_type>(head + R"("instance":1,"place":")" +
                                      std::string(128, 'p') + R"("})")
                   .place.size(),
               max_api_name);
    require_refused<message_type>(
        head + R"("instance":1,"place":"p","params":null})",
        "generator.start.params: must be a JSON object");
    require_refused<message_type>(head + R"("instance":1,"place":"p"})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"generator.start","re":1,"instance":1,)"
        R"("place":"p"})",
        "must have an id");
}

TEST_CASE("plugin api: generator.write keeps its bytes in the blob")
{
    generator_write_message body;
    body.instance = 2;
    body.data = bytes_of(100);
    const auto read = round_trip(body);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"generator.write","instance":2})"));
}

TEST_CASE("plugin api: generator.write has between 1 and 64 KiB")
{
    using message_type = generator_write_message;
    const std::string header = R"({"op":"generator.write","instance":2})";
    REQUIRE_EQ(accepted<message_type>(header, bytes_of(1)).data.size(),
               std::size_t{1});
    REQUIRE_EQ(accepted<message_type>(header, bytes_of(max_generator_write))
                   .data.size(),
               max_generator_write);
    require_refused<message_type>(header, "has a blob of 0 bytes");
    require_refused<message_type>(header, "has a blob of 65537 bytes",
                                  bytes_of(max_generator_write + 1));

    message_type body;
    body.instance = 2;
    require_not_written(body);
    body.data = bytes_of(max_generator_write + 1);
    require_not_written(body);
    body.data = bytes_of(max_generator_write);
    (void)round_trip(body);
    // It is a notification: no id.
    require_not_written(body, 5);
    require_refused<message_type>(
        R"({"op":"generator.write","id":4,"instance":2})",
        "neither an id nor a re", bytes_of(3));
}

TEST_CASE("plugin api: bytes do not travel in the header of generator.write")
{
    // A "bytes" member is a member this version does not know, so it is
    // passed over, and the blob is what counts.
    const auto read = accepted<generator_write_message>(
        R"({"op":"generator.write","instance":2,"bytes":"0102"})", bytes_of(3));
    REQUIRE_EQ(read.data.size(), std::size_t{3});
}

TEST_CASE("plugin api: generator.credit is written and read")
{
    generator_credit_message body;
    body.instance = 2;
    body.bytes = 4096;
    (void)round_trip(body);
    using message_type = generator_credit_message;
    const std::string head = R"({"op":"generator.credit","instance":2,)";
    REQUIRE_EQ(accepted<message_type>(head + R"("bytes":1})").bytes,
               std::uint64_t{1});
    REQUIRE_EQ(accepted<message_type>(head + R"("bytes":65536})").bytes,
               max_generator_credit);
    require_refused<message_type>(
        head + R"("bytes":0})",
        "generator.credit.bytes: must be a whole number from 1");
    require_refused<message_type>(
        head + R"("bytes":65537})",
        "generator.credit.bytes: must be a whole number from 1");
    require_refused<message_type>(head + R"("bytes":-5})",
                                  "generator.credit.bytes");
    require_refused<message_type>(head + R"("bytes":"9"})",
                                  "generator.credit.bytes");
    require_refused<message_type>(R"({"op":"generator.credit","instance":2})",
                                  "generator.credit.bytes: is required");
    require_refused<message_type>(head + R"("bytes":9})", "has no bytes", {1});
    body.bytes = max_generator_credit + 1;
    require_not_written(body);
    body.bytes = 0;
    require_not_written(body);
}

TEST_CASE("plugin api: generator.pattern is written and read")
{
    generator_pattern_message body;
    body.instance = 5;
    body.data = {0x55, 0xaa, 0x00};
    body.repeat = 0;
    body.period_us = 1000;
    body.burst = 4;
    body.jitter = 25;
    const auto read = round_trip(body, 6);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body, 6);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"generator.pattern","id":6,"instance":5,)"
                           R"("repeat":0,"period_us":1000,"burst":4,)"
                           R"("jitter":25})"));
}

TEST_CASE("plugin api: generator.pattern has defaults for what it leaves out")
{
    const auto read = accepted<generator_pattern_message>(
        R"({"op":"generator.pattern","id":1,"instance":5})", {0x41});
    REQUIRE_EQ(read.repeat, std::uint64_t{1});
    REQUIRE_EQ(read.period_us, std::uint64_t{0});
    REQUIRE_EQ(read.burst, std::uint64_t{1});
    REQUIRE_EQ(read.jitter, std::uint64_t{0});
}

TEST_CASE("plugin api: generator.pattern has the limits of its members")
{
    using message_type = generator_pattern_message;
    const std::string head =
        R"({"op":"generator.pattern","id":1,"instance":5,)";
    const byte_buffer one{1};
    // At the edge, and one past it.
    REQUIRE_EQ(
        accepted<message_type>(head + R"("repeat":4294967295})", one).repeat,
        max_pattern_repeat);
    require_refused<message_type>(
        head + R"("repeat":4294967296})",
        "generator.pattern.repeat: must be a whole number", one);
    REQUIRE_EQ(accepted<message_type>(head + R"("period_us":60000000})", one)
                   .period_us,
               max_api_delay_us);
    require_refused<message_type>(
        head + R"("period_us":60000001})",
        "generator.pattern.period_us: must be a whole number", one);
    REQUIRE_EQ(accepted<message_type>(head + R"("burst":4096})", one).burst,
               max_pattern_burst);
    require_refused<message_type>(
        head + R"("burst":4097})",
        "generator.pattern.burst: must be a whole number", one);
    require_refused<message_type>(
        head + R"("burst":0})",
        "generator.pattern.burst: must be a whole number", one);
    REQUIRE_EQ(
        accepted<message_type>(head + R"("jitter":60000000})", one).jitter,
        max_api_delay_us);
    require_refused<message_type>(
        head + R"("jitter":60000001})",
        "generator.pattern.jitter: must be a whole number", one);
    require_refused<message_type>(
        head + R"("jitter":"2"})",
        "generator.pattern.jitter: must be a whole number", one);
    require_refused<message_type>(
        head + R"("repeat":-1})",
        "generator.pattern.repeat: must be a whole number", one);
    // The bytes.
    REQUIRE_EQ(
        accepted<message_type>(head + "\"x\":1}", bytes_of(max_pattern_bytes))
            .data.size(),
        max_pattern_bytes);
    require_refused<message_type>(head + "\"x\":1}", "has a blob of 0 bytes");
    require_refused<message_type>(head + "\"x\":1}", "has a blob of 1025 bytes",
                                  bytes_of(max_pattern_bytes + 1));
}

TEST_CASE("plugin api: generator.stop and generator.done are written and read")
{
    generator_stop_message stop;
    stop.instance = 8;
    (void)round_trip(stop, 2);
    require_refused<generator_stop_message>(R"({"op":"generator.stop","id":2})",
                                            "generator.stop.instance: is");

    generator_done_message done;
    done.instance = 8;
    done.reason = "finished";
    (void)round_trip(done);
    const auto written = encode_as(done);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"generator.done","instance":8,)"
                           R"("reason":"finished"})"));
}

TEST_CASE("plugin api: the reason of generator.done is limited plain text")
{
    using message_type = generator_done_message;
    const std::string head = R"({"op":"generator.done","instance":8,)";
    REQUIRE_EQ(accepted<message_type>(head + R"("reason":")" +
                                      repeated('r', max_api_text) + R"("})")
                   .reason.size(),
               max_api_text);
    require_refused<message_type>(
        head + R"("reason":")" + repeated('r', max_api_text + 1) + R"("})",
        "generator.done.reason: is longer than 200 bytes");
    require_refused<message_type>(head + R"("reason":""})",
                                  "generator.done.reason: must not be empty");
    require_refused<message_type>(head + R"("reason":"a\nb"})",
                                  "generator.done.reason: must be plain text");
    require_refused<message_type>(head + "\"reason\":\"a\xe2\x80\x8f"
                                         "b\"}",
                                  "generator.done.reason: must be plain text");
    require_refused<message_type>(R"({"op":"generator.done","instance":8})",
                                  "generator.done.reason: is required");
    require_refused<message_type>(head + R"("reason":7})",
                                  "generator.done.reason: must be a string");
    message_type body;
    body.instance = 8;
    body.reason = repeated('r', max_api_text + 1);
    require_not_written(body);
    body.reason = "line\n";
    require_not_written(body);
}

TEST_CASE("plugin api: generator.stop takes only an instance")
{
    using message_type = generator_stop_message;
    require_refused<message_type>(
        R"({"op":"generator.stop","id":2,"instance":0})",
        "generator.stop.instance: must be a whole");
    require_refused<message_type>(
        R"({"op":"generator.stop","id":2,"instance":"1"})",
        "generator.stop.instance: must be a whole");
    require_refused<message_type>(
        R"({"op":"generator.stop","id":2,"instance":1})", "has no bytes", {1});
    require_refused<message_type>(R"({"op":"generator.stop","instance":1})",
                                  "must have an id");
    require_refused<message_type>(
        R"({"op":"generator.stop","re":2,"instance":1})",
        "must have an id and no re");
    message_type body;
    body.instance = 1;
    require_not_written(body, 0);
    body.instance = 0;
    require_not_written(body, 1);
}
