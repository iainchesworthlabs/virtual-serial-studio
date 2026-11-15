//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The restart rules of one plugin (docs/plugins.md, section 5.5): the waits
// after each failure, the window, the reset after a long run, the quarantine
// and the stop.  A `plugin_run` has no clock, so every test says what
// happened and what time it is, and no test waits.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/lifecycle.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;

namespace
{

/// The time a test's plugin was started at.
constexpr run_time t0{run_time::duration{1000s}};

error_code crash()
{
    return make_error_code(plugin_errc::crashed);
}

/// A run whose waits are exactly the ones in the table.
plugin_run exact()
{
    restart_options options;
    options.jitter_percent = 0;
    return plugin_run(options);
}

/// A run that was started at `now`.
plugin_run started(run_time now)
{
    plugin_run run = exact();
    const auto step = run.start(now);
    REQUIRE(step.start_now);
    return run;
}

/// The milliseconds from one time to another.
std::int64_t ms_between(run_time from, run_time to)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
        .count();
}

/// A call that asks nothing of the owner.
bool asks_nothing(const run_step& step)
{
    return !step.start_now && !step.stop_now;
}

/// A call that changed nothing: the same state and nothing asked.
void require_unchanged(const run_step& step, run_state expected)
{
    REQUIRE_EQ(step.state, expected);
    REQUIRE(asks_nothing(step));
    REQUIRE(!step.wake_at.has_value());
}

/// Fails the run at `now`, checks that it backs off, and returns the time
/// it asked to be called at.
run_time fail(plugin_run& run, run_time now)
{
    const auto step = run.on_failed(crash(), now);
    REQUIRE_EQ(step.state, run_state::backing_off);
    REQUIRE(asks_nothing(step));
    REQUIRE(step.wake_at.has_value());
    REQUIRE(run.next_start() == step.wake_at);
    return *step.wake_at;
}

/// Fails the run at `now`, then calls it at the time it asked for, so that
/// it is starting again.  Returns that time.
run_time fail_and_wake(plugin_run& run, run_time now)
{
    const run_time wake = fail(run, now);
    const auto step = run.on_time(wake);
    REQUIRE_EQ(step.state, run_state::starting);
    REQUIRE(step.start_now);
    REQUIRE(!step.wake_at.has_value());
    return wake;
}

/// A run that failed five times in a row, the last one at about `now` plus
/// fifteen seconds.
plugin_run quarantined_run(run_time now)
{
    plugin_run run = started(now);
    for (std::size_t i = 0; i < 4; ++i)
        now = fail_and_wake(run, now);
    const auto step = run.on_failed(crash(), now);
    REQUIRE_EQ(step.state, run_state::quarantined);
    return run;
}

/// What a second failure gets, and how many failures count then.
struct second_failure
{
    std::int64_t wait_ms = 0;
    std::size_t failures = 0;
};

/// The plugin fails three times, starts again and becomes ready, and fails
/// a fourth time `ready_for` after that.
second_failure failure_after_ready_for(std::chrono::milliseconds ready_for)
{
    plugin_run run = started(t0);
    run_time now = t0;
    for (std::size_t i = 0; i < 3; ++i)
        now = fail_and_wake(run, now + 1s);
    const run_time ready = now + 1s;
    REQUIRE_EQ(run.on_ready(ready).state, run_state::ready);
    const run_time failed = ready + ready_for;
    const run_time wake = fail(run, failed);
    second_failure result;
    result.wait_ms = ms_between(failed, wake);
    result.failures = run.failures(failed);
    return result;
}

/// The wait after a second failure that comes `gap` after the first.
std::int64_t wait_after_gap(std::chrono::milliseconds gap)
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0);
    REQUIRE(run.on_time(wake).start_now);
    const run_time failed = t0 + gap;
    return ms_between(failed, fail(run, failed));
}

/// The first wait of a run that is started at t0 and fails a second later.
std::int64_t first_wait(const restart_options& options, std::uint64_t seed)
{
    plugin_run run(options, seed);
    REQUIRE(run.start(t0).start_now);
    const auto step = run.on_failed(crash(), t0 + 1s);
    REQUIRE_EQ(step.state, run_state::backing_off);
    REQUIRE(step.wake_at.has_value());
    return ms_between(t0 + 1s, *step.wake_at);
}

} // namespace

