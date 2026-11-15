//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/test_support.hpp"

#include <vss/studio/capture.hpp>
#include <vss/studio/replay.hpp>

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace {

using namespace std::chrono_literals;

capture_record record(std::int64_t offset, data_direction direction,
                      const std::string& port, const std::string& text)
{
    capture_record out;
    out.offset_ms = offset;
    out.direction = direction;
    out.port = port;
    out.data = to_bytes(text);
    return out;
}

/// The times, in milliseconds from `origin`, at which an endpoint wrote.
std::vector<std::int64_t> write_times(const event_recorder& events,
                                      const std::string& port, time_point origin)
{
    std::vector<std::int64_t> times;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::data &&
            event.direction == data_direction::tx && event.name == port)
        {
            times.push_back(
                std::chrono::duration_cast<std::chrono::milliseconds>(event.when - origin)
                    .count());
        }
    }
    return times;
}

std::size_t write_count(const event_recorder& events)
{
    std::size_t count = 0;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::data && event.direction == data_direction::tx)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("replay: records are written at the times they were recorded")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "", "a"),
        record(100, data_direction::rx, "", "b"),
        record(250, data_direction::rx, "", "c"),
    };
    const auto origin = clock_type::now();
    auto player = replay_player::start(*hub, device.endpoint, capture);
    REQUIRE(player);

    REQUIRE(wait_until([&] { return (*player)->finished(); }, 3000ms));
    REQUIRE(wait_until([&] { return write_count(events) == 3; }, 2000ms));
    REQUIRE_EQ((*player)->records_sent(), std::uint64_t{3});
    REQUIRE_EQ((*player)->bytes_sent(), std::uint64_t{3});
    REQUIRE_EQ((*player)->passes(), std::uint64_t{1});
    REQUIRE_EQ(device.written(), std::string("abc"));

    // Each write is at its time, give or take the margin a loaded machine
    // needs, and never early.
    const auto times = write_times(events, "device", origin);
    REQUIRE_EQ(times.size(), std::size_t{3});
    REQUIRE(times[0] >= -2);
    REQUIRE(times[1] >= 98);
    REQUIRE(times[2] >= 248);
    REQUIRE(times[1] - times[0] <= 160);
    REQUIRE(times[2] - times[1] <= 210);
}

TEST_CASE("replay: speed scales the gaps")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "", "a"),
        record(400, data_direction::rx, "", "b"),
    };
    replay_options options;
    options.speed = 4.0;
    const auto origin = clock_type::now();
    auto player = replay_player::start(*hub, device.endpoint, capture, options);
    REQUIRE(player);
    REQUIRE(wait_until([&] { return write_count(events) == 2; }, 3000ms));

    const auto times = write_times(events, "device", origin);
    REQUIRE_EQ(times.size(), std::size_t{2});
    // 400 ms at four times the speed is 100 ms.
    REQUIRE(times[1] - times[0] >= 95);
    REQUIRE(times[1] - times[0] <= 250);
}

TEST_CASE("replay: one direction and one recorded port are chosen")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");

    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "master", "poll1;"),
        record(10, data_direction::tx, "master", "answer1;"),
        record(20, data_direction::rx, "other", "noise;"),
        record(30, data_direction::rx, "master", "poll2;"),
    };

    replay_options polls;
    polls.from_port = "master";
    auto first = replay_player::start(*hub, device.endpoint, capture, polls);
    REQUIRE(first);
    REQUIRE_EQ((*first)->records_per_pass(), std::size_t{2});
    REQUIRE(wait_until([&] { return (*first)->finished(); }, 2000ms));
    REQUIRE(wait_until([&] { return device.written() == "poll1;poll2;"; }, 2000ms));

    replay_options answers;
    answers.direction = data_direction::tx;
    auto second = replay_player::start(*hub, device.endpoint, capture, answers);
    REQUIRE(second);
    REQUIRE_EQ((*second)->records_per_pass(), std::size_t{1});
    REQUIRE(wait_until([&] { return (*second)->finished(); }, 2000ms));
    REQUIRE(wait_until([&] { return device.written() == "poll1;poll2;answer1;"; },
                       2000ms));
}

