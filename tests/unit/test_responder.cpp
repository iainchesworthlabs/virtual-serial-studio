//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/test_support.hpp"

#include "support/cable.hpp"

#include <vss/studio/capture.hpp>
#include <vss/studio/replay.hpp>
#include <vss/studio/responder.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace {

using namespace std::chrono_literals;

responder_script parse_ok(const std::string& text)
{
    std::string detail;
    const auto script = parse_responder_script(text, &detail);
    if (!script)
        throw vss_test::failure("script rejected: " + detail);
    return *script;
}

std::string parse_error(const std::string& text)
{
    std::string detail;
    const auto script = parse_responder_script(text, &detail);
    REQUIRE(!script);
    REQUIRE(script.error() == make_error_code(errc::config_error));
    return detail;
}

/// The times, in milliseconds, of the data events of one direction on one
/// port, from the first event the recorder saw.  All on the engine's clock.
std::vector<time_point> times_of(const event_recorder& events,
                                 const std::string& port,
                                 data_direction direction)
{
    std::vector<time_point> times;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::data && event.name == port &&
            event.direction == direction)
            times.push_back(event.when);
    }
    return times;
}

std::int64_t ms_between(time_point from, time_point to)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

const char* const status_script = R"({
  "version": 1,
  "seed": 5,
  "rules": [
    { "name": "status", "match": "10 02 50", "reply": "10 02 00 16 10 03",
      "delay_ms": 120 }
  ]
})";

} // namespace

// -- the script file --------------------------------------------------------------

TEST_CASE("responder: a script is read with its rules and its poll list")
{
    const auto script = parse_ok(R"({
      "version": 1, "seed": 9, "name": "bench",
      "rules": [
        { "name": "a", "match": "02 30", "mask": "ff f0", "reply": "06",
          "delay_ms": 150, "jitter_ms": 20 },
        { "match": "10:02", "reply": "10 03" }
      ],
      "poll": { "start_ms": 100, "interval_ms": 1000, "cycles": 3,
        "steps": [ { "send": "02 31 03" },
                   { "send": "02 32 03", "at_ms": 250, "jitter_ms": 5 } ] }
    })");

    REQUIRE_EQ(script.rules.size(), std::size_t{2});
    REQUIRE_EQ(script.rules[0].name, std::string("a"));
    REQUIRE(script.rules[0].match == byte_buffer({0x02, 0x30}));
    REQUIRE(script.rules[0].mask == byte_buffer({0xff, 0xf0}));
    REQUIRE_EQ(script.rules[0].delay.count(), 150);
    REQUIRE_EQ(script.rules[0].jitter.count(), 20);
    REQUIRE(script.rules[1].mask.empty());
    REQUIRE_EQ(script.rules[1].delay.count(), 0);
    REQUIRE(script.seed.has_value());
    REQUIRE_EQ(*script.seed, std::uint64_t{9});
    REQUIRE(script.poll.has_value());
    REQUIRE_EQ(script.poll->start.count(), 100);
    REQUIRE_EQ(script.poll->interval.count(), 1000);
    REQUIRE_EQ(script.poll->cycles, std::uint64_t{3});
    REQUIRE_EQ(script.poll->steps.size(), std::size_t{2});
    REQUIRE_EQ(script.poll->steps[1].at.count(), 250);
}

TEST_CASE("responder: a bad script says what is wrong and where")
{
    REQUIRE(parse_error("not json").find("not JSON") != std::string::npos);
    REQUIRE(parse_error("[]").find("object") != std::string::npos);
    REQUIRE(parse_error("{}").find("no \"rules\"") != std::string::npos);
    // A misspelt key is an error, not a rule that silently never waits.
    REQUIRE(parse_error(R"({"rules":[{"match":"01","reply":"02","delay":5}]})")
                .find("rules[0]: unknown key \"delay\"") != std::string::npos);
    REQUIRE(parse_error(R"({"rules":[{"match":"01"}]})")
                .find("\"reply\" is missing") != std::string::npos);
    REQUIRE(parse_error(R"({"rules":[{"match":"zz","reply":"02"}]})")
                .find("not hex") != std::string::npos);
    REQUIRE(parse_error(R"({"rules":[{"match":"01 02","mask":"ff","reply":"02"}]})")
                .find("as many bytes") != std::string::npos);
    REQUIRE(parse_error(R"({"rules":[{"match":"01","reply":"02","delay_ms":-1}]})")
                .find("between 0 and 3600000") != std::string::npos);
    REQUIRE(parse_error(R"({"rules":[{"match":"01","reply":"02","delay_ms":1.5}]})")
                .find("whole number") != std::string::npos);
    REQUIRE(parse_error(R"({"poll":{"steps":[]}})").find("at least one step") !=
            std::string::npos);
    REQUIRE(parse_error(R"({"poll":{"cycles":4,"steps":[{"send":"01"}]}})")
                .find("interval_ms") != std::string::npos);
    REQUIRE(parse_error(R"({"version":2,"rules":[]})").find("version") !=
            std::string::npos);

    const auto missing = load_responder_script("no-such-script.json");
    REQUIRE(!missing);
}