TEST_CASE("plugin lifecycle: a new run is stopped and starts when told to")
{
    plugin_run run = exact();
    REQUIRE_EQ(run.state(), run_state::stopped);
    REQUIRE(!run.next_start().has_value());
    REQUIRE(!run.last_failure());
    REQUIRE_EQ(run.failures(t0), std::size_t{0});

    const auto start = run.start(t0);
    REQUIRE_EQ(start.state, run_state::starting);
    REQUIRE(start.start_now);
    REQUIRE(!start.stop_now);
    REQUIRE(!start.wake_at.has_value());
    REQUIRE_EQ(run.state(), run_state::starting);

    const auto ready = run.on_ready(t0 + 2s);
    require_unchanged(ready, run_state::ready);
    REQUIRE_EQ(run.state(), run_state::ready);
}

TEST_CASE("plugin lifecycle: calls that mean nothing in a state change "
          "nothing")
{
    plugin_run run = exact();
    require_unchanged(run.on_ready(t0), run_state::stopped);
    require_unchanged(run.on_failed(crash(), t0), run_state::stopped);
    require_unchanged(run.on_stopped(t0), run_state::stopped);
    require_unchanged(run.on_time(t0), run_state::stopped);
    require_unchanged(run.stop(t0), run_state::stopped);
    // A failure nobody was waiting for is not counted.
    REQUIRE_EQ(run.failures(t0), std::size_t{0});
    REQUIRE(!run.last_failure());

    REQUIRE(run.start(t0).start_now);
    require_unchanged(run.start(t0 + 1s), run_state::starting);
    require_unchanged(run.on_stopped(t0 + 1s), run_state::starting);
    require_unchanged(run.on_time(t0 + 1s), run_state::starting);

    REQUIRE_EQ(run.on_ready(t0 + 2s).state, run_state::ready);
    require_unchanged(run.start(t0 + 3s), run_state::ready);
    require_unchanged(run.on_ready(t0 + 3s), run_state::ready);
    require_unchanged(run.on_stopped(t0 + 3s), run_state::ready);
    require_unchanged(run.on_time(t0 + 3s), run_state::ready);

    REQUIRE(run.stop(t0 + 4s).stop_now);
    require_unchanged(run.start(t0 + 5s), run_state::stopping);
    require_unchanged(run.on_ready(t0 + 5s), run_state::stopping);
    require_unchanged(run.on_time(t0 + 5s), run_state::stopping);
    require_unchanged(run.stop(t0 + 5s), run_state::stopping);
    REQUIRE_EQ(run.failures(t0 + 5s), std::size_t{0});
}

TEST_CASE("plugin lifecycle: the waits are 1, 2, 4 and 8 seconds and the "
          "fifth failure quarantines")
{
    plugin_run run = started(t0);
    REQUIRE_EQ(run.failure_limit(), std::size_t{5});

    constexpr std::int64_t expected[] = {1000, 2000, 4000, 8000};
    run_time now = t0 + 1s;
    std::size_t count = 0;
    for (const std::int64_t wait : expected)
    {
        const auto step = run.on_failed(crash(), now);
        ++count;
        REQUIRE_EQ(step.state, run_state::backing_off);
        REQUIRE(asks_nothing(step));
        REQUIRE(step.wake_at.has_value());
        REQUIRE_EQ(ms_between(now, *step.wake_at), wait);
        REQUIRE(run.next_start() == step.wake_at);
        REQUIRE_EQ(run.failures(now), count);
        REQUIRE_EQ(run.state(), run_state::backing_off);

        now = *step.wake_at;
        const auto again = run.on_time(now);
        REQUIRE_EQ(again.state, run_state::starting);
        REQUIRE(again.start_now);
        now += 1s;
    }

    const auto last = run.on_failed(crash(), now);
    REQUIRE_EQ(last.state, run_state::quarantined);
    REQUIRE(asks_nothing(last));
    REQUIRE(!last.wake_at.has_value());
    REQUIRE(!run.next_start().has_value());
    REQUIRE_EQ(run.failures(now), std::size_t{5});
    REQUIRE_EQ(run.failures(now), run.failure_limit());
}