TEST_CASE("replay: a loop starts again after the length of the capture")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    event_recorder events;
    hub->subscribe({}, [events](const endpoint_event& e) { events.record(e); });

    // Two records, and a capture that lasted 100 ms in all.
    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "", "a"),
        record(100, data_direction::rx, "", "b"),
    };
    replay_options options;
    options.loop = true;
    const auto origin = clock_type::now();
    auto player = replay_player::start(*hub, device.endpoint, capture, options);
    REQUIRE(player);

    REQUIRE(wait_until([&] { return write_count(events) >= 6; }, 3000ms));
    (*player)->stop();
    REQUIRE(!(*player)->finished());
    REQUIRE(wait_until([&] { return (*player)->passes() >= 2; }, 1000ms));

    // a at 0, b at 100, a at 100, b at 200, a at 200, b at 300: one pass lasts
    // as long as the capture, so every 100 ms there is a write.
    const auto times = write_times(events, "device", origin);
    REQUIRE(times.size() >= std::size_t{6});
    REQUIRE(times[5] >= 295);

    // Stopped means stopped.  Two writes fall on the 300 ms mark (the end of
    // one pass writes b while the next pass writes a), so a second one can be
    // on its way when the first is seen.  Wait until every write the player
    // made has been recorded and the count has held still, then look for more.
    REQUIRE(wait_until(
        [&]
        {
            const auto sent = (*player)->records_sent();
            const auto recorded = write_count(events);
            std::this_thread::sleep_for(50ms);
            return recorded == sent && write_count(events) == sent &&
                   (*player)->records_sent() == sent;
        },
        3000ms));
    const auto before = write_count(events);
    const auto sent_before = (*player)->records_sent();
    std::this_thread::sleep_for(250ms);
    REQUIRE_EQ(write_count(events), before);
    REQUIRE_EQ((*player)->records_sent(), sent_before);
}

TEST_CASE("replay: nothing to send, a bad speed and no port are refused")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");
    const std::vector<capture_record> capture{
        record(0, data_direction::tx, "", "w")};

    // The only record is a W, and R is asked for.
    REQUIRE(!replay_player::start(*hub, device.endpoint, capture));

    replay_options slow;
    slow.speed = 0.0;
    slow.direction = data_direction::tx;
    REQUIRE(!replay_player::start(*hub, device.endpoint, capture, slow));
    slow.speed = -1.0;
    REQUIRE(!replay_player::start(*hub, device.endpoint, capture, slow));

    replay_options fine;
    fine.direction = data_direction::tx;
    REQUIRE(!replay_player::start(*hub, nullptr, capture, fine));
    REQUIRE(replay_player::start(*hub, device.endpoint, capture, fine));
}

