//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The model of a half-duplex line, on its own.  It has no clock: every time
// is written here, so each case says exactly what overlaps and what does
// not.  The line runs at 1000 bit/s, where one byte takes ten milliseconds.
//

#include "test_framework.hpp"

#include <vss/core/bus_line.hpp>

#include <chrono>
#include <cstdint>

using namespace vss;

namespace
{
using namespace std::chrono_literals;

constexpr endpoint_id a = 1;
constexpr endpoint_id b = 2;
constexpr endpoint_id c = 3;

half_duplex_options
line_options(std::uint32_t turnaround_ms = 0,
             collision_action on_collision = collision_action::report,
             bool rts_gating = false)
{
    half_duplex_options options;
    options.enabled = true;
    options.bit_rate = 1000;
    options.turnaround_ms = turnaround_ms;
    options.on_collision = on_collision;
    options.rts_gating = rts_gating;
    return options;
}

/// A time so many milliseconds after the line was made.
time_point at(std::int64_t milliseconds)
{
    return time_point{} + 1h + std::chrono::milliseconds(milliseconds);
}

bool garbled(const bus_verdict& verdict)
{
    return verdict.transmission && verdict.transmission->garble.load();
}

} // namespace

TEST_CASE("bus line: a byte takes ten bits at the line rate")
{
    REQUIRE(bus_line::wire_time(1000, 1) == 10ms);
    REQUIRE(bus_line::wire_time(1000, 25) == 250ms);
    REQUIRE(bus_line::wire_time(9600, 96) == 100ms);
    REQUIRE(bus_line::wire_time(4'000'000, 1) == 2500ns);
    REQUIRE(bus_line::wire_time(50, 65536) == 13107200ms);
    REQUIRE(bus_line::wire_time(1000, 0) == 0ms);
}

TEST_CASE("bus line: one member after another is clean")
{
    bus_line line(line_options());

    const auto poll = line.transmit(a, 10, at(0));
    REQUIRE(poll.fault == bus_fault::none);
    REQUIRE(poll.carried);
    REQUIRE(poll.ends == at(100));
    REQUIRE(poll.transmission != nullptr);

    // The answer starts at the very moment the poll's last byte has gone.
    const auto answer = line.transmit(b, 5, at(100));
    REQUIRE(answer.fault == bus_fault::none);
    REQUIRE(answer.ends == at(150));

    const auto later = line.transmit(c, 1, at(500));
    REQUIRE(later.fault == bus_fault::none);
    REQUIRE(later.ends == at(510));

    REQUIRE_EQ(line.collisions(), std::uint64_t{0});
    REQUIRE(!garbled(poll));
    REQUIRE(!garbled(answer));
}

TEST_CASE("bus line: two members at once collide, and it is only reported")
{
    bus_line line(line_options());

    const auto first = line.transmit(a, 10, at(0));
    const auto second = line.transmit(b, 2, at(50));
    REQUIRE(first.fault == bus_fault::none);
    REQUIRE(second.fault == bus_fault::overlap);
    REQUIRE_EQ(second.other, a);
    // The second is carried all the same, for its own time on the wire.
    REQUIRE(second.carried);
    REQUIRE(second.ends == at(70));

    REQUIRE_EQ(line.collisions(), std::uint64_t{1});
    REQUIRE(!garbled(first));
    REQUIRE(!garbled(second));
}

TEST_CASE("bus line: with garble both chunks that collided are spoilt")
{
    bus_line line(line_options(0, collision_action::garble));

    const auto first = line.transmit(a, 10, at(0));
    REQUIRE(!garbled(first));
    const auto second = line.transmit(b, 2, at(50));
    REQUIRE(second.fault == bus_fault::overlap);
    // The first was judged clean when it started; the second ran into it.
    REQUIRE(garbled(first));
    REQUIRE(garbled(second));

    // A chunk after both have gone is whole again.
    const auto third = line.transmit(c, 1, at(100));
    REQUIRE(third.fault == bus_fault::none);
    REQUIRE(!garbled(third));
    REQUIRE_EQ(line.collisions(), std::uint64_t{1});
}

TEST_CASE("bus line: the last moment of a chunk is still on the wire")
{
    {
        bus_line line(line_options());
        static_cast<void>(line.transmit(a, 10, at(0)));
        const auto late = line.transmit(b, 1, at(100) - 1ns);
        REQUIRE(late.fault == bus_fault::overlap);
    }
    {
        bus_line line(line_options());
        static_cast<void>(line.transmit(a, 10, at(0)));
        const auto after = line.transmit(b, 1, at(100));
        REQUIRE(after.fault == bus_fault::none);
    }
}