// -- rules -------------------------------------------------------------------------

TEST_CASE("responder: the reply comes inside the window, with margin")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    // A reply 120 ms after the request, against a 200 ms window.
    auto running = responder::start(*hub, device.endpoint, parse_ok(status_script));
    REQUIRE(running);

    device.inject_read(std::string("\x10\x02\x50\x14", 4));
    REQUIRE(wait_until([&] { return (*running)->replies_sent() == 1; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return !times_of(events, "device", data_direction::tx).empty(); },
        2000ms));

    const auto request = times_of(events, "device", data_direction::rx).at(0);
    const auto reply = times_of(events, "device", data_direction::tx).at(0);
    const auto gap = ms_between(request, reply);
    // Never early, and well inside 200 ms: 80 ms of margin for a busy machine.
    REQUIRE(gap >= 118);
    REQUIRE(gap <= 195);
    REQUIRE_EQ(device.written(), std::string("\x10\x02\x00\x16\x10\x03", 6));
    REQUIRE_EQ((*running)->matches(), std::uint64_t{1});
}

TEST_CASE("responder: a delay of 200 ms is 200 ms")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    auto running = responder::start(
        *hub, device.endpoint,
        parse_ok(R"({"rules":[{"match":"aa","reply":"bb","delay_ms":200}]})"));
    REQUIRE(running);

    device.inject_read("\xaa");
    REQUIRE(wait_until(
        [&] { return !times_of(events, "device", data_direction::tx).empty(); },
        2000ms));
    const auto gap = ms_between(times_of(events, "device", data_direction::rx).at(0),
                                times_of(events, "device", data_direction::tx).at(0));
    REQUIRE(gap >= 198);
    // The upper bound only rules out a wrong unit; a loaded runner can add
    // a few hundred milliseconds.
    REQUIRE(gap <= 1000);
}

TEST_CASE("responder: a mask matches the bits it names and no others")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    auto running = responder::start(
        *hub, device.endpoint,
        parse_ok(R"({"rules":[{"match":"02 30","mask":"ff f0","reply":"06"}]})"));
    REQUIRE(running);

    device.inject_read("\x02\x4a");  // second byte's high nibble is 4, not 3
    device.inject_read("\x03\x3a");  // first byte differs
    device.inject_read("\x02\x3a");  // 0x3a & 0xf0 == 0x30: matches
    REQUIRE(wait_until([&] { return (*running)->replies_sent() == 1; }, 2000ms));
    REQUIRE_EQ((*running)->matches(), std::uint64_t{1});

    device.inject_read("\x02\x3f");
    REQUIRE(wait_until([&] { return (*running)->replies_sent() == 2; }, 2000ms));
    REQUIRE_EQ(device.written(), std::string("\x06\x06", 2));
}

TEST_CASE("responder: a request split over two reads is found once")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    auto fixed = responder::start(
        *hub, device.endpoint,
        parse_ok(R"({"rules":[{"match":"10 02 50 14","reply":"a5"}]})"));
    REQUIRE(fixed);

    device.inject_read(std::string("\x00\x10\x02", 3));
    std::this_thread::sleep_for(30ms);
    device.inject_read(std::string("\x50\x14\x10\x02\x50", 5));
    std::this_thread::sleep_for(30ms);
    device.inject_read(std::string("\x14", 1));

    // Two complete requests: one split in the middle of the first read pair,
    // and one split again.
    REQUIRE(wait_until([&] { return (*fixed)->replies_sent() == 2; }, 2000ms));
    REQUIRE_EQ((*fixed)->matches(), std::uint64_t{2});
}

