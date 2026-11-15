//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_capture.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

/// `["p0","p1",...]` with `count` names.
std::string place_list(std::size_t count)
{
    std::string out = "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            out.append(",");
        out.append("\"p");
        out.append(std::to_string(i));
        out.append("\"");
    }
    out.append("]");
    return out;
}

/// `[{"name":"f0","size":1},...]` with `count` files.
std::string file_list(std::size_t count)
{
    std::string out = "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            out.append(",");
        out.append("{\"name\":\"f");
        out.append(std::to_string(i));
        out.append("\",\"size\":1}");
    }
    out.append("]");
    return out;
}

const std::string start_head =
    R"({"op":"capture.start","id":1,"places":["a"],)";

capture_start_message plain_start()
{
    capture_start_message body;
    body.places = {"COM7"};
    return body;
}

capture_start_message ring_start()
{
    capture_start_message body = plain_start();
    body.ring = 10;
    return body;
}

} // namespace

// -- capture.start
// -------------------------------------------------------------

TEST_CASE("plugin api: capture.start is written and read")
{
    capture_start_message body;
    body.places = {"COM7", "bus 1"};
    body.limit_bytes = 1000;
    body.limit_seconds = 60;
    (void)round_trip(body, 2);
    const auto written = encode_as(body, 2);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"capture.start","id":2,)"
                           R"("places":["COM7","bus 1"],"limit_bytes":1000,)"
                           R"("limit_seconds":60,"ring":0})"));

    capture_start_message ring = ring_start();
    ring.snapshot_on = snapshot_spec{{"check", "timeout"}, {"bus-1"}};
    ring.before_s = 5;
    ring.after_s = 3;
    const auto read = round_trip(ring, 9);
    REQUIRE(read.snapshot_on.has_value());
    const auto wire = encode_as(ring, 9);
    REQUIRE(wire.has_value());
    REQUIRE_EQ(header_of(wire.value()),
               std::string(R"({"op":"capture.start","id":9,"places":["COM7"],)"
                           R"("limit_bytes":67108864,"limit_seconds":600,)"
                           R"("ring":10,"snapshot_on":{"kinds":)"
                           R"(["check","timeout"],"places":["bus-1"]},)"
                           R"("before_s":5,"after_s":3})"));

    // A snapshot without places is every place of the recording.
    ring.snapshot_on = snapshot_spec{{"decoder_lost"}, {}};
    ring.before_s = 0;
    ring.after_s = 0;
    (void)round_trip(ring, 3);
}

TEST_CASE("plugin api: capture.start has defaults for what it leaves out")
{
    const auto read = accepted<capture_start_message>(
        R"({"op":"capture.start","id":1,"places":["a"]})");
    REQUIRE_EQ(read.limit_bytes, default_capture_bytes);
    REQUIRE_EQ(read.limit_bytes, std::uint64_t{67108864});
    REQUIRE_EQ(read.limit_seconds, default_capture_seconds);
    REQUIRE_EQ(read.limit_seconds, std::uint64_t{600});
    REQUIRE_EQ(read.ring, std::uint64_t{0});
    REQUIRE(!read.snapshot_on.has_value());
    REQUIRE_EQ(read.before_s, std::uint64_t{0});
    REQUIRE_EQ(read.after_s, std::uint64_t{0});
    REQUIRE_EQ(read.places.size(), std::size_t{1});

    // A ring without a snapshot is accepted.
    const auto ring = accepted<capture_start_message>(
        R"({"op":"capture.start","id":1,"places":["a"],"ring":5})");
    REQUIRE_EQ(ring.ring, std::uint64_t{5});
    REQUIRE(!ring.snapshot_on.has_value());
}

TEST_CASE("plugin api: capture.start is refused when it is wrong")
{
    using message_type = capture_start_message;
    const std::string head = R"({"op":"capture.start","id":1,"places":)";
    require_refused<message_type>(R"({"op":"capture.start","id":1})",
                                  "capture.start.places: is required");
    require_refused<message_type>(
        head + "[]}", "capture.start.places: must have at least 1 entry");
    require_refused<message_type>(head + R"("a"})",
                                  "capture.start.places: must be a list");
    require_refused<message_type>(head + "[1]}",
                                  "capture.start.places[0]: must be a string");
    require_refused<message_type>(head + R"(["a",null]})",
                                  "capture.start.places[1]: must be a string");
    require_refused<message_type>(
        head + R"([""]})", "capture.start.places[0]: must be 1 to 128 bytes");
    require_refused<message_type>(
        head + R"(["a\nb"]})", "capture.start.places[0]: must be plain text");
    require_refused<message_type>(
        head + "[\"a\xe2\x80\x8f"
               "b\"]}",
        "capture.start.places[0]: must be plain text");
    require_refused<message_type>(head + R"(["a","b","a"]})",
                                  "capture.start.places[2]: is given twice");
    require_refused<message_type>(
        start_head + R"("limit_bytes":"9"})",
        "capture.start.limit_bytes: must be a whole number");
    require_refused<message_type>(
        start_head + R"("limit_bytes":1.5})",
        "capture.start.limit_bytes: must be a whole number");
    require_refused<message_type>(
        start_head + R"("limit_seconds":null})",
        "capture.start.limit_seconds: must be a whole number");
    require_refused<message_type>(start_head + R"("ring":-1})",
                                  "capture.start.ring: must be a whole number");
    require_refused<message_type>(start_head + R"("ring":true})",
                                  "capture.start.ring: must be a whole number");
    require_refused<message_type>(start_head + R"("ring":0})", "has no bytes",
                                  {1});
    require_refused<message_type>(
        R"({"op":"capture.start","re":1,"places":["a"]})", "must have an id");
    require_refused<message_type>(R"({"op":"capture.start","places":["a"]})",
                                  "must have an id");
}

