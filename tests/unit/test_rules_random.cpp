//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seeded source of a rule set.  Its draws must be the same numbers on
// every platform and compiler, because a harness repeats a run from its seed.
// The first draws of each kind are therefore pinned below as literal values:
// CI builds with GCC, Clang, AppleClang and MSVC, and a platform that drew
// differently (a different integer width, a different rounding, a library
// distribution slipping in) would fail these pins.
//

#include "test_framework.hpp"

#include <vss/rules/model.hpp>
#include <vss/rules/random.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>

using namespace vss;
using namespace vss::rules;

namespace
{

constexpr int draws = 20'000;

delay_spec uniform_spec(std::int64_t low, std::int64_t high)
{
    delay_spec spec;
    spec.kind = delay_kind::uniform;
    spec.min = microseconds(low);
    spec.max = microseconds(high);
    return spec;
}

delay_spec normal_spec(std::int64_t mean, std::int64_t sd, std::int64_t low,
                       std::int64_t high)
{
    delay_spec spec;
    spec.kind = delay_kind::normal;
    spec.mean = microseconds(mean);
    spec.sd = microseconds(sd);
    spec.min = microseconds(low);
    spec.max = microseconds(high);
    return spec;
}

delay_spec exponential_spec(std::int64_t mean, std::int64_t low,
                            std::int64_t high)
{
    delay_spec spec;
    spec.kind = delay_kind::exponential;
    spec.mean = microseconds(mean);
    spec.min = microseconds(low);
    spec.max = microseconds(high);
    return spec;
}

generator pinned_generator()
{
    return generator(derive_stream(7, "set", "rule"));
}

struct sample
{
    double mean = 0;
    double deviation = 0;
    std::int64_t lowest = 0;
    std::int64_t highest = 0;
};

sample draw_many(const delay_spec& spec, std::uint64_t stream)
{
    generator source(stream);
    double sum = 0;
    double squares = 0;
    sample out;
    out.lowest = std::numeric_limits<std::int64_t>::max();
    out.highest = std::numeric_limits<std::int64_t>::min();
    for (int i = 0; i < draws; ++i)
    {
        const std::int64_t value =
            draw_delay(spec, source, static_cast<std::uint64_t>(i)).count();
        out.lowest = std::min(out.lowest, value);
        out.highest = std::max(out.highest, value);
        sum += static_cast<double>(value);
        squares += static_cast<double>(value) * static_cast<double>(value);
    }
    out.mean = sum / draws;
    out.deviation = std::sqrt(squares / draws - out.mean * out.mean);
    return out;
}

} // namespace

TEST_CASE("rules: splitmix64 gives the published values for seed 0")
{
    std::uint64_t state = 0;
    REQUIRE_EQ(splitmix64(state), std::uint64_t{0xe220a8397b1dcdafULL});
    REQUIRE_EQ(splitmix64(state), std::uint64_t{0x6e789e6aa1b965f4ULL});
    REQUIRE_EQ(splitmix64(state), std::uint64_t{0x06c45d188009454fULL});
}

TEST_CASE("rules: a stream differs when the seed or either name differs")
{
    const std::uint64_t base = derive_stream(7, "set", "rule");
    REQUIRE_EQ(derive_stream(7, "set", "rule"), base);
    REQUIRE_NE(derive_stream(8, "set", "rule"), base);
    REQUIRE_NE(derive_stream(7, "set2", "rule"), base);
    REQUIRE_NE(derive_stream(7, "set", "rule2"), base);
    REQUIRE_NE(derive_stream(7, "rule", "set"), base);
    // Where the names divide is part of the stream.
    REQUIRE_NE(derive_stream(1, "ab", "c"), derive_stream(1, "a", "bc"));
    REQUIRE_NE(derive_stream(1, "", "abc"), derive_stream(1, "abc", ""));
    // Seeds one bit apart give streams far apart.
    const std::uint64_t one = derive_stream(100, "s", "r");
    const std::uint64_t two = derive_stream(101, "s", "r");
    REQUIRE(std::popcount(one ^ two) > 8);
}

TEST_CASE("rules: log2_q24 is exact for powers of two")
{
    for (std::uint32_t bit = 0; bit < 32; ++bit)
        REQUIRE_EQ(log2_q24(std::uint32_t{1} << bit), bit << 24U);
}