TEST_CASE("responder: the earliest match wins, then the first rule")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    auto running = responder::start(
        *hub, device.endpoint,
        parse_ok(R"({"rules":[
          { "match": "02 03", "reply": "c1" },
          { "match": "01 02", "reply": "c2" },
          { "match": "01", "reply": "c3" } ]})"));
    REQUIRE(running);

    // 01 02 03: "01 02" starts first and is used up, so "02 03" cannot match.
    device.inject_read(std::string("\x01\x02\x03", 3));
    REQUIRE(wait_until([&] { return (*running)->replies_sent() == 1; }, 2000ms));
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(device.written(), std::string("\xc2", 1));

    // At the same position, the rule listed first: "01 02" before "01".
    device.inject_read(std::string("\x01\x02", 2));
    REQUIRE(wait_until([&] { return (*running)->replies_sent() == 2; }, 2000ms));
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(device.written(), std::string("\xc2\xc2", 2));
}

TEST_CASE("responder: jitter stays inside its bounds and repeats with a seed")
{
    const auto run = [] {
        auto hub = start_hub();
        auto device = add_mock(*hub, "device");
        event_recorder events;
        hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });
        auto running = responder::start(
            *hub, device.endpoint,
            parse_ok(R"({"seed": 42, "rules":[
              { "match": "aa", "reply": "bb", "delay_ms": 100, "jitter_ms": 40 }
            ]})"));
        REQUIRE(running);

        std::vector<std::int64_t> gaps;
        for (std::size_t index = 0; index < 8; ++index)
        {
            device.inject_read("\xaa");
            REQUIRE(wait_until(
                [&] {
                    return times_of(events, "device", data_direction::tx).size() ==
                           index + 1;
                },
                2000ms));
            gaps.push_back(ms_between(
                times_of(events, "device", data_direction::rx).at(index),
                times_of(events, "device", data_direction::tx).at(index)));
        }
        return gaps;
    };

    const auto gaps = run();
    for (const auto gap : gaps)
    {
        REQUIRE(gap >= 58);   // 100 - 40, never early
        REQUIRE(gap <= 190);  // 100 + 40 and a margin, inside 200 ms
    }
    // The delays are not all the same.
    REQUIRE(*std::max_element(gaps.begin(), gaps.end()) -
                *std::min_element(gaps.begin(), gaps.end()) >=
            10);
}

TEST_CASE("responder: a stopped responder says nothing more")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    auto running = responder::start(
        *hub, device.endpoint,
        parse_ok(R"({"rules":[{"match":"aa","reply":"bb","delay_ms":100}]})"));
    REQUIRE(running);

    device.inject_read("\xaa");
    REQUIRE(wait_until([&] { return (*running)->matches() == 1; }, 2000ms));
    (*running)->stop(); // before the reply is due
    std::this_thread::sleep_for(250ms);
    REQUIRE_EQ((*running)->replies_sent(), std::uint64_t{0});
    REQUIRE(device.written().empty());

    REQUIRE(!responder::start(*hub, nullptr, responder_script{}));
    REQUIRE(!responder::start(*hub, device.endpoint, responder_script{}));
}

// -- the poll list -------------------------------------------------------------------

TEST_CASE("responder: a poll list sends its steps in every cycle")
{
    auto hub = start_hub();
    auto master = add_mock(*hub, "master");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    const auto origin = clock_type::now();
    auto running = responder::start(
        *hub, master.endpoint,
        parse_ok(R"({"poll": { "start_ms": 50, "interval_ms": 200, "cycles": 3,
          "steps": [ { "send": "01" }, { "send": "02", "at_ms": 80 } ] }})"));
    REQUIRE(running);

    REQUIRE(wait_until([&] { return (*running)->poll_finished(); }, 3000ms));
    REQUIRE(wait_until([&] { return (*running)->polls_sent() == 6; }, 2000ms));
    REQUIRE_EQ(master.written(), std::string("\x01\x02\x01\x02\x01\x02", 6));

    // The recorder sees events on the hub's dispatcher strand, after the
    // responder has written, so wait for all six before reading them.
    REQUIRE(wait_until(
        [&] {
            return times_of(events, "master", data_direction::tx).size() == 6;
        },
        2000ms));

    // Steps at 50, 130, 250, 330, 450, 530 ms.
    const auto times = times_of(events, "master", data_direction::tx);
    REQUIRE_EQ(times.size(), std::size_t{6});
    const std::int64_t expected[] = {50, 130, 250, 330, 450, 530};
    for (std::size_t i = 0; i < 6; ++i)
    {
        const auto at = ms_between(origin, times[i]);
        REQUIRE(at >= expected[i] - 3);
        REQUIRE(at <= expected[i] + 100);
    }
}