TEST_CASE("plugin api: capture.start has the limits of its members")
{
    using message_type = capture_start_message;
    REQUIRE_EQ(
        accepted<message_type>(start_head + R"("limit_bytes":1})").limit_bytes,
        std::uint64_t{1});
    REQUIRE_EQ(
        accepted<message_type>(start_head + R"("limit_bytes":268435456})")
            .limit_bytes,
        max_capture_bytes);
    require_refused<message_type>(
        start_head + R"("limit_bytes":268435457})",
        "capture.start.limit_bytes: must be a whole number "
        "from 1 to 268435456");
    require_refused<message_type>(
        start_head + R"("limit_bytes":0})",
        "capture.start.limit_bytes: must be a whole number");
    REQUIRE_EQ(accepted<message_type>(start_head + R"("limit_seconds":1})")
                   .limit_seconds,
               std::uint64_t{1});
    REQUIRE_EQ(accepted<message_type>(start_head + R"("limit_seconds":3600})")
                   .limit_seconds,
               max_capture_seconds);
    require_refused<message_type>(
        start_head + R"("limit_seconds":3601})",
        "capture.start.limit_seconds: must be a whole number "
        "from 1 to 3600");
    require_refused<message_type>(
        start_head + R"("limit_seconds":0})",
        "capture.start.limit_seconds: must be a whole number");
    REQUIRE_EQ(accepted<message_type>(start_head + R"("ring":60})").ring,
               max_capture_ring_minutes);
    require_refused<message_type>(
        start_head + R"("ring":61})",
        "capture.start.ring: must be a whole number from 0 "
        "to 60");

    const std::string many = R"({"op":"capture.start","id":1,"places":)";
    REQUIRE_EQ(
        accepted<message_type>(many + place_list(256) + "}").places.size(),
        std::size_t{256});
    require_refused<message_type>(
        many + place_list(257) + "}",
        "capture.start.places: has more than 256 entries");
    REQUIRE_EQ(
        accepted<message_type>(many + "[\"" + repeated('n', 128) + "\"]}")
            .places[0]
            .size(),
        max_api_name);
    require_refused<message_type>(
        many + "[\"" + repeated('n', 129) + "\"]}",
        "capture.start.places[0]: must be 1 to 128 bytes");

    // The same, written.
    message_type body = plain_start();
    body.limit_bytes = max_capture_bytes;
    body.limit_seconds = max_capture_seconds;
    body.ring = max_capture_ring_minutes;
    (void)round_trip(body, 1);
    body.limit_bytes = max_capture_bytes + 1;
    require_not_written(body, 1);
    body.limit_bytes = 0;
    require_not_written(body, 1);
    body.limit_bytes = 1;
    body.limit_seconds = max_capture_seconds + 1;
    require_not_written(body, 1);
    body.limit_seconds = 0;
    require_not_written(body, 1);
    body.limit_seconds = 1;
    body.ring = max_capture_ring_minutes + 1;
    require_not_written(body, 1);
    body.ring = 0;
    (void)round_trip(body, 1);
    body.places.clear();
    require_not_written(body, 1);
    body.places = {"a", "b", "a"};
    require_not_written(body, 1);
    body.places.clear();
    for (std::size_t i = 0; i < 256; ++i)
        body.places.push_back("p" + std::to_string(i));
    (void)round_trip(body, 1);
    body.places.push_back("more");
    require_not_written(body, 1);
    body.places.pop_back();
    body.places[3] = repeated('n', 129);
    require_not_written(body, 1);
    body.places[3] = "";
    require_not_written(body, 1);
    body.places[3] = "a\nb";
    require_not_written(body, 1);
    body.places[3] = "ok";
    // A request needs a number.
    require_not_written(body, 0);
}

TEST_CASE("plugin api: a snapshot is accepted only with a ring")
{
    using message_type = capture_start_message;
    const std::string snapshot =
        R"("snapshot_on":{"kinds":["check"],"places":[]})";
    const std::string only = "is accepted only when ring is not 0";
    require_refused<message_type>(start_head + snapshot + "}",
                                  "capture.start.snapshot_on: " + only);
    require_refused<message_type>(start_head + R"("ring":0,)" + snapshot + "}",
                                  "capture.start.snapshot_on: " + only);
    require_refused<message_type>(start_head + R"("before_s":1})",
                                  "capture.start.before_s: " + only);
    require_refused<message_type>(start_head + R"("before_s":0})",
                                  "capture.start.before_s: " + only);
    require_refused<message_type>(start_head + R"("after_s":1})",
                                  "capture.start.after_s: " + only);
    require_refused<message_type>(start_head + R"("ring":0,"after_s":0})",
                                  "capture.start.after_s: " + only);

    const auto read =
        accepted<message_type>(start_head + R"("ring":1,)" + snapshot +
                               R"(,"before_s":2,"after_s":3})");
    REQUIRE(read.snapshot_on.has_value());
    REQUIRE_EQ(read.before_s, std::uint64_t{2});
    REQUIRE_EQ(read.after_s, std::uint64_t{3});

    // And written.
    message_type body = plain_start();
    body.snapshot_on = snapshot_spec{{"check"}, {}};
    require_not_written(body, 1);
    body.snapshot_on.reset();
    body.before_s = 1;
    require_not_written(body, 1);
    body.before_s = 0;
    body.after_s = 1;
    require_not_written(body, 1);
    body.after_s = 0;
    (void)round_trip(body, 1);
    body.ring = 1;
    body.snapshot_on = snapshot_spec{{"check"}, {}};
    body.before_s = 1;
    body.after_s = 1;
    (void)round_trip(body, 1);
}