TEST_CASE("bus line: a member's own chunks follow each other")
{
    bus_line line(line_options(0, collision_action::garble));

    // Three writes in a burst leave one after another, without a fault.
    const auto one = line.transmit(a, 10, at(0));
    const auto two = line.transmit(a, 10, at(1));
    const auto three = line.transmit(a, 5, at(2));
    REQUIRE(one.fault == bus_fault::none);
    REQUIRE(two.fault == bus_fault::none);
    REQUIRE(three.fault == bus_fault::none);
    REQUIRE(one.ends == at(100));
    REQUIRE(two.ends == at(200));
    REQUIRE(three.ends == at(250));

    // Another member that starts during the second spoils that one only:
    // the first had left the wire and the third had not begun.
    const auto other = line.transmit(b, 2, at(150));
    REQUIRE(other.fault == bus_fault::overlap);
    REQUIRE_EQ(other.other, a);
    REQUIRE(!garbled(one));
    REQUIRE(garbled(two));
    REQUIRE(!garbled(three));
    REQUIRE(garbled(other));
}

TEST_CASE("bus line: a long chunk spoils every chunk it covers")
{
    bus_line line(line_options(0, collision_action::garble));

    const auto one = line.transmit(a, 10, at(0));   // 0 to 100
    const auto two = line.transmit(a, 10, at(0));   // 100 to 200
    const auto three = line.transmit(a, 10, at(0)); // 200 to 300
    const auto wide = line.transmit(b, 12, at(90)); // 90 to 210
    REQUIRE(wide.fault == bus_fault::overlap);
    REQUIRE(garbled(one));
    REQUIRE(garbled(two));
    REQUIRE(garbled(three));
    REQUIRE_EQ(line.collisions(), std::uint64_t{1});
}

TEST_CASE("bus line: an answer inside the turn-around has come too soon")
{
    bus_line line(line_options(5, collision_action::garble));

    const auto poll = line.transmit(a, 10, at(0));
    const auto hasty = line.transmit(b, 1, at(100));
    REQUIRE(hasty.fault == bus_fault::too_soon);
    REQUIRE_EQ(hasty.other, a);
    REQUIRE(hasty.carried);
    // The poll had left the wire whole; only the hasty answer is spoilt.
    REQUIRE(!garbled(poll));
    REQUIRE(garbled(hasty));
    REQUIRE_EQ(line.collisions(), std::uint64_t{1});
}

TEST_CASE("bus line: the turn-around ends exactly when it says")
{
    {
        bus_line line(line_options(5));
        static_cast<void>(line.transmit(a, 10, at(0)));
        REQUIRE(line.transmit(b, 1, at(105) - 1ns).fault ==
                bus_fault::too_soon);
    }
    {
        bus_line line(line_options(5));
        static_cast<void>(line.transmit(a, 10, at(0)));
        REQUIRE(line.transmit(b, 1, at(105)).fault == bus_fault::none);
    }
    {
        // Without a turn-around there is nothing to come too soon for.
        bus_line line(line_options(0));
        static_cast<void>(line.transmit(a, 10, at(0)));
        REQUIRE(line.transmit(b, 1, at(100)).fault == bus_fault::none);
    }
}

TEST_CASE("bus line: a member need not wait for its own turn-around")
{
    bus_line line(line_options(50));
    static_cast<void>(line.transmit(a, 10, at(0)));
    REQUIRE(line.transmit(a, 1, at(101)).fault == bus_fault::none);
    // The turn-around now runs from the end of that second chunk.
    REQUIRE(line.transmit(b, 1, at(160)).fault == bus_fault::too_soon);
    REQUIRE(line.transmit(c, 1, at(500)).fault == bus_fault::none);
}

TEST_CASE("bus line: an overlap is named before a turn-around")
{
    bus_line line(line_options(50));
    static_cast<void>(line.transmit(a, 1, at(0)));   // 0 to 10
    static_cast<void>(line.transmit(b, 10, at(20))); // too soon, 20 to 120
    // c is inside a's turn-around and on top of b: the overlap is the news.
    const auto third = line.transmit(c, 1, at(30));
    REQUIRE(third.fault == bus_fault::overlap);
    REQUIRE_EQ(third.other, b);
    REQUIRE_EQ(line.collisions(), std::uint64_t{2});
}

