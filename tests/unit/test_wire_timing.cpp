//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wire time and pacing (docs/wire-timing-plan.md).  The model has no clock, so
// its cases write every time down; the hub cases use a real clock and check
// only lower bounds, which hold however slow the machine is.
//

#include "support/test_support.hpp"

#include <vss/core/bus_line.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/line_break.hpp>
#include <vss/core/pacing.hpp>
#include <vss/core/wire_pacer.hpp>
#include <vss/core/wire_time.hpp>
#include <vss/ports/loopback_transport.hpp>
#include <vss/testkit/harness.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

constexpr time_point origin = time_point{} + 1h;

time_point at(std::chrono::nanoseconds by)
{
    return origin + by;
}

wire_rate rate(std::uint32_t bit_rate, wire_framing framing = {})
{
    return wire_rate{bit_rate, framing};
}

} // namespace

TEST_CASE("wire time: a character takes its real bits")
{
    // 1000 bit/s makes a bit a millisecond.
    REQUIRE_EQ(wire_time(1000, wire_framing{}, 1), clock_type::duration(10ms));
    REQUIRE_EQ(wire_time(1000, {8, parity_mode::even, stop_bits_mode::one}, 1),
               clock_type::duration(11ms));
    REQUIRE_EQ(wire_time(1000, {8, parity_mode::none, stop_bits_mode::two}, 1),
               clock_type::duration(11ms));
    REQUIRE_EQ(wire_time(1000, {7, parity_mode::odd, stop_bits_mode::two}, 1),
               clock_type::duration(11ms));
    REQUIRE_EQ(wire_time(1000,
                         {5, parity_mode::none, stop_bits_mode::one_point_five},
                         1),
               clock_type::duration(7500us));
    REQUIRE_EQ(wire_time(1000, {8, parity_mode::mark, stop_bits_mode::two}, 4),
               clock_type::duration(48ms));
    REQUIRE_EQ(wire_time(9600, wire_framing{}, 96),
               clock_type::duration(100ms));
    REQUIRE_EQ(wire_time(0, wire_framing{}, 5), clock_type::duration(0));
    REQUIRE_EQ(wire_time(1000, wire_framing{}, 0), clock_type::duration(0));
}

TEST_CASE("wire time: framing text reads and writes")
{
    REQUIRE_EQ(to_text(wire_framing{}), std::string("8N1"));
    REQUIRE_EQ(to_text({7, parity_mode::even, stop_bits_mode::two}),
               std::string("7E2"));
    REQUIRE_EQ(to_text({5, parity_mode::space, stop_bits_mode::one_point_five}),
               std::string("5S1.5"));
    for (const char* text : {"8N1", "7E2", "5N1.5", "6O1", "8M2", "8S1"})
    {
        const auto framing = parse_framing(text);
        REQUIRE(framing.has_value());
        REQUIRE_EQ(to_text(*framing), std::string(text));
    }
    REQUIRE(parse_framing("8e1").has_value());
    for (const char* text :
         {"", "8", "8N", "9N1", "4N1", "8X1", "8N3", "8N1.6", "8N1 ", "NN1"})
        REQUIRE(!parse_framing(text).has_value());
}

TEST_CASE("wire time: the bus counts the framing it is given")
{
    REQUIRE_EQ(bus_line::wire_time(1000, 1), clock_type::duration(10ms));
    REQUIRE_EQ(bus_line::wire_time(1000, 10,
                                   {8, parity_mode::even, stop_bits_mode::one}),
               clock_type::duration(110ms));
}

TEST_CASE("pacer: an item waits for the one before it to finish on the wire")
{
    wire_pacer pacer;
    // Three bytes at 1000 bit/s take 30 ms, and the accuracy is far
    // longer than a character so it is one run.
    const auto first = pacer.plan_bytes(at(0ms), 3, rate(1000), 100s);
    REQUIRE_EQ(first.size(), std::size_t{1});
    REQUIRE(first[0].release == at(0ms));
    REQUIRE_EQ(first[0].length, std::size_t{3});
    REQUIRE(pacer.free_at() == at(30ms));

    // Due at 10 ms, it waits for 30.
    const auto second = pacer.plan_bytes(at(10ms), 1, rate(1000), 100s);
    REQUIRE(second[0].release == at(30ms));
    REQUIRE(pacer.free_at() == at(40ms));

    // Due after the wire is free, it goes when it is due.
    const auto third = pacer.plan_bytes(at(100ms), 1, rate(1000), 100s);
    REQUIRE(third[0].release == at(100ms));
    REQUIRE(pacer.free_at() == at(110ms));
}