TEST_CASE("plugin api: the snapshot of capture.start has its limits")
{
    using message_type = capture_start_message;
    const std::string head = start_head + R"("ring":3,)";
    const auto kinds = [&head](std::string_view list)
    { return head + R"("snapshot_on":{"kinds":)" + std::string(list) + "}}"; };
    // Every kind of section 20.7 is accepted.
    const auto all = accepted<message_type>(kinds(
        R"(["framing","unknown_type","unknown_value","check","length",)"
        R"("timeout","sequence","contention","line_error","decoder_lost",)"
        R"("custom"])"));
    REQUIRE(all.snapshot_on.has_value());
    if (all.snapshot_on.has_value())
    {
        REQUIRE_EQ(all.snapshot_on->kinds.size(), std::size_t{11});
        REQUIRE(all.snapshot_on->places.empty());
    }
    REQUIRE_EQ(snapshot_kinds.size(), std::size_t{11});

    require_refused<message_type>(
        head + R"("snapshot_on":null})",
        "capture.start.snapshot_on: must be an object");
    require_refused<message_type>(
        head + R"("snapshot_on":[]})",
        "capture.start.snapshot_on: must be an object");
    require_refused<message_type>(
        head + R"("snapshot_on":{}})",
        "capture.start.snapshot_on.kinds: is required");
    require_refused<message_type>(
        kinds("[]"), "capture.start.snapshot_on.kinds: must have at least "
                     "1 entry");
    require_refused<message_type>(
        kinds(R"("check")"), "capture.start.snapshot_on.kinds: must be a list");
    require_refused<message_type>(
        kinds(R"(["check","bogus"])"),
        "capture.start.snapshot_on.kinds[1]: must be a kind of "
        "trigger");
    require_refused<message_type>(
        kinds(R"(["Check"])"),
        "capture.start.snapshot_on.kinds[0]: must be a kind");
    require_refused<message_type>(
        kinds("[3]"), "capture.start.snapshot_on.kinds[0]: must be a kind");
    require_refused<message_type>(
        kinds(R"(["check","timeout","check"])"),
        "capture.start.snapshot_on.kinds[2]: is given twice");
    require_refused<message_type>(
        kinds(place_list(17)),
        "capture.start.snapshot_on.kinds: has more than 16 "
        "entries");

    const std::string with_places =
        head + R"("snapshot_on":{"kinds":["check"],"places":)";
    const auto many =
        accepted<message_type>(with_places + place_list(256) + "}}");
    REQUIRE(many.snapshot_on.has_value());
    if (many.snapshot_on.has_value())
        REQUIRE_EQ(many.snapshot_on->places.size(), std::size_t{256});
    require_refused<message_type>(
        with_places + place_list(257) + "}}",
        "capture.start.snapshot_on.places: has more than 256 "
        "entries");
    require_refused<message_type>(
        with_places + R"([""]}})",
        "capture.start.snapshot_on.places[0]: must be 1 to "
        "128 bytes");
    require_refused<message_type>(
        with_places + R"([5]}})",
        "capture.start.snapshot_on.places[0]: must be a "
        "string");
    require_refused<message_type>(
        with_places + R"(["a\u0001"]}})",
        "capture.start.snapshot_on.places[0]: must be plain "
        "text");
    require_refused<message_type>(
        with_places + R"("a"}})",
        "capture.start.snapshot_on.places: must be a list");

    // The seconds around a trigger.
    const std::string tail = head + R"("snapshot_on":{"kinds":["check"]},)";
    REQUIRE_EQ(accepted<message_type>(tail + R"("before_s":600,"after_s":600})")
                   .before_s,
               max_snapshot_seconds);
    require_refused<message_type>(
        tail + R"("before_s":601})",
        "capture.start.before_s: must be a whole number from "
        "0 to 600");
    require_refused<message_type>(
        tail + R"("after_s":601})",
        "capture.start.after_s: must be a whole number from 0 "
        "to 600");
    require_refused<message_type>(
        tail + R"("after_s":-1})",
        "capture.start.after_s: must be a whole number");
    require_refused<message_type>(
        tail + R"("before_s":"1"})",
        "capture.start.before_s: must be a whole number");

    // Written.
    message_type body = ring_start();
    body.snapshot_on = snapshot_spec{{"check"}, {}};
    body.before_s = max_snapshot_seconds;
    body.after_s = max_snapshot_seconds;
    (void)round_trip(body, 1);
    body.before_s = max_snapshot_seconds + 1;
    require_not_written(body, 1);
    body.before_s = 0;
    body.after_s = max_snapshot_seconds + 1;
    require_not_written(body, 1);
    body.after_s = 0;
    body.snapshot_on = snapshot_spec{{}, {}};
    require_not_written(body, 1);
    body.snapshot_on = snapshot_spec{{"check", "check"}, {}};
    require_not_written(body, 1);
    body.snapshot_on = snapshot_spec{{"bogus"}, {}};
    require_not_written(body, 1);
    body.snapshot_on = snapshot_spec{{"check"}, {""}};
    require_not_written(body, 1);
    snapshot_spec spec{{"check"}, {}};
    for (std::size_t i = 0; i < 256; ++i)
        spec.places.push_back("p" + std::to_string(i));
    body.snapshot_on = spec;
    (void)round_trip(body, 1);
    spec.places.push_back("more");
    body.snapshot_on = spec;
    require_not_written(body, 1);
}