TEST_CASE("responder: a poll with no interval is sent once")
{
    auto hub = start_hub();
    auto master = add_mock(*hub, "master");
    auto running = responder::start(
        *hub, master.endpoint,
        parse_ok(R"({"poll": {"steps": [ { "send": "05" } ]}})"));
    REQUIRE(running);
    REQUIRE(wait_until([&] { return (*running)->poll_finished(); }, 2000ms));
    std::this_thread::sleep_for(100ms);
    REQUIRE_EQ(master.written(), std::string("\x05", 1));
}

// -- the scenario ------------------------------------------------------------------

TEST_CASE("responder: a replayed master polls devices that answer inside 200 ms")
{
    auto hub = start_hub();

    // Each device is a port in the hub with a program on the other end of a
    // cable; here the program is a responder.
    auto first_cable = cable_end::make_pair(hub->executor(), "one");
    auto second_cable = cable_end::make_pair(hub->executor(), "two");
    auto master_cable = cable_end::make_pair(hub->executor(), "m");

    const auto make = [&](const std::string& name,
                          const std::shared_ptr<cable_end>& transport) {
        auto added = hub->add_endpoint(name, endpoint_kind::virtual_port, transport);
        REQUIRE(added);
        return *added;
    };
    auto master_port = make("master", master_cable.first);
    auto device_one = make("device-1", first_cable.first);
    auto device_two = make("device-2", second_cable.first);
    auto master_program = make("master-program", master_cable.second);
    auto program_one = make("program-1", first_cable.second);
    auto program_two = make("program-2", second_cable.second);
    REQUIRE(hub->split(master_port->id(), {device_one->id(), device_two->id()}));

    // The two device programs answer different polls.
    auto one = responder::start(
        *hub, program_one,
        parse_ok(R"({"rules":[{"match":"02 31","reply":"02 31 41 03",
                   "delay_ms":150}]})"));
    auto two = responder::start(
        *hub, program_two,
        parse_ok(R"({"rules":[{"match":"02 32","reply":"02 32 42 03",
                   "delay_ms":90, "jitter_ms":30}]})"));
    REQUIRE(one);
    REQUIRE(two);

    auto out = std::make_shared<std::ostringstream>();
    capture_options options;
    options.format = capture_format::jsonl;
    options.ports = {"master"};
    auto session = capture_session::start(*hub, out, options);

    // The master is a recording: two polls, 400 ms apart.
    const std::vector<capture_record> recording{
        [] {
            capture_record r;
            r.offset_ms = 0;
            r.data = {0x02, 0x31, 0x03};
            return r;
        }(),
        [] {
            capture_record r;
            r.offset_ms = 400;
            r.data = {0x02, 0x32, 0x03};
            return r;
        }(),
    };
    auto player = replay_player::start(*hub, master_program, recording);
    REQUIRE(player);

    // The master's port received two polls and was written two answers.
    REQUIRE(wait_until([&] { return session->records() >= 4; }, 3000ms));
    session->stop();

    std::istringstream in(out->str());
    const auto read = read_capture(in);
    REQUIRE(read);
    REQUIRE_EQ(read->records.size(), std::size_t{4});

    // Answers follow their poll by the delay, inside 200 ms.
    const auto gap_after = [&](std::int64_t poll_offset) {
        for (const auto& item : read->records)
        {
            if (item.direction == data_direction::tx && item.offset_ms >= poll_offset)
                return item.offset_ms - poll_offset;
        }
        return std::int64_t{-1};
    };
    const auto first_poll = read->records.front().offset_ms;
    const auto first_gap = gap_after(first_poll);
    REQUIRE(first_gap >= 148);
    REQUIRE(first_gap <= 199);
    std::int64_t second_poll = 0;
    for (const auto& item : read->records)
    {
        if (item.direction == data_direction::rx)
            second_poll = item.offset_ms;
    }
    REQUIRE(second_poll - first_poll >= 395);
    const auto second_gap = gap_after(second_poll);
    REQUIRE(second_gap >= 58);
    REQUIRE(second_gap <= 199);
}
