//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the helper puts the bytes a program wrote to a driver port back in order
// with its break calls and its write calls (libs/vss_ports/src/tx_order.hpp).
//

#include "tx_order.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace
{

using vss::detail::tx_break_mark;
using vss::detail::tx_order;
using vss::detail::tx_piece;
using vss::detail::tx_write_mark;
using kind = vss::detail::tx_piece::kind;

std::vector<std::uint8_t> bytes_of(const std::string& text)
{
    return {text.begin(), text.end()};
}

tx_break_mark held(std::uint64_t on_time, std::uint64_t on_offset)
{
    tx_break_mark mark;
    mark.on_time = on_time;
    mark.on_offset = on_offset;
    mark.open = true;
    return mark;
}

tx_break_mark ended(std::uint64_t on_time, std::uint64_t off_time,
                    std::uint64_t on_offset, std::uint64_t off_offset)
{
    tx_break_mark mark;
    mark.on_time = on_time;
    mark.off_time = off_time;
    mark.on_offset = on_offset;
    mark.off_offset = off_offset;
    return mark;
}

/// The pieces as text: bytes as themselves, a break as [begin], [end] or [break].
std::string show(const std::vector<tx_piece>& pieces)
{
    std::string out;
    for (const auto& piece : pieces)
    {
        switch (piece.what)
        {
        case kind::bytes:
            out.append(piece.bytes.begin(), piece.bytes.end());
            out += '|';
            break;
        case kind::break_begin: out += "[begin]"; break;
        case kind::break_end: out += "[end]"; break;
        case kind::break_complete: out += "[break]"; break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("tx order: bytes with no marks pass straight through")
{
    tx_order order;
    order.add_bytes(bytes_of("AB"));
    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("AB|"));
    REQUIRE(!out[0].stamp.has_value());
}

TEST_CASE("tx order: a break falls between the bytes written before and after it")
{
    tx_order order;
    const std::vector<tx_break_mark> breaks{ended(200, 5200, 2, 2)};
    order.add_marks(breaks, {});
    order.add_bytes(bytes_of("ABCD"));

    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("AB|[break]CD|"));
    REQUIRE_EQ(out[1].on_time, 200u);
    REQUIRE_EQ(out[1].off_time, 5200u);
}

TEST_CASE("tx order: a break at the start or the end of the bytes")
{
    tx_order order;
    const std::vector<tx_break_mark> breaks{ended(1, 2, 0, 0), ended(3, 4, 2, 2)};
    order.add_marks(breaks, {});
    order.add_bytes(bytes_of("AB"));
    REQUIRE_EQ(show(order.take()), std::string("[break]AB|[break]"));
}

TEST_CASE("tx order: a break waits for the bytes written before it")
{
    tx_order order;
    const std::vector<tx_break_mark> breaks{ended(1, 2, 4, 4)};
    order.add_marks(breaks, {});
    order.add_bytes(bytes_of("AB"));
    REQUIRE_EQ(show(order.take()), std::string("AB|"));

    order.add_bytes(bytes_of("CDEF"));
    REQUIRE_EQ(show(order.take()), std::string("CD|[break]EF|"));
}

TEST_CASE("tx order: a break with no bytes at all is still given")
{
    tx_order order;
    const std::vector<tx_break_mark> breaks{ended(1, 2, 0, 0)};
    order.add_marks(breaks, {});
    REQUIRE_EQ(show(order.take()), std::string("[break]"));
}

TEST_CASE("tx order: bytes written while a break is held follow its end")
{
    tx_order order;
    order.add_bytes(bytes_of("AB"));
    order.add_marks(std::vector<tx_break_mark>{held(10, 2)}, {});
    REQUIRE_EQ(show(order.take()), std::string("AB|[begin]"));
    REQUIRE(order.holding());

    // The program writes while the break is held: it waits.
    order.add_bytes(bytes_of("CD"));
    REQUIRE_EQ(show(order.take()), std::string());
    REQUIRE_EQ(order.waiting(), 2u);

    // The same open break given again changes nothing.
    order.add_marks(std::vector<tx_break_mark>{held(10, 2)}, {});
    REQUIRE_EQ(show(order.take()), std::string());

    order.add_marks(std::vector<tx_break_mark>{ended(10, 99, 2, 4)}, {});
    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("[end]CD|"));
    REQUIRE_EQ(out[0].on_time, 10u);
    REQUIRE_EQ(out[0].off_time, 99u);
    REQUIRE(!order.holding());
}

TEST_CASE("tx order: bytes after the end of a held break come after it")
{
    tx_order order;
    order.add_marks(std::vector<tx_break_mark>{held(10, 0)}, {});
    REQUIRE_EQ(show(order.take()), std::string("[begin]"));
    order.add_marks(std::vector<tx_break_mark>{ended(10, 20, 0, 0)}, {});
    order.add_bytes(bytes_of("XY"));
    REQUIRE_EQ(show(order.take()), std::string("[end]XY|"));
}

TEST_CASE("tx order: a break given as held and ended in one call is one item")
{
    tx_order order;
    order.add_marks(std::vector<tx_break_mark>{held(10, 1)}, {});
    order.add_marks(std::vector<tx_break_mark>{ended(10, 20, 1, 1)}, {});
    order.add_bytes(bytes_of("AB"));
    REQUIRE_EQ(show(order.take()), std::string("A|[break]B|"));
}

TEST_CASE("tx order: every write is a piece with the time of its call")
{
    tx_order order;
    const std::vector<tx_write_mark> writes{{100, 0}, {200, 2}, {350, 5}};
    order.add_marks({}, writes);
    order.add_bytes(bytes_of("ABCDEFG"));

    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("AB|CDE|FG|"));
    REQUIRE_EQ(*out[0].stamp, 100u);
    REQUIRE_EQ(*out[1].stamp, 200u);
    REQUIRE_EQ(*out[2].stamp, 350u);
}