// -- capture.stop, capture.status, capture.result
// --------------------------------

TEST_CASE("plugin api: capture.stop and capture.status have no members")
{
    (void)round_trip(capture_stop_message{}, 4);
    (void)round_trip(capture_status_message{}, 5);
    const auto stop = encode_as(capture_stop_message{}, 4);
    REQUIRE(stop.has_value());
    REQUIRE_EQ(header_of(stop.value()),
               std::string(R"({"op":"capture.stop","id":4})"));
    const auto status = encode_as(capture_status_message{}, 5);
    REQUIRE(status.has_value());
    REQUIRE_EQ(header_of(status.value()),
               std::string(R"({"op":"capture.status","id":5})"));

    // A member this version does not know is passed over.
    (void)accepted<capture_stop_message>(
        R"({"op":"capture.stop","id":1,"later":1})");
    (void)accepted<capture_status_message>(
        R"({"op":"capture.status","id":1,"later":1})");
    require_refused<capture_stop_message>(R"({"op":"capture.stop","id":1})",
                                          "has no bytes", {1});
    require_refused<capture_status_message>(R"({"op":"capture.status","id":1})",
                                            "has no bytes", {1});
    require_refused<capture_stop_message>(R"({"op":"capture.stop"})",
                                          "must have an id");
    require_refused<capture_status_message>(R"({"op":"capture.status","re":1})",
                                            "must have an id");
    require_not_written(capture_stop_message{}, 0);
    require_not_written(capture_status_message{}, 0);
}

TEST_CASE("plugin api: capture.result is written and read")
{
    capture_result_message body;
    body.running = true;
    body.bytes = 48213;
    body.seconds = 184;
    body.dropped = 0;
    body.ring = true;
    (void)round_trip(body, 5);
    const auto written = encode_as(body, 5);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"capture.result","re":5,"running":true,)"
                           R"("bytes":48213,"seconds":184,"dropped":0,)"
                           R"("ring":true})"));
    (void)round_trip(capture_result_message{}, 6);
}

TEST_CASE("plugin api: capture.result is refused when it is wrong")
{
    using message_type = capture_result_message;
    const std::string full = R"("running":false,"bytes":1,"seconds":2,)"
                             R"("dropped":3,"ring":false})";
    const std::string head = R"({"op":"capture.result","re":1,)";
    (void)accepted<message_type>(head + full);
    require_refused<message_type>(R"({"op":"capture.result","re":1,"bytes":1,)"
                                  R"("seconds":2,"dropped":3,"ring":false})",
                                  "capture.result.running: is required");
    require_refused<message_type>(
        head + R"("running":false,"seconds":2,"dropped":3,)"
               R"("ring":false})",
        "capture.result.bytes: is required");
    require_refused<message_type>(
        head + R"("running":false,"bytes":1,"dropped":3,)"
               R"("ring":false})",
        "capture.result.seconds: is required");
    require_refused<message_type>(
        head + R"("running":false,"bytes":1,"seconds":2,)"
               R"("ring":false})",
        "capture.result.dropped: is required");
    require_refused<message_type>(
        head + R"("running":false,"bytes":1,"seconds":2,)"
               R"("dropped":3})",
        "capture.result.ring: is required");
    require_refused<message_type>(
        head + R"("running":"no","bytes":1,"seconds":2,)"
               R"("dropped":3,"ring":false})",
        "capture.result.running: must be true or false");
    require_refused<message_type>(
        head + R"("running":false,"bytes":-1,"seconds":2,)"
               R"("dropped":3,"ring":false})",
        "capture.result.bytes: must be a whole number");
    require_refused<message_type>(
        head + R"("running":false,"bytes":1,"seconds":"2",)"
               R"("dropped":3,"ring":false})",
        "capture.result.seconds: must be a whole number");
    require_refused<message_type>(
        head + R"("running":false,"bytes":1,"seconds":2,)"
               R"("dropped":3,"ring":1})",
        "capture.result.ring: must be true or false");
    require_refused<message_type>(head + full, "has no bytes", {1});
    require_refused<message_type>(R"({"op":"capture.result","id":1,)" + full,
                                  "must have a re and no id");
    require_refused<message_type>(R"({"op":"capture.result",)" + full,
                                  "must have a re and no id");

    // The numbers go up to max_api_number.
    const std::string most = std::to_string(max_api_number);
    const auto edge = accepted<message_type>(
        head + R"("running":false,"bytes":)" + most + R"(,"seconds":)" + most +
        R"(,"dropped":)" + most + R"(,"ring":false})");
    REQUIRE_EQ(edge.bytes, max_api_number);
    REQUIRE_EQ(edge.seconds, max_api_number);
    REQUIRE_EQ(edge.dropped, max_api_number);
    const std::string past = std::to_string(max_api_number + 1);
    require_refused<message_type>(
        head + R"("running":false,"bytes":)" + past +
            R"(,"seconds":0,"dropped":0,"ring":false})",
        "capture.result.bytes: must be a whole number");
    require_refused<message_type>(
        head + R"("running":false,"bytes":0,"seconds":)" + past +
            R"(,"dropped":0,"ring":false})",
        "capture.result.seconds: must be a whole number");
    require_refused<message_type>(
        head +
            R"("running":false,"bytes":0,"seconds":0,)"
            R"("dropped":)" +
            past + R"(,"ring":false})",
        "capture.result.dropped: must be a whole number");

    message_type body;
    body.bytes = max_api_number;
    body.seconds = max_api_number;
    body.dropped = max_api_number;
    (void)round_trip(body, 1);
    body.bytes = max_api_number + 1;
    require_not_written(body, 1);
    body.bytes = 0;
    body.seconds = max_api_number + 1;
    require_not_written(body, 1);
    body.seconds = 0;
    body.dropped = max_api_number + 1;
    require_not_written(body, 1);
    body.dropped = 0;
    // An answer needs the number of its request.
    require_not_written(body, 0);
}