TEST_CASE("plugin lifecycle: a call before the time does nothing and one at "
          "the time starts")
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0 + 1s);

    const auto early = run.on_time(wake - 1ms);
    REQUIRE_EQ(early.state, run_state::backing_off);
    REQUIRE(asks_nothing(early));
    REQUIRE(early.wake_at == std::optional<run_time>{wake});
    REQUIRE(run.next_start() == std::optional<run_time>{wake});

    const auto on_time = run.on_time(wake);
    REQUIRE_EQ(on_time.state, run_state::starting);
    REQUIRE(on_time.start_now);
    REQUIRE(!on_time.wake_at.has_value());
    REQUIRE(!run.next_start().has_value());

    // The same call again, and the wake-up of the old time, start nothing.
    require_unchanged(run.on_time(wake), run_state::starting);
    require_unchanged(run.on_time(wake + 1h), run_state::starting);
}

TEST_CASE("plugin lifecycle: a call after the time starts as well")
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0 + 1s);
    const auto late = run.on_time(wake + 30s);
    REQUIRE_EQ(late.state, run_state::starting);
    REQUIRE(late.start_now);
    REQUIRE(!late.wake_at.has_value());
}

TEST_CASE("plugin lifecycle: ready for a few seconds counts like never "
          "ready")
{
    plugin_run quick = started(t0);
    plugin_run silent = started(t0);
    run_time now = t0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        now += 2s;
        REQUIRE_EQ(quick.on_ready(now).state, run_state::ready);
        now += 3s;
        const auto quick_step = quick.on_failed(crash(), now);
        const auto silent_step = silent.on_failed(crash(), now);
        REQUIRE(quick_step.wake_at.has_value());
        REQUIRE(silent_step.wake_at.has_value());
        REQUIRE(*quick_step.wake_at == *silent_step.wake_at);
        REQUIRE_EQ(quick.failures(now), i + 1);
        REQUIRE_EQ(silent.failures(now), i + 1);

        now = *quick_step.wake_at;
        REQUIRE(quick.on_time(now).start_now);
        REQUIRE(silent.on_time(now).start_now);
    }

    now += 1s;
    REQUIRE_EQ(quick.on_failed(crash(), now).state, run_state::quarantined);
    REQUIRE_EQ(silent.on_failed(crash(), now).state, run_state::quarantined);
}

TEST_CASE("plugin lifecycle: a run ready for five minutes forgets the "
          "failures before it")
{
    const auto result = failure_after_ready_for(5min);
    REQUIRE_EQ(result.wait_ms, 1000);
    REQUIRE_EQ(result.failures, std::size_t{1});
}

TEST_CASE("plugin lifecycle: a run ready for a millisecond less than five "
          "minutes does not")
{
    const auto result = failure_after_ready_for(5min - 1ms);
    REQUIRE_EQ(result.wait_ms, 8000);
    REQUIRE_EQ(result.failures, std::size_t{4});
}

TEST_CASE("plugin lifecycle: a failure exactly ten minutes old does not "
          "count and one a millisecond younger does")
{
    plugin_run run = started(t0);
    REQUIRE(fail(run, t0) > t0);
    REQUIRE_EQ(run.failures(t0), std::size_t{1});
    REQUIRE_EQ(run.failures(t0 + 10min - 1ms), std::size_t{1});
    REQUIRE_EQ(run.failures(t0 + 10min), std::size_t{0});
    REQUIRE_EQ(run.failures(t0 + 10min + 1ms), std::size_t{0});
}

TEST_CASE("plugin lifecycle: the age of a failure sets the next wait")
{
    // The first failure was a wait of one second.  The second one is a wait
    // of two seconds when the first still counts, and of one when not.
    REQUIRE_EQ(wait_after_gap(10min - 1ms), 2000);
    REQUIRE_EQ(wait_after_gap(10min), 1000);
    REQUIRE_EQ(wait_after_gap(10min + 1ms), 1000);
}

TEST_CASE("plugin lifecycle: four old failures do not quarantine on the "
          "fifth")
{
    plugin_run run = started(t0);
    run_time now = t0;
    for (std::size_t i = 0; i < 4; ++i)
        now = fail_and_wake(run, now);

    const run_time later = t0 + 11min;
    REQUIRE_EQ(run.failures(later), std::size_t{0});
    const run_time wake = fail(run, later);
    REQUIRE_EQ(ms_between(later, wake), 1000);
    REQUIRE_EQ(run.failures(later), std::size_t{1});
}