TEST_CASE("bus line: with RTS gating a low RTS switches the transmitter off")
{
    bus_line line(line_options(0, collision_action::report, true));
    line.set_rts(a, false);

    const auto muted = line.transmit(a, 4, at(0));
    REQUIRE(muted.fault == bus_fault::transmitter_off);
    REQUIRE(!muted.carried);
    REQUIRE(muted.transmission == nullptr);
    // It never took the wire, so another member is clean straight away.
    REQUIRE(line.transmit(b, 4, at(1)).fault == bus_fault::none);
    // Nothing collided: the bytes were dropped, which the hub counts.
    REQUIRE_EQ(line.collisions(), std::uint64_t{0});

    line.set_rts(a, true);
    const auto sent = line.transmit(a, 4, at(100));
    REQUIRE(sent.fault == bus_fault::none);
    REQUIRE(sent.carried);
}

TEST_CASE("bus line: a member without an RTS is never switched off")
{
    bus_line line(line_options(0, collision_action::report, true));
    // Nothing was ever said of b's RTS, and c's was withdrawn.
    line.set_rts(c, false);
    line.set_rts(c, std::nullopt);

    REQUIRE(line.transmit(b, 1, at(0)).fault == bus_fault::none);
    REQUIRE(line.transmit(c, 1, at(100)).fault == bus_fault::none);
}

TEST_CASE("bus line: a member that holds RTS high holds the line")
{
    bus_line line(line_options(0, collision_action::garble, true));
    line.set_rts(a, true);

    // a sends nothing, but its transmitter is on.
    const auto blocked = line.transmit(b, 2, at(0));
    REQUIRE(blocked.fault == bus_fault::line_held);
    REQUIRE_EQ(blocked.other, a);
    REQUIRE(blocked.carried);
    REQUIRE(garbled(blocked));
    REQUIRE_EQ(line.collisions(), std::uint64_t{1});

    // a itself sends cleanly while it holds the line.
    REQUIRE(line.transmit(a, 1, at(100)).fault == bus_fault::none);

    line.set_rts(a, false);
    REQUIRE(line.transmit(b, 2, at(200)).fault == bus_fault::none);
}

TEST_CASE("bus line: without RTS gating the RTS means nothing")
{
    bus_line line(line_options());
    line.set_rts(a, false);
    line.set_rts(b, true);
    REQUIRE(line.transmit(a, 1, at(0)).fault == bus_fault::none);
    REQUIRE(line.transmit(c, 1, at(100)).fault == bus_fault::none);
}

TEST_CASE("bus line: a member that is removed lets go of the line")
{
    bus_line line(line_options(50, collision_action::garble, true));
    line.set_rts(a, true);
    const auto cut = line.transmit(a, 10, at(0));
    REQUIRE(cut.fault == bus_fault::none);

    line.remove(a);

    // Neither its bytes, its turn-around nor its RTS is in the way now.
    const auto next = line.transmit(b, 1, at(50));
    REQUIRE(next.fault == bus_fault::none);
    REQUIRE(!garbled(cut));
    REQUIRE_EQ(line.collisions(), std::uint64_t{0});
}

TEST_CASE("bus line: a member too far ahead of the line is dropped")
{
    half_duplex_options options = line_options();
    options.bit_rate = 50; // a byte takes 200 ms, 100 bytes 20 s
    bus_line line(options);

    // Four chunks start within a minute of being read: 0, 20, 40 and 60 s.
    for (int chunk = 0; chunk < 4; ++chunk)
    {
        const auto verdict = line.transmit(a, 100, at(0));
        REQUIRE(verdict.fault == bus_fault::none);
        REQUIRE(verdict.ends == at((chunk + 1) * 20'000));
    }
    // The fifth could not start for 80 s.
    const auto over = line.transmit(a, 100, at(0));
    REQUIRE(over.fault == bus_fault::overrun);
    REQUIRE(!over.carried);
    REQUIRE(over.transmission == nullptr);

    // Once the line has caught up there is room again.
    const auto later = line.transmit(a, 100, at(30'000));
    REQUIRE(later.fault == bus_fault::none);
    REQUIRE(later.ends == at(100'000));
}

TEST_CASE("bus line: what has long left the wire is forgotten")
{
    bus_line line(line_options(5));
    for (int round = 0; round < 1000; ++round)
    {
        const auto base = static_cast<std::int64_t>(round) * 1000;
        REQUIRE(line.transmit(a, 10, at(base)).fault == bus_fault::none);
        REQUIRE(line.transmit(b, 10, at(base + 500)).fault == bus_fault::none);
    }
    REQUIRE_EQ(line.collisions(), std::uint64_t{0});
}