// -- timeline.load
// -----------------------------------------------------------------

TEST_CASE("plugin api: timeline.load is written and read")
{
    timeline_load_message body;
    body.instance = 2;
    body.recording = "traffic/1.jsonl";
    body.place = "COM7";
    body.time_scale = 0.5;
    body.seed = 7;
    (void)round_trip(body, 3);
    const auto written = encode_as(body, 3);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"timeline.load","id":3,"instance":2,)"
                           R"("recording":"traffic/1.jsonl","place":"COM7",)"
                           R"("time_scale":5E-1,"seed":7})"));

    body.seed.reset();
    body.time_scale = 2.5;
    (void)round_trip(body, 3);
    const auto without = encode_as(body, 3);
    REQUIRE(without.has_value());
    REQUIRE_EQ(header_of(without.value()),
               std::string(R"({"op":"timeline.load","id":3,"instance":2,)"
                           R"("recording":"traffic/1.jsonl","place":"COM7",)"
                           R"("time_scale":2.5E0})"));
}

TEST_CASE("plugin api: timeline.load has defaults for what it leaves out")
{
    const auto read = accepted<timeline_load_message>(
        R"({"op":"timeline.load","id":1,"instance":4,"recording":"r.jsonl",)"
        R"("place":"COM7"})");
    REQUIRE_EQ(read.instance, std::uint64_t{4});
    REQUIRE_EQ(read.recording, std::string("r.jsonl"));
    REQUIRE_EQ(read.place, std::string("COM7"));
    REQUIRE(read.time_scale == 1.0);
    REQUIRE(!read.seed.has_value());
    (void)round_trip(read, 1);

    // A whole number is a time scale too.
    const auto whole = accepted<timeline_load_message>(
        R"({"op":"timeline.load","id":1,"instance":4,"recording":"r.jsonl",)"
        R"("place":"COM7","time_scale":5,"seed":0})");
    REQUIRE(whole.time_scale == 5.0);
    REQUIRE(whole.seed.has_value());
    if (whole.seed.has_value())
        REQUIRE_EQ(*whole.seed, std::uint64_t{0});
    (void)round_trip(whole, 1);
}

TEST_CASE("plugin api: a time scale of timeline.load reads back as itself")
{
    timeline_load_message body;
    body.instance = 1;
    body.recording = "r.jsonl";
    body.place = "COM7";
    // These are exact in any reader.
    for (const double scale : {0.01, 0.125, 0.5, 1.0, 2.0, 10.0, 100.0})
    {
        body.time_scale = scale;
        const auto read = round_trip(body, 1);
        REQUIRE(read.time_scale == scale);
    }
    // Any other is written and read back as the same double, or it is
    // not written.
    for (const double scale : {0.1, 0.3, 0.7, 1.1, 3.14159, 12.345, 33.3, 99.99,
                               1.0 / 3.0, 100.0 / 7.0})
    {
        body.time_scale = scale;
        const auto written = encode_as(body, 1);
        if (written.has_error())
        {
            REQUIRE(written.error() == api_errc::bad_request);
            continue;
        }
        const auto read = round_trip(body, 1);
        REQUIRE(read.time_scale == scale);
    }
}

TEST_CASE("plugin api: timeline.load is refused when it is wrong")
{
    using message_type = timeline_load_message;
    const std::string head = R"({"op":"timeline.load","id":1,)";
    const std::string rest = R"("recording":"r","place":"p")";
    require_refused<message_type>(head + rest + "}",
                                  "timeline.load.instance: is required");
    require_refused<message_type>(
        head + R"("instance":0,)" + rest + "}",
        "timeline.load.instance: must be a whole number");
    require_refused<message_type>(
        head + R"("instance":"1",)" + rest + "}",
        "timeline.load.instance: must be a whole number");
    require_refused<message_type>(head + R"("instance":1,"place":"p"})",
                                  "timeline.load.recording: is required");
    require_refused<message_type>(head + R"("instance":1,"recording":"r"})",
                                  "timeline.load.place: is required");
    require_refused<message_type>(
        head + R"("instance":1,"recording":"","place":"p"})",
        "timeline.load.recording: must not be empty");
    require_refused<message_type>(
        head + R"("instance":1,"recording":3,"place":"p"})",
        "timeline.load.recording: must be a string");
    require_refused<message_type>(
        head + R"("instance":1,"recording":"a\nb","place":"p"})",
        "timeline.load.recording: must be plain text");
    require_refused<message_type>(
        head + R"("instance":1,"recording":"r","place":""})",
        "timeline.load.place: must not be empty");
    require_refused<message_type>(
        head + R"("instance":1,"recording":"r","place":null})",
        "timeline.load.place: must be a string");
    require_refused<message_type>(
        "{\"op\":\"timeline.load\",\"id\":1,\"instance\":1,"
        "\"recording\":\"r\",\"place\":\"a\xe2\x80\x8f"
        "b\"}",
        "timeline.load.place: must be plain text");
    require_refused<message_type>(head + R"("instance":1,)" + rest +
                                      R"(,"seed":-1})",
                                  "timeline.load.seed: must be a whole number");
    require_refused<message_type>(head + R"("instance":1,)" + rest +
                                      R"(,"seed":1.5})",
                                  "timeline.load.seed: must be a whole number");
    require_refused<message_type>(head + R"("instance":1,)" + rest +
                                      R"(,"seed":"3"})",
                                  "timeline.load.seed: must be a whole number");
    require_refused<message_type>(head + R"("instance":1,)" + rest + "}",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"timeline.load","re":1,"instance":1,)" + rest + "}",
        "must have an id");
}