TEST_CASE("plugin lifecycle: a quarantined plugin stays quarantined until "
          "it is started")
{
    plugin_run run = quarantined_run(t0);
    REQUIRE_EQ(run.state(), run_state::quarantined);
    require_unchanged(run.on_time(t0 + 24h), run_state::quarantined);
    require_unchanged(run.on_ready(t0 + 24h), run_state::quarantined);
    require_unchanged(run.on_failed(crash(), t0 + 24h), run_state::quarantined);
    require_unchanged(run.on_stopped(t0 + 24h), run_state::quarantined);
    REQUIRE(!run.next_start().has_value());
    REQUIRE_EQ(run.last_failure(), crash());
}

TEST_CASE("plugin lifecycle: a start from quarantine counts failures "
          "afresh")
{
    plugin_run run = quarantined_run(t0);
    const run_time user = t0 + 1min;
    REQUIRE_EQ(run.failures(user), run.failure_limit());

    const auto step = run.start(user);
    REQUIRE_EQ(step.state, run_state::starting);
    REQUIRE(step.start_now);
    REQUIRE(!step.wake_at.has_value());
    REQUIRE_EQ(run.failures(user), std::size_t{0});
    REQUIRE(!run.last_failure());

    run_time now = user;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const run_time wake = fail(run, now);
        REQUIRE_EQ(ms_between(now, wake), std::int64_t{1000} << i);
        REQUIRE(run.on_time(wake).start_now);
        now = wake;
    }
    REQUIRE_EQ(run.on_failed(crash(), now).state, run_state::quarantined);
}

TEST_CASE("plugin lifecycle: a start while backing off starts at once and "
          "the failure still counts")
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0 + 1s);
    const run_time user = t0 + 1s + 100ms;
    REQUIRE(user < wake);

    const auto step = run.start(user);
    REQUIRE_EQ(step.state, run_state::starting);
    REQUIRE(step.start_now);
    REQUIRE(!step.wake_at.has_value());
    REQUIRE(!run.next_start().has_value());
    REQUIRE_EQ(run.failures(user), std::size_t{1});

    // The old wake-up is no longer asked for and starts nothing.
    require_unchanged(run.on_time(wake), run_state::starting);

    const run_time failed = user + 1s;
    const run_time next = fail(run, failed);
    REQUIRE_EQ(ms_between(failed, next), 2000);
    REQUIRE_EQ(run.failures(failed), std::size_t{2});
}

TEST_CASE("plugin lifecycle: stopping a plugin that runs waits for it to "
          "be gone")
{
    plugin_run starting = started(t0);
    const auto from_starting = starting.stop(t0 + 1s);
    REQUIRE_EQ(from_starting.state, run_state::stopping);
    REQUIRE(from_starting.stop_now);
    REQUIRE(!from_starting.start_now);
    REQUIRE(!from_starting.wake_at.has_value());

    plugin_run run = started(t0);
    REQUIRE_EQ(run.on_ready(t0 + 1s).state, run_state::ready);
    const auto step = run.stop(t0 + 2s);
    REQUIRE_EQ(step.state, run_state::stopping);
    REQUIRE(step.stop_now);
    REQUIRE(!step.start_now);
    REQUIRE(!step.wake_at.has_value());
    REQUIRE_EQ(run.state(), run_state::stopping);

    const auto gone = run.on_stopped(t0 + 3s);
    require_unchanged(gone, run_state::stopped);
    REQUIRE_EQ(run.state(), run_state::stopped);
    REQUIRE(run.start(t0 + 4s).start_now);
}