TEST_CASE("replay: capture, replay, capture again and compare")
{
    // -- first run: a master sends three messages and two devices listen.
    std::vector<capture_record> first;
    {
        auto hub = start_hub();
        auto master = add_mock(*hub, "master");
        auto one = add_mock(*hub, "device-1");
        auto two = add_mock(*hub, "device-2");
        REQUIRE(hub->split(master.endpoint->id(),
                           {one.endpoint->id(), two.endpoint->id()}));

        auto out = std::make_shared<std::ostringstream>();
        capture_options options;
        options.format = capture_format::jsonl;
        options.ports = {"master"};
        auto session = capture_session::start(*hub, out, options);

        master.inject_read(std::string("\x10\x02\x50\x14", 4));
        std::this_thread::sleep_for(120ms);
        master.inject_read(std::string("\x10\x02\x51\x00\x7f", 5));
        std::this_thread::sleep_for(180ms);
        master.inject_read(std::string("\xff\x00", 2));
        REQUIRE(wait_until([&] { return session->records() >= 3; }, 2000ms));
        session->stop();

        std::istringstream in(out->str());
        const auto read = read_capture(in);
        REQUIRE(read);
        first = read->records;
        REQUIRE_EQ(first.size(), std::size_t{3});
    }

    // -- replay it into a loopback port, whose writes come back in as bytes the
    // port received, and capture that.
    auto hub = start_hub();
    auto port = add_loopback(*hub, "master");
    auto listener = add_mock(*hub, "listener");
    REQUIRE(hub->redirect(port->id(), listener.endpoint->id()));

    auto out = std::make_shared<std::ostringstream>();
    capture_options options;
    options.format = capture_format::jsonl;
    options.ports = {"master"};
    auto session = capture_session::start(*hub, out, options);

    replay_options replay;
    replay.from_port = "master";
    auto player = replay_player::start(*hub, port, first, replay);
    REQUIRE(player);
    REQUIRE(wait_until([&] { return (*player)->finished(); }, 3000ms));
    // Three writes, three bytes read back.
    REQUIRE(wait_until([&] { return session->records() >= 6; }, 2000ms));
    session->stop();

    std::istringstream in(out->str());
    const auto second = read_capture(in);
    REQUIRE(second);

    std::vector<capture_record> received;
    std::vector<capture_record> written;
    for (const auto& item : second->records)
        (item.direction == data_direction::rx ? received : written).push_back(item);

    // The same bytes in the same order, both as written and as read back, and
    // the gaps are those of the first run.
    REQUIRE_EQ(received.size(), first.size());
    REQUIRE_EQ(written.size(), first.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        REQUIRE(received[i].data == first[i].data);
        REQUIRE(written[i].data == first[i].data);
        const auto expected = first[i].offset_ms - first[0].offset_ms;
        const auto got = received[i].offset_ms - received[0].offset_ms;
        REQUIRE(got >= expected - 5);
        REQUIRE(got <= expected + 80);
    }
}

TEST_CASE("replay: bytes written and bytes failed are counted when the write ends")
{
    auto hub = start_hub();
    boost::system::error_code ec;
    auto transport = serial_transport::make_mock(hub->executor(),
                                                 serial_settings{}, "small", ec);
    REQUIRE(!ec);
    // A queue of four bytes takes the first record and refuses the second.
    endpoint_options options = test_options();
    options.max_write_queue_bytes = 4;
    auto added = hub->add_endpoint("small", endpoint_kind::mock, transport, options);
    REQUIRE(added);

    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "", "abc"),
        record(0, data_direction::rx, "", "defgh"),
    };
    auto player = replay_player::start(*hub, *added, capture);
    REQUIRE(player);

    REQUIRE(wait_until(
        [&] { return (*player)->bytes_written() + (*player)->bytes_failed() ==
                     (*player)->bytes_sent() && (*player)->finished(); },
        3000ms));
    REQUIRE_EQ((*player)->bytes_sent(), std::uint64_t{8});
    REQUIRE_EQ((*player)->bytes_written(), std::uint64_t{3});
    REQUIRE_EQ((*player)->bytes_failed(), std::uint64_t{5});
    REQUIRE_EQ((*player)->write_errors(), std::uint64_t{1});
}

TEST_CASE("replay: what another writer puts on the endpoint is not counted")
{
    auto hub = start_hub();
    auto device = add_mock(*hub, "device");

    const std::vector<capture_record> capture{
        record(0, data_direction::rx, "", "aa"),
        record(150, data_direction::rx, "", "bb"),
    };
    auto player = replay_player::start(*hub, device.endpoint, capture);
    REQUIRE(player);

    // A link or a program writing to the same port: ten bytes that are not
    // the player's.
    device.endpoint->write(to_bytes("0123456789"));

    REQUIRE(wait_until([&] { return (*player)->finished(); }, 3000ms));
    REQUIRE(wait_until(
        [&] { return (*player)->bytes_written() == (*player)->bytes_sent(); },
        3000ms));
    REQUIRE_EQ((*player)->bytes_sent(), std::uint64_t{4});
    REQUIRE_EQ((*player)->bytes_written(), std::uint64_t{4});
    REQUIRE_EQ((*player)->bytes_failed(), std::uint64_t{0});
    REQUIRE(wait_until([&] { return device.written().size() == 14; }, 2000ms));
}