TEST_CASE("plugin api: timeline.load has the limits of its members")
{
    using message_type = timeline_load_message;
    const std::string head =
        R"({"op":"timeline.load","id":1,"instance":1,"recording":"r",)"
        R"("place":"p",)";
    const std::string scale = "timeline.load.time_scale: must be a number from";
    REQUIRE(accepted<message_type>(head + R"("time_scale":0.01})").time_scale ==
            0.01);
    REQUIRE(accepted<message_type>(head + R"("time_scale":100})").time_scale ==
            100.0);
    REQUIRE(
        accepted<message_type>(head + R"("time_scale":100.0})").time_scale ==
        100.0);
    require_refused<message_type>(head + R"("time_scale":0.009})", scale);
    require_refused<message_type>(head + R"("time_scale":100.5})", scale);
    require_refused<message_type>(head + R"("time_scale":0})", scale);
    require_refused<message_type>(head + R"("time_scale":-1})", scale);
    require_refused<message_type>(head + R"("time_scale":101})", scale);
    require_refused<message_type>(head + R"("time_scale":"2"})", scale);
    require_refused<message_type>(head + R"("time_scale":null})", scale);
    require_refused<message_type>(head + R"("time_scale":true})", scale);

    const std::string most = std::to_string(max_api_number);
    const auto top = accepted<message_type>(head + R"("seed":)" + most + "}");
    REQUIRE(top.seed.has_value());
    if (top.seed.has_value())
        REQUIRE_EQ(*top.seed, max_api_number);
    require_refused<message_type>(
        head + R"("seed":)" + std::to_string(max_api_number + 1) + "}",
        "timeline.load.seed: must be a whole number from 0");
    require_refused<message_type>(
        R"({"op":"timeline.load","id":1,"instance":)" +
            std::to_string(max_api_number + 1) +
            R"(,"recording":"r","place":"p"})",
        "timeline.load.instance: must be a whole number");
    REQUIRE_EQ(
        accepted<message_type>(R"({"op":"timeline.load","id":1,"instance":)" +
                               most + R"(,"recording":"r","place":"p"})")
            .instance,
        max_api_number);

    REQUIRE_EQ(
        accepted<message_type>(R"({"op":"timeline.load","id":1,"instance":1,)"
                               R"("recording":")" +
                               repeated('r', 1024) + R"(","place":"p"})")
            .recording.size(),
        max_recording_path);
    require_refused<message_type>(
        R"({"op":"timeline.load","id":1,"instance":1,)"
        R"("recording":")" +
            repeated('r', 1025) + R"(","place":"p"})",
        "timeline.load.recording: is longer than 1024 bytes");
    REQUIRE_EQ(
        accepted<message_type>(R"({"op":"timeline.load","id":1,"instance":1,)"
                               R"("recording":"r","place":")" +
                               repeated('p', 128) + R"("})")
            .place.size(),
        max_api_name);
    require_refused<message_type>(
        R"({"op":"timeline.load","id":1,"instance":1,)"
        R"("recording":"r","place":")" +
            repeated('p', 129) + R"("})",
        "timeline.load.place: is longer than 128 bytes");

    // Written.
    message_type body;
    body.instance = max_api_number;
    body.recording = repeated('r', 1024);
    body.place = repeated('p', 128);
    body.seed = max_api_number;
    (void)round_trip(body, 1);
    body.instance = max_api_number + 1;
    require_not_written(body, 1);
    body.instance = 0;
    require_not_written(body, 1);
    body.instance = 1;
    body.recording = repeated('r', 1025);
    require_not_written(body, 1);
    body.recording.clear();
    require_not_written(body, 1);
    body.recording = "a\nb";
    require_not_written(body, 1);
    body.recording = "r";
    body.place = repeated('p', 129);
    require_not_written(body, 1);
    body.place.clear();
    require_not_written(body, 1);
    body.place = "p";
    body.seed = max_api_number + 1;
    require_not_written(body, 1);
    body.seed.reset();
    for (const double bad :
         {0.009, 100.01, 0.0, -1.0, std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
    {
        body.time_scale = bad;
        require_not_written(body, 1);
    }
    body.time_scale = 1.0;
    (void)round_trip(body, 1);
    require_not_written(body, 0);
}

// -- bundle.prepare and bundle.result
// --------------------------------------------

TEST_CASE("plugin api: bundle.prepare is written and read")
{
    bundle_prepare_message body;
    body.section = "traffic";
    body.settings = R"({"ports":["COM7"]})";
    (void)round_trip(body, 5);
    const auto written = encode_as(body, 5);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"bundle.prepare","id":5,)"
                           R"("section":"traffic",)"
                           R"("settings":{"ports":["COM7"]}})"));

    const auto plain = accepted<bundle_prepare_message>(
        R"({"op":"bundle.prepare","id":1,"section":"notes"})");
    REQUIRE_EQ(plain.settings, std::string("{}"));
    bundle_prepare_message third_party;
    third_party.section = "com.example.log-view.events_2";
    (void)round_trip(third_party, 6);
}