TEST_CASE("plugin lifecycle: a failure while stopping ends the stop and is "
          "not counted")
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0 + 1s);
    REQUIRE(run.on_time(wake).start_now);
    REQUIRE_EQ(run.on_ready(wake + 1s).state, run_state::ready);
    REQUIRE(run.stop(wake + 2s).stop_now);

    const auto step = run.on_failed(crash(), wake + 3s);
    require_unchanged(step, run_state::stopped);
    REQUIRE_EQ(run.failures(wake + 3s), std::size_t{1});

    // A failure after the stop is over means nothing either.
    require_unchanged(run.on_failed(crash(), wake + 4s), run_state::stopped);
    REQUIRE_EQ(run.failures(wake + 4s), std::size_t{1});

    // The next failure is the second, not the third.
    REQUIRE(run.start(wake + 5s).start_now);
    const run_time failed = wake + 6s;
    REQUIRE_EQ(ms_between(failed, fail(run, failed)), 2000);
}

TEST_CASE("plugin lifecycle: stopping while backing off stops at once")
{
    plugin_run run = started(t0);
    const run_time wake = fail(run, t0 + 1s);

    const auto step = run.stop(t0 + 2s);
    require_unchanged(step, run_state::stopped);
    REQUIRE(!run.next_start().has_value());

    require_unchanged(run.on_time(wake), run_state::stopped);
    require_unchanged(run.on_time(wake + 1h), run_state::stopped);
}

TEST_CASE("plugin lifecycle: stopping while quarantined stops at once and "
          "the next start counts afresh")
{
    plugin_run run = quarantined_run(t0);
    const run_time user = t0 + 1min;

    const auto step = run.stop(user);
    require_unchanged(step, run_state::stopped);
    REQUIRE_EQ(run.failures(user), std::size_t{0});

    REQUIRE(run.start(user + 1s).start_now);
    const run_time failed = user + 2s;
    REQUIRE_EQ(ms_between(failed, fail(run, failed)), 1000);
    REQUIRE_EQ(run.failures(failed), std::size_t{1});
}

TEST_CASE("plugin lifecycle: the last failure holds the latest reason")
{
    plugin_run run = started(t0);
    REQUIRE(!run.last_failure());

    const auto exited = make_error_code(plugin_errc::exited);
    const auto unresponsive = make_error_code(plugin_errc::unresponsive);

    REQUIRE_EQ(run.on_failed(crash(), t0 + 1s).state, run_state::backing_off);
    REQUIRE_EQ(run.last_failure(), crash());

    REQUIRE(run.on_time(t0 + 10s).start_now);
    REQUIRE_EQ(run.on_failed(exited, t0 + 11s).state, run_state::backing_off);
    REQUIRE_EQ(run.last_failure(), exited);

    REQUIRE(run.on_time(t0 + 20s).start_now);
    REQUIRE_EQ(run.on_ready(t0 + 21s).state, run_state::ready);
    REQUIRE_EQ(run.on_failed(unresponsive, t0 + 22s).state,
               run_state::backing_off);
    REQUIRE_EQ(run.last_failure(), unresponsive);

    // A call that is not a failure leaves it alone.
    const auto ignored = run.on_failed(crash(), t0 + 23s);
    REQUIRE_EQ(ignored.state, run_state::backing_off);
    REQUIRE(asks_nothing(ignored));
    REQUIRE_EQ(run.last_failure(), unresponsive);
}

TEST_CASE("plugin lifecycle: the jitter adds up to 20 percent to a wait")
{
    const restart_options defaults{};
    REQUIRE_EQ(defaults.jitter_percent, 20U);

    const std::int64_t first = first_wait(defaults, 0);
    bool differ = false;
    for (std::uint64_t seed = 0; seed < 50; ++seed)
    {
        const std::int64_t wait = first_wait(defaults, seed);
        REQUIRE(wait >= 1000);
        REQUIRE(wait <= 1200);
        // The same seed gives the same wait.
        REQUIRE_EQ(first_wait(defaults, seed), wait);
        if (wait != first)
            differ = true;
    }
    REQUIRE(differ);
}

TEST_CASE("plugin lifecycle: a jitter above 100 percent is 100 percent")
{
    restart_options options;
    options.jitter_percent = 500;

    bool above_twenty = false;
    for (std::uint64_t seed = 0; seed < 50; ++seed)
    {
        const std::int64_t wait = first_wait(options, seed);
        REQUIRE(wait >= 1000);
        REQUIRE(wait <= 2000);
        if (wait > 1200)
            above_twenty = true;
    }
    REQUIRE(above_twenty);
}

