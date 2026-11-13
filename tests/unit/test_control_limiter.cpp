//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The failure limiter of the remote-control gateway: the free failures, the
// delay that doubles up to its limit, forgetting by success and by quiet,
// independent keys, the limit over all keys, `retry_at` and the size of the
// table.  Time is a number that the test advances by hand.
//

#include <vss/control/failure_limiter.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace control = vss::control;

namespace
{

using namespace std::chrono_literals;

using clock_type = control::failure_limiter::clock;
using time_point = clock_type::time_point;

/// A time that is not the epoch, so that a missing initialisation shows.
time_point start_time()
{
    return time_point{} + std::chrono::hours(1);
}

void fail_times(control::failure_limiter& limiter, std::string_view key,
                unsigned times, time_point now)
{
    for (unsigned attempt = 0; attempt < times; ++attempt)
        limiter.failed(key, now);
}

} // namespace

TEST_CASE("control limiter: the first failures cost nothing, the fifth waits")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    for (unsigned attempt = 1; attempt <= 4; ++attempt)
    {
        REQUIRE(limiter.allowed("source", start));
        limiter.failed("source", start);
        REQUIRE(limiter.allowed("source", start));
    }

    REQUIRE(limiter.allowed("source", start));
    limiter.failed("source", start);
    REQUIRE(!limiter.allowed("source", start));
    REQUIRE(!limiter.allowed("source", start + 999ms));
    REQUIRE(limiter.allowed("source", start + 1s));
    REQUIRE(limiter.allowed("source", start + 1001ms));
}

TEST_CASE("control limiter: the delay doubles and stops at five minutes")
{
    control::failure_limiter limiter;
    time_point now = start_time();
    fail_times(limiter, "source", 4, now);

    const std::chrono::seconds expected[] = {1s,  2s,   4s,   8s,   16s,  32s,
                                             64s, 128s, 256s, 300s, 300s, 300s};
    for (const std::chrono::seconds delay : expected)
    {
        limiter.failed("source", now);
        REQUIRE(!limiter.allowed("source", now));
        REQUIRE(!limiter.allowed("source", now + delay - 1ms));
        REQUIRE(limiter.allowed("source", now + delay));
        REQUIRE(limiter.retry_at("source", now) == now + delay);
        now += delay;
    }
}

TEST_CASE("control limiter: a success forgets the failures of a key")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    fail_times(limiter, "source", 5, start);
    REQUIRE(!limiter.allowed("source", start));
    REQUIRE_EQ(limiter.tracked(), std::size_t{1});

    limiter.succeeded("source");
    REQUIRE_EQ(limiter.tracked(), std::size_t{0});
    REQUIRE(limiter.allowed("source", start));

    // The count starts again from nothing: four more cost nothing.
    fail_times(limiter, "source", 4, start);
    REQUIRE(limiter.allowed("source", start));

    // A key that never failed can succeed without effect.
    limiter.succeeded("stranger");
    REQUIRE_EQ(limiter.tracked(), std::size_t{1});
}

TEST_CASE("control limiter: fifteen quiet minutes forget the failures")
{
    const time_point start = start_time();

    // Four failures, then one just before the quiet time has passed: it is
    // the fifth.
    control::failure_limiter before;
    fail_times(before, "source", 4, start);
    before.failed("source", start + 15min - 1ms);
    REQUIRE(!before.allowed("source", start + 15min - 1ms));

    // The same, one millisecond later: the key was forgotten, this is the
    // first failure of a new count.
    control::failure_limiter after;
    fail_times(after, "source", 4, start);
    after.failed("source", start + 15min);
    REQUIRE(after.allowed("source", start + 15min));
    REQUIRE_EQ(after.tracked(), std::size_t{1});
}

TEST_CASE("control limiter: asking forgets a key that has been quiet")
{
    control::failure_limits limits;
    limits.first_delay = 1h;
    limits.longest_delay = 1h;
    limits.quiet_reset = 10min;
    control::failure_limiter limiter(limits);
    const time_point start = start_time();

    fail_times(limiter, "source", 5, start);
    REQUIRE(!limiter.allowed("source", start + 10min - 1ms));
    REQUIRE_EQ(limiter.tracked(), std::size_t{1});

    REQUIRE(limiter.allowed("source", start + 10min));
    REQUIRE_EQ(limiter.tracked(), std::size_t{0});
}

TEST_CASE("control limiter: keys are independent")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    fail_times(limiter, "first", 5, start);
    fail_times(limiter, "second", 3, start);
    REQUIRE(!limiter.allowed("first", start));
    REQUIRE(limiter.allowed("second", start));
    REQUIRE(limiter.allowed("third", start));

    // The failures of one key do not add to those of another.
    fail_times(limiter, "second", 1, start);
    REQUIRE(limiter.allowed("second", start));
    fail_times(limiter, "second", 1, start);
    REQUIRE(!limiter.allowed("second", start));
    REQUIRE_EQ(limiter.tracked(), std::size_t{2});
}