TEST_CASE("tx order: a write split by a break keeps its time on both sides")
{
    tx_order order;
    order.add_marks(std::vector<tx_break_mark>{ended(5, 6, 2, 2)},
                    std::vector<tx_write_mark>{{100, 0}});
    order.add_bytes(bytes_of("ABCD"));
    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("AB|[break]CD|"));
    REQUIRE_EQ(*out[0].stamp, 100u);
    REQUIRE_EQ(*out[2].stamp, 100u);
}

TEST_CASE("tx order: marks may come in a later call than the bytes before them")
{
    tx_order order;
    order.add_bytes(bytes_of("AB"));
    REQUIRE_EQ(show(order.take()), std::string("AB|"));
    order.add_marks(std::vector<tx_break_mark>{ended(1, 2, 2, 2)},
                    std::vector<tx_write_mark>{{500, 2}});
    order.add_bytes(bytes_of("CD"));
    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("[break]CD|"));
    REQUIRE_EQ(*out[1].stamp, 500u);
}

TEST_CASE("tx order: a restart forgets the bytes and keeps a break that is held")
{
    tx_order order;
    order.add_marks(std::vector<tx_break_mark>{held(10, 0)}, {});
    order.add_bytes(bytes_of("AB"));
    REQUIRE_EQ(show(order.take()), std::string("[begin]"));

    order.restart();
    REQUIRE_EQ(order.waiting(), 0u);
    REQUIRE(order.holding());

    // The driver reports it again, still held, at offset 0 of the new handle.
    order.add_marks(std::vector<tx_break_mark>{held(10, 0)}, {});
    order.settle(50);
    REQUIRE_EQ(show(order.take()), std::string());
    REQUIRE(order.holding());

    order.add_marks(std::vector<tx_break_mark>{ended(10, 77, 0, 0)}, {});
    order.add_bytes(bytes_of("C"));
    REQUIRE_EQ(show(order.take()), std::string("[end]C|"));
}

TEST_CASE("tx order: a break that ended while the hub had no handle ends at settle")
{
    tx_order order;
    order.add_marks(std::vector<tx_break_mark>{held(10, 0)}, {});
    REQUIRE_EQ(show(order.take()), std::string("[begin]"));

    order.restart();
    // The driver no longer has it.
    order.add_marks({}, {});
    order.settle(123);
    const auto out = order.take();
    REQUIRE_EQ(show(out), std::string("[end]"));
    REQUIRE_EQ(out[0].off_time, 123u);
    REQUIRE(!order.holding());
}

TEST_CASE("tx order: a thousand breaks in a loop come out once each, in order")
{
    tx_order order;
    std::vector<tx_break_mark> breaks;
    std::string payload;
    for (std::uint64_t i = 0; i < 1000; ++i)
    {
        breaks.push_back(ended(i * 10, i * 10 + 1, i, i));
        payload += 'x';
    }
    order.add_marks(breaks, {});
    order.add_bytes(std::vector<std::uint8_t>(payload.begin(), payload.end()));
    const auto out = order.take();

    std::size_t seen = 0;
    std::uint64_t last = 0;
    for (const auto& piece : out)
    {
        if (piece.what != kind::break_complete)
            continue;
        REQUIRE(seen == 0 || piece.on_time > last);
        last = piece.on_time;
        ++seen;
    }
    REQUIRE_EQ(seen, std::size_t{1000});
}