TEST_CASE("plugin lifecycle: with no waits the first failure quarantines")
{
    restart_options options;
    options.waits.clear();
    options.jitter_percent = 0;
    plugin_run run(options);
    REQUIRE_EQ(run.failure_limit(), std::size_t{1});

    REQUIRE(run.start(t0).start_now);
    const auto step = run.on_failed(crash(), t0 + 1s);
    REQUIRE_EQ(step.state, run_state::quarantined);
    REQUIRE(asks_nothing(step));
    REQUIRE(!step.wake_at.has_value());
    REQUIRE(!run.next_start().has_value());
    REQUIRE_EQ(run.failures(t0 + 1s), std::size_t{1});
}

namespace
{

/// SplitMix64.  The standard library's distributions give different numbers
/// in different implementations, and a failing seed must fail everywhere.
class generator
{
public:
    explicit generator(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next()
    {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    /// A number from 0 up to, and not including, `limit`.
    std::uint64_t below(std::uint64_t limit) { return next() % limit; }

private:
    std::uint64_t state_;
};

/// From nothing to two minutes, and mostly a few seconds, so that waits
/// are reached and the window passes as well.
std::chrono::milliseconds advance(generator& random)
{
    const std::uint64_t limit = random.below(4) == 0 ? 120001U : 9000U;
    return std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(random.below(limit))};
}

constexpr std::uint64_t call_count = 12;
constexpr std::uint64_t failed_call = 4;
constexpr std::uint64_t time_call = 9;

/// One of the six calls, chosen by `call`, below `call_count`.
run_step apply(plugin_run& run, std::uint64_t call, run_time now)
{
    switch (call)
    {
    case 0:
    case 1:
        return run.start(now);
    case 2:
    case 3:
        return run.on_ready(now);
    case 4:
    case 5:
    case 6:
        return run.on_failed(crash(), now);
    case 7:
        return run.stop(now);
    case 8:
        return run.on_stopped(now);
    default:
        return run.on_time(now);
    }
}

bool may_start_from(run_state state)
{
    return state == run_state::stopped || state == run_state::backing_off ||
           state == run_state::quarantined;
}

bool runs(run_state state)
{
    return state == run_state::starting || state == run_state::ready;
}

/// What must hold after every call, whatever the calls were.
void check_after_call(const plugin_run& run, const run_step& step,
                      run_state before, run_time now, run_time last_failed)
{
    REQUIRE_EQ(step.state, run.state());

    const bool backing_off = step.state == run_state::backing_off;
    REQUIRE_EQ(step.wake_at.has_value(), backing_off);
    REQUIRE(step.wake_at == run.next_start());

    REQUIRE(!(step.start_now && step.stop_now));
    if (step.start_now)
    {
        REQUIRE_EQ(step.state, run_state::starting);
        REQUIRE(may_start_from(before));
    }
    if (step.stop_now)
    {
        REQUIRE_EQ(step.state, run_state::stopping);
        REQUIRE(runs(before));
    }

    REQUIRE(run.failures(now) <= run.failure_limit());

    // The failure that gave the wait still counts, or it is ten minutes
    // old or more.
    if (backing_off)
        REQUIRE(run.failures(now) >= 1 || now - last_failed >= 10min);
}

} // namespace

TEST_CASE("plugin lifecycle: any run of calls keeps the answers consistent")
{
    std::array<bool, 6> seen{};
    for (std::uint64_t seed = 0; seed < 200; ++seed)
    {
        generator random(seed);
        plugin_run run(restart_options{}, seed);
        run_time now = t0;
        run_time last_failed = t0;
        for (std::size_t i = 0; i < 500; ++i)
        {
            now += advance(random);
            const std::uint64_t call = random.below(call_count);
            const auto due = run.next_start();
            if (call >= time_call && due.has_value() && random.below(2) == 0)
                now = std::max(now, *due);

            const run_state before = run.state();
            const run_step step = apply(run, call, now);
            if (call >= failed_call && call < failed_call + 3 && runs(before))
                last_failed = now;

            check_after_call(run, step, before, now, last_failed);
            seen[static_cast<std::size_t>(step.state)] = true;
        }
    }

    // The calls did get everywhere.
    for (const bool state_seen : seen)
        REQUIRE(state_seen);
}