TEST_CASE("pacer: the delay and the jitter are in the due time and are kept")
{
    wire_pacer pacer;
    // A later due time than the wire needs is not pulled back.
    pacer.plan_bytes(at(5ms), 1, rate(1000), 100s);
    const auto next = pacer.plan_bytes(at(80ms), 1, rate(1000), 100s);
    REQUIRE(next[0].release == at(80ms));
}

TEST_CASE("pacer: a long chunk is split at character boundaries")
{
    wire_pacer pacer;
    // Five bytes at 1000 bit/s; accuracy under a character, so one byte
    // to a run, at the time the character begins.
    const auto runs = pacer.plan_bytes(at(0ms), 5, rate(1000), 1ms);
    REQUIRE_EQ(runs.size(), std::size_t{5});
    for (std::size_t index = 0; index < runs.size(); ++index)
    {
        REQUIRE_EQ(runs[index].offset, index);
        REQUIRE_EQ(runs[index].length, std::size_t{1});
        REQUIRE(runs[index].release ==
                at(std::chrono::milliseconds(10 * index)));
    }
    REQUIRE(pacer.free_at() == at(50ms));
}

TEST_CASE("pacer: runs take the larger of the accuracy and one character")
{
    // A DMX512 frame, 513 slots of 8N2 at 250000 bit/s: 44 us a slot, a
    // little over 22 ms.  With 500 us accuracy a run holds 11 slots.
    wire_pacer pacer;
    const wire_framing dmx{8, parity_mode::none, stop_bits_mode::two};
    const auto runs = pacer.plan_bytes(at(0ms), 513, rate(250000, dmx), 500us);
    REQUIRE_EQ(runs.size(), std::size_t{47});
    std::size_t total = 0;
    for (const auto& run : runs)
    {
        REQUIRE_EQ(run.offset, total);
        REQUIRE(run.length <= 11);
        total += run.length;
    }
    REQUIRE_EQ(total, std::size_t{513});
    REQUIRE(pacer.free_at() == at(0ms) + wire_time(250000, dmx, 513));

    // At 115200 baud, a character is 86 us, so 5 to a run.
    wire_pacer other;
    REQUIRE_EQ(other.plan_bytes(at(0ms), 512, rate(115200), 500us).size(),
               std::size_t{103});

    // A Modbus frame of 8 bytes at 115200 is under 700 us: one run.
    wire_pacer modbus;
    REQUIRE_EQ(modbus.plan_bytes(at(0ms), 8, rate(115200), 1ms).size(),
               std::size_t{1});
    // The silence after it counts from the frame's own wire time, and 3.5
    // characters of it are 304 us.
    const auto reply = modbus.plan_bytes(at(0ms), 1, rate(115200), 1ms);
    REQUIRE(reply[0].release == at(0ms) + wire_time(115200, {}, 8));
}

TEST_CASE("pacer: a break holds the wire for its length")
{
    wire_pacer pacer;
    // A complete break of 12 ms (measured), then a byte due at once.
    REQUIRE(pacer.plan_break(at(0ms), true, true, 12ms, rate(1000)) == at(0ms));
    REQUIRE(pacer.free_at() == at(12ms));
    const auto after = pacer.plan_bytes(at(0ms), 1, rate(1000), 100s);
    REQUIRE(after[0].release == at(12ms));

    // A byte before the break is released first, and holds the break.
    wire_pacer ordered;
    ordered.plan_bytes(at(0ms), 2, rate(1000), 100s);
    REQUIRE(ordered.plan_break(at(0ms), true, true, 5ms, rate(1000)) ==
            at(20ms));
    REQUIRE(ordered.free_at() == at(25ms));
}

TEST_CASE("pacer: a break that was not measured is one character long")
{
    wire_pacer pacer;
    pacer.plan_break(at(0ms), true, false, 3ms, rate(1000));
    REQUIRE(pacer.free_at() == at(10ms));

    wire_pacer framed;
    framed.plan_break(at(0ms), true, false, 0ms,
                      rate(1000, {8, parity_mode::even, stop_bits_mode::two}));
    REQUIRE(framed.free_at() == at(12ms));
}

TEST_CASE("pacer: the begin of a break holds nothing")
{
    wire_pacer pacer;
    REQUIRE(pacer.plan_break(at(0ms), false, true, 0ms, rate(1000)) == at(0ms));
    REQUIRE(pacer.free_at() == at(0ms));
    // The end carries the length.
    pacer.plan_break(at(30ms), true, true, 30ms, rate(1000));
    REQUIRE(pacer.free_at() == at(60ms));
}