TEST_CASE("plugin api: bundle.prepare is refused when it is wrong")
{
    using message_type = bundle_prepare_message;
    const std::string head = R"({"op":"bundle.prepare","id":1,)";
    const std::string token = "bundle.prepare.section: must be 1 to 128";
    require_refused<message_type>(R"({"op":"bundle.prepare","id":1})",
                                  "bundle.prepare.section: is required");
    require_refused<message_type>(head + R"("section":""})", token);
    require_refused<message_type>(head + R"("section":"Traffic"})", token);
    require_refused<message_type>(head + R"("section":"a b"})", token);
    require_refused<message_type>(head + R"("section":"a/b"})", token);
    require_refused<message_type>(head + R"("section":5})",
                                  "bundle.prepare.section: must be a string");
    require_refused<message_type>(
        head + R"("section":"traffic","settings":null})",
        "bundle.prepare.settings: must be a JSON object");
    require_refused<message_type>(
        head + R"("section":"traffic","settings":[]})",
        "bundle.prepare.settings: must be a JSON object");
    require_refused<message_type>(head + R"("section":"traffic"})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"bundle.prepare","re":1,"section":"t"})", "must have an id");
    require_refused<message_type>(R"({"op":"bundle.prepare","section":"t"})",
                                  "must have an id");

    REQUIRE_EQ(accepted<message_type>(head + R"("section":")" +
                                      repeated('s', 128) + "\"}")
                   .section.size(),
               max_api_name);
    require_refused<message_type>(
        head + R"("section":")" + repeated('s', 129) + "\"}", token);

    message_type body;
    body.section = repeated('s', 128);
    (void)round_trip(body, 1);
    body.section = repeated('s', 129);
    require_not_written(body, 1);
    body.section.clear();
    require_not_written(body, 1);
    body.section = "Traffic";
    require_not_written(body, 1);
    body.section = "traffic";
    body.settings = "[]";
    require_not_written(body, 1);
    body.settings = "{";
    require_not_written(body, 1);
    body.settings = "{}";
    require_not_written(body, 0);
}

TEST_CASE("plugin api: bundle.result is written and read")
{
    bundle_result_message body;
    body.files = {{"recording.json", 812}, {"1.jsonl", 48213}};
    const auto read = round_trip(body, 5);
    REQUIRE_EQ(read.files.size(), std::size_t{2});
    const auto written = encode_as(body, 5);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"bundle.result","re":5,"files":[)"
                           R"({"name":"recording.json","size":812},)"
                           R"({"name":"1.jsonl","size":48213}]})"));

    // A section with no file is an answer too.
    bundle_result_message none;
    (void)round_trip(none, 6);
    const auto empty = encode_as(none, 6);
    REQUIRE(empty.has_value());
    REQUIRE_EQ(header_of(empty.value()),
               std::string(R"({"op":"bundle.result","re":6,"files":[]})"));
}

TEST_CASE("plugin api: bundle.result is refused when it is wrong")
{
    using message_type = bundle_result_message;
    const std::string head = R"({"op":"bundle.result","re":1,"files":[)";
    const std::string size = "bundle.result.files[0].size: must be a whole "
                             "number";
    require_refused<message_type>(R"({"op":"bundle.result","re":1})",
                                  "bundle.result.files: is required");
    require_refused<message_type>(R"({"op":"bundle.result","re":1,"files":{}})",
                                  "bundle.result.files: must be a list");
    require_refused<message_type>(head + "3]}",
                                  "bundle.result.files[0]: must be an object");
    require_refused<message_type>(head + R"({"size":1}]})",
                                  "bundle.result.files[0].name: is required");
    require_refused<message_type>(head + R"({"name":"a"}]})",
                                  "bundle.result.files[0].size: is required");
    require_refused<message_type>(head + R"({"name":"a","size":-1}]})", size);
    require_refused<message_type>(head + R"({"name":"a","size":"1"}]})", size);
    require_refused<message_type>(head + R"({"name":"a","size":1.5}]})", size);
    require_refused<message_type>(
        head + R"({"name":"a","size":1},{"name":7,"size":1}]})",
        "bundle.result.files[1].name: must be a string");
    require_refused<message_type>(
        head + R"({"name":"a","size":1},{"name":"a","size":2}]})",
        "bundle.result.files[1].name: is given twice");
    require_refused<message_type>(head + R"({"name":"a","size":1}]})",
                                  "has no bytes", {1});
    require_refused<message_type>(R"({"op":"bundle.result","id":1,"files":[]})",
                                  "must have a re and no id");
    require_refused<message_type>(R"({"op":"bundle.result","files":[]})",
                                  "must have a re and no id");
}