TEST_CASE("control limiter: twenty failures in a second refuse every key")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    // Nineteen failures over nineteen keys: nothing is refused.
    for (unsigned number = 0; number < 19; ++number)
        limiter.failed("key" + std::to_string(number), start + 10ms * number);
    REQUIRE(limiter.allowed("never", start + 190ms));

    // The twentieth one makes the limit.
    limiter.failed("key19", start + 190ms);
    REQUIRE(!limiter.allowed("never", start + 190ms));
    REQUIRE(!limiter.allowed("key0", start + 500ms));
    REQUIRE(!limiter.allowed("never", start + 999ms));

    // The first failure leaves the second at start + 1 s.
    REQUIRE(limiter.allowed("never", start + 1s));
    REQUIRE(limiter.allowed("key0", start + 1s));
    REQUIRE_EQ(limiter.tracked(), std::size_t{20});
}

TEST_CASE("control limiter: retry_at says now when nothing refuses")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    REQUIRE(limiter.retry_at("stranger", start) == start);
    fail_times(limiter, "source", 4, start);
    REQUIRE(limiter.retry_at("source", start) == start);
    REQUIRE(limiter.retry_at("source", start + 1h) == start + 1h);
}

TEST_CASE("control limiter: retry_at says when a refused key may try again")
{
    control::failure_limiter limiter;
    const time_point start = start_time();

    fail_times(limiter, "source", 5, start);
    REQUIRE(limiter.retry_at("source", start) == start + 1s);
    REQUIRE(limiter.retry_at("source", start + 400ms) == start + 1s);
    // After the delay it is the time asked about.
    REQUIRE(limiter.retry_at("source", start + 2s) == start + 2s);
    // Another key is not held back.
    REQUIRE(limiter.retry_at("other", start) == start);
}

TEST_CASE("control limiter: retry_at counts the limit over all keys")
{
    control::failure_limits limits;
    limits.first_delay = 100ms;
    const time_point start = start_time();

    {
        // The key's own delay is short; the limit over all keys lasts longer.
        control::failure_limiter limiter(limits);
        fail_times(limiter, "source", 5, start);
        for (unsigned number = 0; number < 15; ++number)
            limiter.failed("key" + std::to_string(number), start);
        REQUIRE(limiter.retry_at("source", start) == start + 1s);
        REQUIRE(limiter.retry_at("fresh", start) == start + 1s);
        REQUIRE(limiter.retry_at("fresh", start + 300ms) == start + 1s);
        REQUIRE(limiter.retry_at("fresh", start + 1s) == start + 1s);
    }

    {
        // The key's own delay is the longer one.
        limits.first_delay = 10s;
        limits.longest_delay = 10s;
        control::failure_limiter limiter(limits);
        fail_times(limiter, "source", 5, start);
        for (unsigned number = 0; number < 15; ++number)
            limiter.failed("key" + std::to_string(number), start);
        REQUIRE(limiter.retry_at("source", start) == start + 10s);
        REQUIRE(limiter.retry_at("fresh", start) == start + 1s);
    }
}

TEST_CASE("control limiter: the table never holds more than max_keys")
{
    control::failure_limits limits;
    limits.free_failures = 2;
    limits.max_keys = 3;
    control::failure_limiter limiter(limits);
    const time_point start = start_time();

    limiter.failed("a", start);
    limiter.failed("b", start + 10ms);
    limiter.failed("c", start + 20ms);
    REQUIRE_EQ(limiter.tracked(), std::size_t{3});

    // A fourth key drops "a", which failed longest ago.
    limiter.failed("d", start + 30ms);
    REQUIRE_EQ(limiter.tracked(), std::size_t{3});

    // "a" is a new key again: its count starts at one and it is not refused.
    // It takes the place of "b".
    limiter.failed("a", start + 40ms);
    REQUIRE(limiter.allowed("a", start + 40ms));
    REQUIRE_EQ(limiter.tracked(), std::size_t{3});

    // "d" was remembered: a second failure is its second.
    limiter.failed("d", start + 50ms);
    REQUIRE(!limiter.allowed("d", start + 50ms));

    // Many different keys never make the table larger than its limit.
    for (unsigned number = 0; number < 50; ++number)
    {
        limiter.failed("many" + std::to_string(number),
                       start + 2s + 100ms * number);
        REQUIRE(limiter.tracked() <= std::size_t{3});
    }
    REQUIRE_EQ(limiter.tracked(), std::size_t{3});
}

TEST_CASE("control limiter: no free failures is taken as one")
{
    control::failure_limits limits;
    limits.free_failures = 0;
    control::failure_limiter limiter(limits);
    const time_point start = start_time();

    REQUIRE(limiter.allowed("source", start));
    limiter.failed("source", start);
    REQUIRE(!limiter.allowed("source", start));
    REQUIRE(!limiter.allowed("source", start + 999ms));
    REQUIRE(limiter.allowed("source", start + 1s));

    limiter.failed("source", start + 1s);
    REQUIRE(!limiter.allowed("source", start + 2999ms));
    REQUIRE(limiter.allowed("source", start + 3s));
}