TEST_CASE("rules: log2_q24 is monotonic and close to the logarithm")
{
    std::uint32_t previous = 0;
    for (std::uint32_t value = 1; value <= 70'000; ++value)
    {
        const std::uint32_t now = log2_q24(value);
        REQUIRE(now >= previous);
        previous = now;
    }
    // A sample over the whole range, including both ends.
    previous = 0;
    for (std::uint64_t value = 1; value <= 0xffffffffULL; value += 40'009)
    {
        const auto narrow = static_cast<std::uint32_t>(value);
        const std::uint32_t now = log2_q24(narrow);
        REQUIRE(now >= previous);
        previous = now;
        const double exact = std::log2(static_cast<double>(narrow));
        REQUIRE_NEAR(static_cast<double>(now) / 16777216.0, exact, 1e-5);
    }
    REQUIRE(log2_q24(0xffffffffU) >= previous);
    REQUIRE(log2_q24(0xffffffffU) < (std::uint32_t{32} << 24U));
}

TEST_CASE("rules: between stays in range and reaches both ends")
{
    generator source(1234);
    std::set<std::uint64_t> seen;
    for (int i = 0; i < 2000; ++i)
    {
        const std::uint64_t value = source.between(10, 14);
        REQUIRE(value >= 10);
        REQUIRE(value <= 14);
        seen.insert(value);
    }
    REQUIRE_EQ(seen.size(), std::size_t{5});

    for (int i = 0; i < 100; ++i)
        REQUIRE_EQ(source.between(42, 42), std::uint64_t{42});

    // The two ends of the whole range, and the whole range.
    constexpr std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
    bool low = false;
    bool high = false;
    for (int i = 0; i < 200; ++i)
    {
        const std::uint64_t value = source.between(top - 1, top);
        REQUIRE(value >= top - 1);
        low = low || value == top - 1;
        high = high || value == top;
    }
    REQUIRE(low);
    REQUIRE(high);
    bool above_half = false;
    bool below_half = false;
    for (int i = 0; i < 200; ++i)
    {
        const std::uint64_t value = source.between(0, top);
        above_half = above_half || value > top / 2;
        below_half = below_half || value <= top / 2;
    }
    REQUIRE(above_half);
    REQUIRE(below_half);

    // A range more than half of the whole one takes the rejection path.
    constexpr std::uint64_t most = (top / 4) * 3;
    for (int i = 0; i < 200; ++i)
        REQUIRE(source.between(0, most) <= most);
}

TEST_CASE("rules: between is even over a small range")
{
    generator source(99);
    std::array<int, 6> counts{};
    for (int i = 0; i < 12'000; ++i)
        ++counts[static_cast<std::size_t>(source.between(0, 5))];
    for (const int count : counts)
        REQUIRE(count > 1800);
}

TEST_CASE("rules: the generator counts what it drew")
{
    generator source(5);
    REQUIRE_EQ(source.drawn(), std::uint64_t{0});
    static_cast<void>(source.next());
    static_cast<void>(source.next64());
    REQUIRE_EQ(source.drawn(), std::uint64_t{3});
}

TEST_CASE("rules: two generators from one stream draw the same")
{
    generator a(derive_stream(3, "x", "y"));
    generator b(derive_stream(3, "x", "y"));
    generator c(derive_stream(4, "x", "y"));
    bool differs = false;
    for (int i = 0; i < 32; ++i)
    {
        const std::uint32_t value = a.next();
        REQUIRE_EQ(b.next(), value);
        differs = differs || c.next() != value;
    }
    REQUIRE(differs);
}

TEST_CASE("rules: a fixed delay is the value")
{
    generator source(1);
    delay_spec spec;
    spec.kind = delay_kind::fixed;
    spec.fixed = microseconds(1234);
    REQUIRE(draw_delay(spec, source, 0) == microseconds(1234));
    REQUIRE_EQ(source.drawn(), std::uint64_t{0});
}

TEST_CASE("rules: uniform delays stay within bounds and have the mean")
{
    const delay_spec spec = uniform_spec(1000, 3000);
    const sample got = draw_many(spec, 11);
    REQUIRE(got.lowest >= 1000);
    REQUIRE(got.highest <= 3000);
    REQUIRE(got.lowest <= 1010);
    REQUIRE(got.highest >= 2990);
    REQUIRE_NEAR(got.mean, 2000.0, 40.0);
    // A uniform spread over 2000 has a deviation of 2000 / sqrt(12).
    REQUIRE_NEAR(got.deviation, 577.35, 30.0);

    const sample point = draw_many(uniform_spec(500, 500), 12);
    REQUIRE_EQ(point.lowest, std::int64_t{500});
    REQUIRE_EQ(point.highest, std::int64_t{500});
}

TEST_CASE("rules: normal delays stay within bounds and have the mean and sd")
{
    // Bounds far from the mean: the cut touches nothing.
    const delay_spec spec = normal_spec(5000, 400, 1000, 9000);
    const sample got = draw_many(spec, 21);
    REQUIRE(got.lowest >= 1000);
    REQUIRE(got.highest <= 9000);
    REQUIRE_NEAR(got.mean, 5000.0, 50.0);
    REQUIRE_NEAR(got.deviation, 400.0, 20.0);
}

TEST_CASE("rules: normal delays are cut at the bounds")
{
    const delay_spec spec = normal_spec(5000, 2000, 4500, 5500);
    const sample got = draw_many(spec, 22);
    REQUIRE(got.lowest >= 4500);
    REQUIRE(got.highest <= 5500);
    // Most of the mass is outside, so both cuts are reached.
    REQUIRE_EQ(got.lowest, std::int64_t{4500});
    REQUIRE_EQ(got.highest, std::int64_t{5500});

    // No spread at all: always the mean.
    const sample flat = draw_many(normal_spec(3000, 0, 0, 6000), 23);
    REQUIRE_EQ(flat.lowest, std::int64_t{3000});
    REQUIRE_EQ(flat.highest, std::int64_t{3000});
}

TEST_CASE("rules: exponential delays stay within bounds and have the mean")
{
    // The upper cut is a hundred means out: it touches nothing.
    const delay_spec spec = exponential_spec(1000, 0, 100'000);
    const sample got = draw_many(spec, 31);
    REQUIRE(got.lowest >= 0);
    REQUIRE(got.highest <= 100'000);
    REQUIRE_NEAR(got.mean, 1000.0, 50.0);
    REQUIRE_NEAR(got.deviation, 1000.0, 80.0);

    const sample cut = draw_many(exponential_spec(1000, 400, 2500), 32);
    REQUIRE(cut.lowest >= 400);
    REQUIRE(cut.highest <= 2500);
    REQUIRE_EQ(cut.lowest, std::int64_t{400});
    REQUIRE_EQ(cut.highest, std::int64_t{2500});
}

TEST_CASE("rules: a table taken in order cycles through its values")
{
    delay_spec spec;
    spec.kind = delay_kind::table;
    spec.values = {microseconds(10), microseconds(20), microseconds(30)};
    spec.in_order = true;
    generator source(1);
    for (std::uint64_t position = 0; position < 9; ++position)
    {
        REQUIRE(draw_delay(spec, source, position) ==
                spec.values[position % 3]);
    }
    // Taking in order draws nothing.
    REQUIRE_EQ(source.drawn(), std::uint64_t{0});
}

TEST_CASE("rules: a table picked by the source yields only listed values")
{
    delay_spec spec;
    spec.kind = delay_kind::table;
    spec.values = {microseconds(7), microseconds(70), microseconds(700),
                   microseconds(7000)};
    spec.in_order = false;
    generator source(77);
    std::set<std::int64_t> seen;
    for (int i = 0; i < 2000; ++i)
    {
        const microseconds value = draw_delay(spec, source, 0);
        REQUIRE(value == spec.values[0] || value == spec.values[1] ||
                value == spec.values[2] || value == spec.values[3]);
        seen.insert(value.count());
    }
    REQUIRE_EQ(seen.size(), std::size_t{4});

    // The position does not matter to a table picked at random.
    generator one(5);
    generator two(5);
    for (std::uint64_t position = 0; position < 50; ++position)
        REQUIRE(draw_delay(spec, one, position) ==
                draw_delay(spec, two, 1000 - position));

    delay_spec empty;
    empty.kind = delay_kind::table;
    REQUIRE(draw_delay(empty, source, 3) == microseconds(0));
}

TEST_CASE("rules: golden values of the generator, the same on every platform")
{
    // These are literals on purpose: the draws are whole-number arithmetic of
    // fixed width, so every platform must give exactly these.
    generator source = pinned_generator();
    constexpr std::array<std::uint32_t, 8> expected{
        3218451926U, 3778138177U, 2689675723U, 3652362882U,
        2163979796U, 3269921902U, 3480588089U, 1952149746U};
    for (const std::uint32_t want : expected)
        REQUIRE_EQ(source.next(), want);
}

TEST_CASE("rules: golden values of the uniform, normal and exponential draws")
{
    constexpr std::array<std::int64_t, 8> uniform_expected{
        4924, 1616, 1054, 5400, 1111, 7750, 8133, 3187};
    constexpr std::array<std::int64_t, 8> normal_expected{
        5883, 5095, 5206, 5435, 4860, 5079, 5563, 5016};
    constexpr std::array<std::int64_t, 8> exponential_expected{
        577, 256, 936, 324, 1370, 545, 420, 1577};

    const delay_spec uniform = uniform_spec(1000, 9000);
    const delay_spec normal = normal_spec(5000, 400, 1000, 9000);
    const delay_spec exponential = exponential_spec(2000, 0, 20'000);

    generator from_uniform = pinned_generator();
    generator from_normal = pinned_generator();
    generator from_exponential = pinned_generator();
    for (std::size_t i = 0; i < 8; ++i)
    {
        REQUIRE_EQ(draw_delay(uniform, from_uniform, 0).count(),
                   uniform_expected[i]);
        REQUIRE_EQ(draw_delay(normal, from_normal, 0).count(),
                   normal_expected[i]);
        REQUIRE_EQ(draw_delay(exponential, from_exponential, 0).count(),
                   exponential_expected[i]);
    }
}