TEST_CASE("pacer: without a rate nothing is held")
{
    wire_pacer pacer;
    REQUIRE(pacer.plan_unpaced(at(7ms)) == at(7ms));
    REQUIRE(pacer.free_at() == at(7ms));
}

TEST_CASE("spin budget: it fills at a tenth of a second a second")
{
    spin_budget budget(100ms);
    const auto start = std::chrono::steady_clock::now();
    // A full budget gives 100 ms at once, then nothing.
    REQUIRE(budget.take(60ms, start));
    REQUIRE(budget.take(40ms, start));
    REQUIRE(!budget.take(1ms, start));
    // A second later it has 100 ms again, and no more than that.
    REQUIRE(budget.take(100ms, start + 1s));
    REQUIRE(!budget.take(1ms, start + 1s));
    REQUIRE(!budget.take(101ms, start + 100s));
    // Half a second earns 50 ms.
    spin_budget slow(100ms);
    REQUIRE(slow.take(100ms, start));
    REQUIRE(!slow.take(60ms, start + 500ms));
    REQUIRE(slow.take(50ms, start + 1s));
    // What a release did not use goes back.
    slow.give_back(20ms);
    REQUIRE(slow.take(20ms, start + 1s));
}

namespace
{

/// A port whose reads are in the test's hands.
struct source_port
{
    std::shared_ptr<vss::endpoint> endpoint;
    std::shared_ptr<loopback_transport> transport;

    void bytes(const std::string& text) const
    {
        transport->inject(bytes_view(reinterpret_cast<const byte*>(text.data()),
                                     text.size()));
    }

    void make_break(std::chrono::nanoseconds length) const
    {
        line_break item;
        item.phase = break_phase::complete;
        item.start = clock_type::now();
        item.duration = length;
        item.measured = true;
        transport->inject_break(item);
    }
};

source_port add_source(vss::hub& hub, const std::string& name)
{
    auto transport = loopback_transport::create(hub.executor(), name);
    auto added = hub.add_endpoint(name, endpoint_kind::virtual_port, transport);
    REQUIRE(added);
    return source_port{*added, std::move(transport)};
}

link_options paced(std::uint32_t bit_rate = 1000)
{
    link_options options;
    options.wire_timing.enabled = true;
    options.wire_timing.bit_rate = bit_rate;
    return options;
}

/// What a target wrote out and when, one entry for each write.
struct arrival
{
    time_point when;
    std::string what;
};

struct watch
{
    event_recorder events;
    subscription_guard guard;

    explicit watch(vss::hub& hub)
        : guard(hub, event_filter{},
                [recorder = events](const endpoint_event& event)
                { recorder.record(event); })
    {
    }

    std::vector<arrival> written_by(endpoint_id id) const
    {
        std::vector<arrival> out;
        for (const auto& event : events.snapshot())
        {
            if (event.endpoint != id || event.direction != data_direction::tx)
                continue;
            if (event.kind == event_kind::data && event.data)
                out.push_back({event.when, to_string(bytes_view(*event.data))});
            else if (event.kind == event_kind::line_break)
                out.push_back({event.when, "[break]"});
        }
        return out;
    }

    std::string text_by(endpoint_id id) const
    {
        std::string out;
        for (const auto& item : written_by(id))
            out += item.what;
        return out;
    }

    std::size_t notes_containing(const std::string& text) const
    {
        std::size_t count = 0;
        for (const auto& event : events.snapshot())
        {
            if (event.kind == event_kind::log_message &&
                event.message.find(text) != std::string::npos)
                ++count;
        }
        return count;
    }
};

} // namespace

TEST_CASE("wire timing: a long write arrives over the time a wire takes")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    const auto link =
        hub->redirect(from.endpoint->id(), to.endpoint->id(), paced(1000));
    REQUIRE(link);

    // Five bytes at 10 ms each, and a sixth written at once after them.
    from.bytes("ABCDE");
    from.bytes("F");
    REQUIRE(wait_until(
        [&] { return seen.text_by(to.endpoint->id()) == "ABCDEF"; }, 5000ms));
    const auto arrivals = seen.written_by(to.endpoint->id());
    REQUIRE(arrivals.size() >= 2);
    // The sixth left the wire no sooner than 50 ms after the first began.
    REQUIRE(arrivals.back().when - arrivals.front().when >= 35ms);

    const auto info = hub->find_link(*link);
    REQUIRE(info);
    REQUIRE(info->stats.paced_releases >= 2);
    REQUIRE(info->options.wire_timing.enabled);
}