TEST_CASE("plugin api: the file names of bundle.result are plain names")
{
    using message_type = bundle_result_message;
    const std::string head = R"({"op":"bundle.result","re":1,"files":[)";
    const auto one = [&head](std::string_view name)
    { return head + R"({"name":")" + std::string(name) + R"(","size":1}]})"; };
    for (const std::string_view bad :
         {"a/b", "/a", "a/", R"(a\\b)", R"(\\a)", "..", "."})
    {
        require_refused<message_type>(
            one(bad), "bundle.result.files[0].name: must be a plain file name");
    }
    // A control character is refused, whichever check sees it first.
    for (const std::string_view bad :
         {R"(a\u0001b)", R"(a\nb)", R"(a\tb)", R"(a\u007fb)", R"(a\u0000b)"})
    {
        require_refused<message_type>(one(bad),
                                      "bundle.result.files[0].name: must be");
    }
    require_refused<message_type>(
        one(""), "bundle.result.files[0].name: must not be empty");
    require_refused<message_type>(
        one(repeated('n', 129)),
        "bundle.result.files[0].name: is longer than 128 bytes");
    require_refused<message_type>(
        "{\"op\":\"bundle.result\",\"re\":1,\"files\":[{"
        "\"name\":\"a\xe2\x80\x8f"
        "b\",\"size\":1}]}",
        "bundle.result.files[0].name: must be plain text");
    for (const std::string_view good :
         {"a", "recording.json", "..x", "x..", ".hidden", "a b", "a-b_c.d",
          "\xc3\xa9.txt"})
    {
        const auto read = accepted<message_type>(one(good));
        REQUIRE_EQ(read.files.size(), std::size_t{1});
    }
    REQUIRE_EQ(
        accepted<message_type>(one(repeated('n', 128))).files[0].name.size(),
        max_bundle_file_name);

    // The same, written.
    message_type body;
    body.files = {{repeated('n', 128), 1}};
    (void)round_trip(body, 1);
    const std::string control("a\x01"
                              "b");
    const std::string delete_char("a\x7f"
                                  "b");
    const std::string with_nul("a\0b", 3);
    for (const std::string& bad :
         {std::string("a/b"), std::string("a\\b"), std::string(".."),
          std::string("."), std::string(), repeated('n', 129),
          std::string("a\nb"), control, delete_char, with_nul,
          std::string("a\xe2\x80\x8f"
                      "b")})
    {
        body.files = {{bad, 1}};
        require_not_written(body, 1);
    }
}

TEST_CASE("plugin api: bundle.result has at most 16 files of 256 MiB")
{
    using message_type = bundle_result_message;
    const std::string head = R"({"op":"bundle.result","re":1,"files":)";
    REQUIRE_EQ(accepted<message_type>(head + file_list(16) + "}").files.size(),
               std::size_t{16});
    require_refused<message_type>(
        head + file_list(17) + "}",
        "bundle.result.files: has more than 16 entries");

    // One file at the edge, one past it.
    const auto big =
        accepted<message_type>(head + R"([{"name":"a","size":268435456}]})");
    REQUIRE_EQ(big.files.size(), std::size_t{1});
    if (big.files.size() == 1)
        REQUIRE_EQ(big.files[0].size, max_bundle_section_bytes);
    require_refused<message_type>(
        head + R"([{"name":"a","size":268435457}]})",
        "bundle.result.files[0].size: must be a whole number "
        "from 0 to 268435456");
    const auto none =
        accepted<message_type>(head + R"([{"name":"a","size":0}]})");
    REQUIRE_EQ(none.files.size(), std::size_t{1});
    // The sum, at the edge and one past it.
    REQUIRE_EQ(accepted<message_type>(head +
                                      R"([{"name":"a","size":134217728},)"
                                      R"({"name":"b","size":134217728}]})")
                   .files.size(),
               std::size_t{2});
    require_refused<message_type>(
        head + R"([{"name":"a","size":134217728},)"
               R"({"name":"b","size":134217729}]})",
        "bundle.result.files: has sizes that add up to more "
        "than 268435456 bytes");
    require_refused<message_type>(head + R"([{"name":"a","size":268435456},)"
                                         R"({"name":"b","size":1}]})",
                                  "has sizes that add up to more than");
    // The sum of many large files does not overflow.
    std::string many = "[";
    for (std::size_t i = 0; i < 16; ++i)
    {
        if (i != 0)
            many.append(",");
        many.append("{\"name\":\"f");
        many.append(std::to_string(i));
        many.append("\",\"size\":268435456}");
    }
    many.append("]}");
    require_refused<message_type>(head + many,
                                  "has sizes that add up to more than");

    // Written.
    message_type body;
    for (std::size_t i = 0; i < 16; ++i)
        body.files.push_back({"f" + std::to_string(i), 1});
    (void)round_trip(body, 1);
    body.files.push_back({"f16", 1});
    require_not_written(body, 1);
    body.files = {{"a", max_bundle_section_bytes}};
    (void)round_trip(body, 1);
    body.files = {{"a", max_bundle_section_bytes + 1}};
    require_not_written(body, 1);
    body.files = {{"a", max_bundle_section_bytes / 2},
                  {"b", max_bundle_section_bytes / 2}};
    (void)round_trip(body, 1);
    body.files = {{"a", max_bundle_section_bytes / 2},
                  {"b", max_bundle_section_bytes / 2 + 1}};
    require_not_written(body, 1);
    body.files = {{"a", 1}, {"b", 1}, {"a", 1}};
    require_not_written(body, 1);
    body.files.clear();
    // An answer needs the number of its request.
    require_not_written(body, 0);
}