TEST_CASE("wire timing: a break holds the bytes after it for its length")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    REQUIRE(hub->redirect(from.endpoint->id(), to.endpoint->id(), paced(1000)));

    from.bytes("A");
    from.make_break(60ms);
    from.bytes("B");
    REQUIRE(wait_until(
        [&] { return seen.text_by(to.endpoint->id()) == "A[break]B"; },
        5000ms));
    const auto arrivals = seen.written_by(to.endpoint->id());
    // The byte took 10 ms, the break 60: B is no sooner than 70 ms after A.
    const auto a = arrivals.front().when;
    const auto b = arrivals.back().when;
    REQUIRE(b - a >= 50ms);
}

TEST_CASE("wire timing: the format the sender sets is used before the option")
{
    // A loopback port reports no peer format, so the option is what is
    // used, and a faster option is shorter: 100000 bit/s takes 100 us.
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    REQUIRE(hub->redirect(from.endpoint->id(), to.endpoint->id(),
                          paced(1'000'000)));
    from.bytes("ABCDEFGH");
    REQUIRE(wait_until(
        [&] { return seen.text_by(to.endpoint->id()) == "ABCDEFGH"; }, 5000ms));
}

TEST_CASE("wire timing: with no rate the link is not paced and says so once")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    link_options options;
    options.wire_timing.enabled = true;
    const auto link =
        hub->redirect(from.endpoint->id(), to.endpoint->id(), options);
    REQUIRE(link);

    from.bytes("one");
    from.bytes("two");
    REQUIRE(wait_until(
        [&] { return seen.text_by(to.endpoint->id()) == "onetwo"; }, 5000ms));
    REQUIRE_EQ(seen.notes_containing("no rate for this link"), std::size_t{1});
    const auto info = hub->find_link(*link);
    REQUIRE(info);
    REQUIRE_EQ(info->stats.paced_releases, std::uint64_t{0});
}

TEST_CASE("wire timing: a link made without it is not paced")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    const auto link = hub->redirect(from.endpoint->id(), to.endpoint->id());
    REQUIRE(link);
    from.bytes("AB");
    REQUIRE(wait_until([&] { return seen.text_by(to.endpoint->id()) == "AB"; },
                       5000ms));
    const auto info = hub->find_link(*link);
    REQUIRE(info);
    REQUIRE(!info->options.wire_timing.enabled);
    REQUIRE_EQ(info->stats.paced_releases, std::uint64_t{0});
}

TEST_CASE("wire timing: it can be turned on and off on a running link")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    const auto link = hub->redirect(from.endpoint->id(), to.endpoint->id());
    REQUIRE(link);

    REQUIRE(hub->set_link_options(*link, paced(1'000'000)));
    from.bytes("A");
    REQUIRE(wait_until([&] { return seen.text_by(to.endpoint->id()) == "A"; },
                       5000ms));
    REQUIRE(wait_until(
        [&] { return hub->find_link(*link)->stats.paced_releases >= 1; },
        5000ms));

    link_options off;
    off.emulate_modem_lines = false;
    REQUIRE(hub->set_link_options(*link, off));
    from.bytes("B");
    REQUIRE(wait_until([&] { return seen.text_by(to.endpoint->id()) == "AB"; },
                       5000ms));
    REQUIRE(!hub->find_link(*link)->options.wire_timing.enabled);
}

TEST_CASE("wire timing: values out of range and a bus are refused")
{
    auto hub = start_hub();
    const auto a = add_mock(*hub, "a");
    const auto b = add_mock(*hub, "b");

    link_options slow = paced(10);
    REQUIRE_EQ(hub->redirect(a.endpoint->id(), b.endpoint->id(), slow).error(),
               make_error_code(errc::invalid_argument));
    link_options fast = paced(12'000'001);
    REQUIRE_EQ(hub->redirect(a.endpoint->id(), b.endpoint->id(), fast).error(),
               make_error_code(errc::invalid_argument));
    link_options framed = paced(9600);
    framed.wire_timing.framing =
        wire_framing{9, parity_mode::none, stop_bits_mode::one};
    REQUIRE_EQ(
        hub->redirect(a.endpoint->id(), b.endpoint->id(), framed).error(),
        make_error_code(errc::invalid_argument));

    // A bus has a wire of its own.
    REQUIRE_EQ(
        hub->bus({a.endpoint->id(), b.endpoint->id()}, paced(9600)).error(),
        make_error_code(errc::unsupported));
    const auto bus = hub->bus({a.endpoint->id(), b.endpoint->id()});
    REQUIRE(bus);
    REQUIRE_EQ(hub->set_link_options(*bus, paced(9600)).error(),
               make_error_code(errc::unsupported));
}
