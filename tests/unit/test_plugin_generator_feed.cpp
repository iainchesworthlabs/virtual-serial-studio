//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's side of a generator, with the plugin and the port played from
// memory: one plugin that behaves and ones that break each rule, a port
// that is fast, slow, full or gone.  There is no process, no channel and no
// clock here; a test says what arrives and what time it is.
//

#include <vss/core/bytes.hpp>
#include <vss/plugin/host/generator_feed.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin::host;
using namespace std::chrono_literals;

namespace
{

constexpr feed_time t0{feed_time::duration{1000s}};

/// The port of a test: takes what it is told to, and remembers what it took
/// and when.
class fake_port : public port_writer
{
public:
    struct entry
    {
        feed_time at;
        std::uint64_t port = 0;
        byte_buffer data;
    };

    /// The time of the call being made, set by the test.
    feed_time clock = t0;
    /// The most bytes one write takes.
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool not_ready = false;
    bool failed = false;
    std::vector<entry> writes;
    std::size_t calls = 0;

    port_write_result write(std::uint64_t port, bytes_view data) override
    {
        ++calls;
        port_write_result result;
        if (failed)
        {
            result.failed = true;
            return result;
        }
        if (not_ready)
        {
            result.not_ready = true;
            return result;
        }
        const std::size_t taken = std::min(limit, data.size());
        entry written;
        written.at = clock;
        written.port = port;
        written.data.assign(data.begin(), data.begin() + std::ptrdiff_t(taken));
        writes.push_back(std::move(written));
        result.taken = taken;
        return result;
    }

    std::size_t total() const
    {
        std::size_t sum = 0;
        for (const entry& item : writes)
        {
            sum += item.data.size();
        }
        return sum;
    }
};

feed_batch bytes_batch(std::size_t size, std::uint8_t fill = 0x41)
{
    feed_batch batch;
    batch.data.assign(size, static_cast<byte>(fill));
    return batch;
}

feed_batch timed_batch(std::size_t size, std::chrono::nanoseconds offset)
{
    feed_batch batch = bytes_batch(size);
    batch.offset = offset;
    return batch;
}

feed_options async_options()
{
    feed_options options;
    options.port = 7;
    options.timing = feed_timing::async;
    return options;
}

feed_options rules_options()
{
    feed_options options = async_options();
    options.timing = feed_timing::rules;
    return options;
}

std::vector<feed_refusal> refusals_of(const feed_output& out)
{
    std::vector<feed_refusal> found;
    for (const feed_event& event : out.events)
    {
        if (const auto* refused = std::get_if<feed_refused_event>(&event))
        {
            found.push_back(refused->reason);
        }
    }
    return found;
}

std::optional<feed_ended_event> ended_of(const feed_output& out)
{
    for (const feed_event& event : out.events)
    {
        if (const auto* ended = std::get_if<feed_ended_event>(&event))
        {
            return *ended;
        }
    }
    return std::nullopt;
}

/// The one refusal of a call, or a failure when there was not exactly one.
feed_refusal only_refusal(const feed_output& out)
{
    const auto found = refusals_of(out);
    REQUIRE_EQ(found.size(), std::size_t{1});
    return found.front();
}

feed_time at_ms(long long milliseconds)
{
    return t0 + std::chrono::duration_cast<feed_time::duration>(
                    std::chrono::milliseconds{milliseconds});
}

/// A feed that was started at `t0`.
generator_feed started(const feed_options& options)
{
    generator_feed feed{options};
    const feed_output out = feed.start(t0);
    REQUIRE(out.credit.has_value());
    return feed;
}

} // namespace

TEST_CASE("generator feed: a well-behaved generator, end to end")
{
    fake_port port;
    generator_feed feed{async_options()};
    REQUIRE(feed.state() == feed_state::created);

    const feed_output first = feed.start(t0);
    REQUIRE(feed.state() == feed_state::running);
    REQUIRE(first.credit.has_value());
    REQUIRE_EQ(first.credit->bytes, std::uint64_t{65536});
    REQUIRE_EQ(first.credit->batches, std::uint32_t{64});
    REQUIRE(!first.wake.has_value());

    // A batch is written at once, and the credit it used is given back
    // because nothing is left in the queue.
    port.clock = at_ms(1);
    const feed_output one = feed.on_batch(bytes_batch(100), at_ms(1), port);
    REQUIRE_EQ(port.writes.size(), std::size_t{1});
    REQUIRE_EQ(port.writes[0].port, std::uint64_t{7});
    REQUIRE_EQ(port.writes[0].data.size(), std::size_t{100});
    REQUIRE_EQ(one.written, std::uint64_t{100});
    REQUIRE(one.credit.has_value());
    REQUIRE_EQ(one.credit->bytes, std::uint64_t{100});
    REQUIRE_EQ(one.credit->batches, std::uint32_t{1});
    REQUIRE(one.events.empty());
    REQUIRE(!one.wake.has_value());

    port.clock = at_ms(2);
    const feed_output two = feed.on_batch(bytes_batch(65536), at_ms(2), port);
    REQUIRE_EQ(two.written, std::uint64_t{65536});
    REQUIRE(two.credit.has_value());
    REQUIRE_EQ(two.credit->bytes, std::uint64_t{65536});

    const feed_stats stats = feed.stats();
    REQUIRE_EQ(stats.accepted_batches, std::uint64_t{2});
    REQUIRE_EQ(stats.written_batches, std::uint64_t{2});
    REQUIRE_EQ(stats.written_bytes, std::uint64_t{65636});
    REQUIRE_EQ(stats.credit_bytes, std::uint64_t{65536});
    REQUIRE_EQ(stats.credit_batches, std::uint32_t{64});
    REQUIRE_EQ(stats.refused_batches, std::uint64_t{0});
    REQUIRE_EQ(stats.late_batches, std::uint64_t{0});

    // The plugin is done, with nothing held: the feed ends at once.
    const feed_output done = feed.on_done(at_ms(3), port);
    REQUIRE(feed.state() == feed_state::ended);
    const auto ended = ended_of(done);
    REQUIRE(ended.has_value());
    REQUIRE(ended->reason == feed_end_reason::done);
    REQUIRE(!feed.next_wake().has_value());
}

TEST_CASE("generator feed: done waits for what is held")
{
    fake_port port;
    port.not_ready = true;
    generator_feed feed = started(async_options());
    feed.on_batch(bytes_batch(10), t0, port);
    feed.on_batch(bytes_batch(10), t0, port);

    const feed_output done = feed.on_done(at_ms(1), port);
    REQUIRE(feed.state() == feed_state::draining);
    REQUIRE(!ended_of(done).has_value());

    // Nothing more is taken while it drains.
    const feed_output late = feed.on_batch(bytes_batch(1), at_ms(2), port);
    REQUIRE(only_refusal(late) == feed_refusal::not_running);

    port.not_ready = false;
    port.clock = at_ms(3);
    const feed_output ready = feed.on_port_ready(at_ms(3), port);
    REQUIRE_EQ(ready.written, std::uint64_t{20});
    const auto ended = ended_of(ready);
    REQUIRE(ended.has_value());
    REQUIRE(ended->reason == feed_end_reason::done);
    REQUIRE(feed.state() == feed_state::ended);
}

TEST_CASE("generator feed: each refusal has its reason")
{
    fake_port port;
    port.not_ready = true;

    // Before it starts.
    {
        generator_feed feed{async_options()};
        const feed_output out = feed.on_batch(bytes_batch(1), t0, port);
        REQUIRE(only_refusal(out) == feed_refusal::not_running);
        REQUIRE(feed.state() == feed_state::created);
    }

    feed_options options = async_options();
    options.window_bytes = 4000;
    options.window_batches = 4;
    options.max_batch_bytes = 1000;
    options.refusals_to_end = 1000;

    {
        generator_feed feed = started(options);
        REQUIRE(only_refusal(feed.on_batch(bytes_batch(0), t0, port)) ==
                feed_refusal::empty);
        REQUIRE(only_refusal(feed.on_batch(bytes_batch(1001), t0, port)) ==
                feed_refusal::too_large);
        // A time given to an instance that did not declare "rules".
        REQUIRE(only_refusal(feed.on_batch(timed_batch(1, 1ms), t0, port)) ==
                feed_refusal::not_declared);
        REQUIRE_EQ(feed.stats().accepted_batches, std::uint64_t{0});
        REQUIRE_EQ(feed.stats().refused_batches, std::uint64_t{3});
    }

    {
        // The bytes of the credit are spent first.
        generator_feed feed = started(options);
        for (int index = 0; index < 4; ++index)
        {
            REQUIRE(refusals_of(feed.on_batch(bytes_batch(1000), t0, port))
                        .empty());
        }
        REQUIRE_EQ(feed.stats().credit_bytes, std::uint64_t{0});
        REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), t0, port)) ==
                feed_refusal::no_credit);
    }

    {
        // Enough bytes and no batches.
        options.window_batches = 2;
        generator_feed feed = started(options);
        REQUIRE(refusals_of(feed.on_batch(bytes_batch(1), t0, port)).empty());
        REQUIRE(refusals_of(feed.on_batch(bytes_batch(1), t0, port)).empty());
        REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), t0, port)) ==
                feed_refusal::no_batch_credit);
    }

    {
        generator_feed feed = started(rules_options());
        // No time on a "rules" instance.
        REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), t0, port)) ==
                feed_refusal::not_declared);
        REQUIRE(only_refusal(feed.on_batch(timed_batch(1, -1ns), t0, port)) ==
                feed_refusal::offset_out_of_range);
        REQUIRE(only_refusal(feed.on_batch(
                    timed_batch(1, std::chrono::hours{24} + 1ns), t0, port)) ==
                feed_refusal::offset_out_of_range);
        REQUIRE(refusals_of(feed.on_batch(
                                timed_batch(1, std::chrono::hours{24}), t0,
                                port))
                    .empty());
    }

    {
        // The scale cannot make an offset overflow.
        feed_options scaled = rules_options();
        scaled.time_scale = {max_feed_scale_part, 1};
        generator_feed feed = started(scaled);
        REQUIRE(only_refusal(feed.on_batch(
                    timed_batch(1, std::chrono::nanoseconds::max()), t0,
                    port)) == feed_refusal::offset_out_of_range);
        REQUIRE(only_refusal(feed.on_batch(timed_batch(1, 1h), t0, port)) ==
                feed_refusal::offset_out_of_range);
    }

    {
        generator_feed feed = started(rules_options());
        REQUIRE(refusals_of(feed.on_batch(timed_batch(1, 100ms), t0, port))
                    .empty());
        REQUIRE(only_refusal(feed.on_batch(timed_batch(1, 99ms), t0, port)) ==
                feed_refusal::out_of_order);
        // The same time again is in order.
        REQUIRE(refusals_of(feed.on_batch(timed_batch(1, 100ms), t0, port))
                    .empty());
    }
}

TEST_CASE("generator feed: enough refusals end the feed")
{
    fake_port port;
    generator_feed feed = started(async_options());
    feed.on_batch(bytes_batch(5), t0, port);

    REQUIRE(only_refusal(feed.on_batch(bytes_batch(0), t0, port)) ==
            feed_refusal::empty);
    const feed_output second = feed.on_batch(bytes_batch(0), t0, port);
    REQUIRE(!ended_of(second).has_value());

    const feed_output third = feed.on_batch(bytes_batch(0), t0, port);
    REQUIRE(only_refusal(third) == feed_refusal::empty);
    const auto ended = ended_of(third);
    REQUIRE(ended.has_value());
    REQUIRE(ended->reason == feed_end_reason::misbehaving);
    REQUIRE_EQ(ended->detail, std::string{"refused 3 batches, the last: empty"});
    REQUIRE(feed.state() == feed_state::ended);
    REQUIRE_EQ(feed.stats().refused_by_reason[std::size_t(feed_refusal::empty)],
               std::uint64_t{3});

    // It accepts nothing more, and says nothing more.
    const std::size_t before = port.writes.size();
    const feed_output after = feed.on_batch(bytes_batch(5), t0, port);
    REQUIRE(after.events.empty());
    REQUIRE_EQ(after.written, std::uint64_t{0});
    REQUIRE_EQ(port.writes.size(), before);
    REQUIRE_EQ(feed.stats().refused_batches, std::uint64_t{4});
}

TEST_CASE("generator feed: what the plugin sent is never in the detail")
{
    fake_port port;
    feed_options options = async_options();
    options.refusals_to_end = 1;
    generator_feed feed = started(options);
    feed_batch batch = bytes_batch(0);
    const std::string secret = "SECRET-TEXT";
    batch.data = to_bytes(secret);
    batch.offset = 5ms;
    const feed_output out = feed.on_batch(std::move(batch), t0, port);
    const auto ended = ended_of(out);
    REQUIRE(ended.has_value());
    REQUIRE(ended->detail.find(secret) == std::string::npos);
}

TEST_CASE("generator feed: the limits at their edge and one past it")
{
    fake_port port;
    port.not_ready = true;
    feed_options options = async_options();
    options.window_bytes = 4096;
    options.window_batches = 8;
    options.max_batch_bytes = 1024;
    options.refusals_to_end = 1000;

    generator_feed feed = started(options);
    REQUIRE(only_refusal(feed.on_batch(bytes_batch(1025), t0, port)) ==
            feed_refusal::too_large);
    for (int index = 0; index < 4; ++index)
    {
        REQUIRE(refusals_of(feed.on_batch(bytes_batch(1024), t0, port))
                    .empty());
    }
    const feed_stats full = feed.stats();
    REQUIRE_EQ(full.held_bytes, std::uint64_t{4096});
    REQUIRE_EQ(full.held_batches, std::uint32_t{4});
    REQUIRE_EQ(full.credit_bytes, std::uint64_t{0});
    REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), t0, port)) ==
            feed_refusal::no_credit);

    // One byte is enough to be refused for the bytes, and exactly the
    // credit is enough to be taken.
    port.not_ready = false;
    port.clock = at_ms(1);
    const feed_output out = feed.on_port_ready(at_ms(1), port);
    REQUIRE_EQ(out.written, std::uint64_t{4096});
    REQUIRE(out.credit.has_value());
    REQUIRE_EQ(out.credit->bytes, std::uint64_t{4096});
    REQUIRE_EQ(out.credit->batches, std::uint32_t{4});

    // Batches: eight of one byte, and the ninth is one past.
    port.not_ready = true;
    for (int index = 0; index < 8; ++index)
    {
        REQUIRE(refusals_of(feed.on_batch(bytes_batch(1), at_ms(2), port))
                    .empty());
    }
    REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), at_ms(2), port)) ==
            feed_refusal::no_batch_credit);
    REQUIRE_EQ(feed.stats().held_batches, std::uint32_t{8});
}

TEST_CASE("generator feed: options are held to their maxima")
{
    feed_options options = async_options();
    options.window_bytes = 1'000'000;
    options.window_batches = 0;
    options.max_batch_bytes = 5'000'000;
    options.refusals_to_end = 0;
    options.time_scale = {0, 2'000'000};
    generator_feed feed{options};
    REQUIRE_EQ(feed.options().window_bytes, max_feed_window);
    REQUIRE_EQ(feed.options().window_batches, std::uint32_t{1});
    REQUIRE_EQ(feed.options().max_batch_bytes, max_feed_window);
    REQUIRE_EQ(feed.options().refusals_to_end, std::uint32_t{1});
    REQUIRE_EQ(feed.options().time_scale.num, std::uint32_t{1});
    REQUIRE_EQ(feed.options().time_scale.den, std::uint32_t{1});
    const feed_output out = feed.start(t0);
    REQUIRE(out.credit.has_value());
    REQUIRE_EQ(out.credit->bytes, max_feed_window);
}

TEST_CASE("generator feed: a batch is never written early")
{
    fake_port port;
    generator_feed feed = started(rules_options());

    // Early: held until its time.
    const feed_output held =
        feed.on_batch(timed_batch(10, 100ms), t0, port);
    REQUIRE(port.writes.empty());
    REQUIRE(held.wake.has_value());
    REQUIRE(*held.wake == at_ms(100));
    REQUIRE(!held.credit.has_value());
    REQUIRE_EQ(feed.stats().held_batches, std::uint32_t{1});

    // Calling early does nothing, however often.
    port.clock = at_ms(99);
    REQUIRE_EQ(feed.on_time(at_ms(99), port).written, std::uint64_t{0});
    REQUIRE(port.writes.empty());
    REQUIRE(*feed.next_wake() == at_ms(100));

    // On time: written at the time asked for, and not late.
    port.clock = at_ms(100);
    const feed_output on_time = feed.on_time(at_ms(100), port);
    REQUIRE_EQ(on_time.written, std::uint64_t{10});
    REQUIRE(port.writes[0].at == at_ms(100));
    REQUIRE(on_time.credit.has_value());
    REQUIRE(!on_time.wake.has_value());
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{0});

    // Late: written at once, and counted.
    port.clock = at_ms(250);
    const feed_output late =
        feed.on_batch(timed_batch(10, 200ms), at_ms(250), port);
    REQUIRE_EQ(late.written, std::uint64_t{10});
    REQUIRE(port.writes[1].at == at_ms(250));
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{1});
    REQUIRE_EQ(feed.stats().max_lateness.count(),
               std::chrono::nanoseconds{50ms}.count());
}

TEST_CASE("generator feed: held batches go in order, each at its time")
{
    fake_port port;
    generator_feed feed = started(rules_options());
    feed.on_batch(bytes_batch(1), t0, port);
    REQUIRE(refusals_of(feed.on_batch(timed_batch(1, 300ms), t0, port))
                .empty());
    REQUIRE(refusals_of(feed.on_batch(timed_batch(2, 400ms), t0, port))
                .empty());
    REQUIRE(refusals_of(feed.on_batch(timed_batch(3, 400ms), t0, port))
                .empty());
    // The first of these was refused; three are held.
    REQUIRE_EQ(feed.stats().held_batches, std::uint32_t{3});

    port.clock = at_ms(300);
    feed.on_time(at_ms(300), port);
    REQUIRE_EQ(port.writes.size(), std::size_t{1});
    REQUIRE(*feed.next_wake() == at_ms(400));

    // The owner was late for the next two: both go at once, in order.
    port.clock = at_ms(450);
    feed.on_time(at_ms(450), port);
    REQUIRE_EQ(port.writes.size(), std::size_t{3});
    REQUIRE_EQ(port.writes[1].data.size(), std::size_t{2});
    REQUIRE_EQ(port.writes[2].data.size(), std::size_t{3});
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{2});
    REQUIRE(!feed.next_wake().has_value());
}

TEST_CASE("generator feed: lateness within the tolerance is not counted")
{
    fake_port port;
    feed_options options = rules_options();
    options.late_tolerance = 1ms;
    generator_feed feed = started(options);
    feed.on_batch(timed_batch(1, 10ms), at_ms(11), port);
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{0});
    feed.on_batch(timed_batch(1, 20ms), at_ms(22), port);
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{1});
}

TEST_CASE("generator feed: the time scale gives whole nanoseconds, rounded down")
{
    struct row
    {
        feed_time_scale scale;
        std::chrono::nanoseconds offset;
        std::chrono::nanoseconds due;
    };
    const std::vector<row> rows = {
        {{1, 1}, 100ms, 100ms},   // the original timing
        {{1, 2}, 100ms, 50ms},    // time_scale 0.5: twice as fast
        {{10, 1}, 5ms, 50ms},     // time_scale 10: a tenth of the speed
        {{3, 7}, 10ns, 4ns},      // 30 / 7 rounded down
        {{1, 3}, 2ns, 0ns},       // 2 / 3 rounded down
        {{999'999, 1'000'000}, 1'000'000'000ns, 999'999'000ns},
    };
    for (const row& item : rows)
    {
        fake_port port;
        feed_options options = rules_options();
        options.time_scale = item.scale;
        generator_feed feed = started(options);
        // The batch is due `item.due` after the start.  When that is the
        // start itself it is written at once; otherwise it is held.
        const feed_output out =
            feed.on_batch(timed_batch(4, item.offset), t0, port);
        if (item.due.count() == 0)
        {
            REQUIRE_EQ(out.written, std::uint64_t{4});
            continue;
        }
        REQUIRE(port.writes.empty());
        REQUIRE(out.wake.has_value());
        REQUIRE(*out.wake == t0 + std::chrono::duration_cast<feed_time::duration>(
                                      item.due));
        // One tick before: nothing.  At the time: written, not late.
        port.clock = *out.wake - feed_time::duration{1};
        REQUIRE_EQ(feed.on_time(port.clock, port).written, std::uint64_t{0});
        port.clock = *out.wake;
        REQUIRE_EQ(feed.on_time(port.clock, port).written, std::uint64_t{4});
        REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{0});
    }
}

TEST_CASE("generator feed: a late port makes a batch late")
{
    fake_port port;
    generator_feed feed = started(rules_options());
    port.not_ready = true;
    port.clock = at_ms(10);
    feed.on_batch(timed_batch(5, 10ms), at_ms(10), port);
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{0});
    port.not_ready = false;
    port.clock = at_ms(40);
    feed.on_port_ready(at_ms(40), port);
    REQUIRE_EQ(feed.stats().written_bytes, std::uint64_t{5});
    REQUIRE_EQ(feed.stats().late_batches, std::uint64_t{1});
    REQUIRE_EQ(feed.stats().max_lateness.count(),
               std::chrono::nanoseconds{30ms}.count());
}

TEST_CASE("generator feed: a slow port holds the credit back")
{
    fake_port port;
    feed_options options = async_options();
    options.window_bytes = 4000;
    options.window_batches = 8;
    options.grant_threshold = 1000;
    options.retry_after = 10ms;
    generator_feed feed = started(options);

    // The port takes 100 bytes of the 1000 at a time.  No credit comes
    // back until the whole 1000 has gone.
    port.limit = 100;
    port.clock = t0;
    feed_output out = feed.on_batch(bytes_batch(1000), t0, port);
    REQUIRE_EQ(out.written, std::uint64_t{100});
    REQUIRE(!out.credit.has_value());
    REQUIRE(out.wake.has_value());
    REQUIRE(*out.wake == at_ms(10));
    REQUIRE_EQ(feed.stats().credit_bytes, std::uint64_t{3000});

    // Before the retry time the port is not offered anything.
    const std::size_t calls = port.calls;
    feed.on_time(at_ms(5), port);
    REQUIRE_EQ(port.calls, calls);

    for (int step = 1; step < 9; ++step)
    {
        port.clock = at_ms(10LL * step);
        out = feed.on_time(port.clock, port);
        REQUIRE_EQ(out.written, std::uint64_t{100});
        REQUIRE(!out.credit.has_value());
    }
    port.clock = at_ms(90);
    out = feed.on_time(port.clock, port);
    REQUIRE_EQ(out.written, std::uint64_t{100});
    REQUIRE(out.credit.has_value());
    REQUIRE_EQ(out.credit->bytes, std::uint64_t{1000});
    REQUIRE_EQ(out.credit->batches, std::uint32_t{1});
    REQUIRE(!out.wake.has_value());
    REQUIRE_EQ(feed.stats().credit_bytes, std::uint64_t{4000});
}

TEST_CASE("generator feed: a port that is not ready stops the credit")
{
    fake_port port;
    port.not_ready = true;
    feed_options options = async_options();
    options.window_bytes = 4000;
    options.window_batches = 16;
    generator_feed feed = started(options);

    // The plugin sends all the credit it has, and the host holds just that.
    for (int index = 0; index < 4; ++index)
    {
        const feed_output out = feed.on_batch(bytes_batch(1000), t0, port);
        REQUIRE(refusals_of(out).empty());
        REQUIRE(!out.credit.has_value());
    }
    REQUIRE_EQ(feed.stats().held_bytes, std::uint64_t{4000});
    REQUIRE(only_refusal(feed.on_batch(bytes_batch(1), t0, port)) ==
            feed_refusal::no_credit);

    // Time passes and nothing leaves, so nothing is given back.
    for (int step = 1; step < 50; ++step)
    {
        const feed_output out = feed.on_time(at_ms(10LL * step), port);
        REQUIRE(!out.credit.has_value());
        REQUIRE_EQ(out.written, std::uint64_t{0});
    }
    REQUIRE_EQ(feed.stats().held_bytes, std::uint64_t{4000});
    REQUIRE(feed.next_wake().has_value());

    // The port is ready again.  Everything goes, and all the credit is given
    // back at once.
    port.not_ready = false;
    port.clock = at_ms(600);
    const feed_output ready = feed.on_port_ready(at_ms(600), port);
    REQUIRE_EQ(ready.written, std::uint64_t{4000});
    REQUIRE(ready.credit.has_value());
    REQUIRE_EQ(ready.credit->bytes, std::uint64_t{4000});
    REQUIRE_EQ(ready.credit->batches, std::uint32_t{4});
    REQUIRE_EQ(feed.stats().held_bytes, std::uint64_t{0});
}

TEST_CASE("generator feed: credit is given when it would run out")
{
    fake_port port;
    feed_options options = async_options();
    options.window_bytes = 100;
    options.window_batches = 8;
    options.grant_threshold = 100;
    generator_feed feed = started(options);

    // Under the threshold, with a queue behind: held back.
    port.limit = 10;
    feed.on_batch(bytes_batch(40), t0, port);
    feed.on_batch(bytes_batch(40), t0, port);
    REQUIRE_EQ(feed.stats().owed_bytes, std::uint64_t{10});

    // Credit of 20 bytes is left, so nothing has run out yet.  A third batch
    // takes all of it, and the 10 bytes owed are given back at once, below
    // the threshold, so that the plugin is never left without credit.
    const feed_output third = feed.on_batch(bytes_batch(20), t0, port);
    REQUIRE(third.credit.has_value());
    REQUIRE_EQ(third.credit->bytes, std::uint64_t{10});
    REQUIRE_EQ(third.credit->batches, std::uint32_t{0});
    REQUIRE_EQ(feed.stats().credit_bytes, std::uint64_t{10});
    REQUIRE_EQ(feed.stats().owed_bytes, std::uint64_t{0});
}

TEST_CASE("generator feed: a port that is gone ends the feed")
{
    fake_port port;
    generator_feed feed = started(async_options());
    port.failed = true;
    const feed_output out = feed.on_batch(bytes_batch(10), t0, port);
    const auto ended = ended_of(out);
    REQUIRE(ended.has_value());
    REQUIRE(ended->reason == feed_end_reason::port_failed);
    REQUIRE(feed.state() == feed_state::ended);
    REQUIRE_EQ(feed.stats().held_bytes, std::uint64_t{0});
    REQUIRE(!feed.next_wake().has_value());
}

TEST_CASE("generator feed: a port that takes more than it was offered")
{
    struct greedy : port_writer
    {
        port_write_result write(std::uint64_t, bytes_view) override
        {
            port_write_result result;
            result.taken = 1'000'000;
            return result;
        }
    };
    greedy port;
    generator_feed feed = started(async_options());
    const feed_output out = feed.on_batch(bytes_batch(10), t0, port);
    REQUIRE_EQ(out.written, std::uint64_t{10});
    REQUIRE_EQ(feed.stats().written_bytes, std::uint64_t{10});
}

TEST_CASE("generator feed: stop and a vanished plugin, in every state")
{
    for (const bool plugin_gone : {false, true})
    {
        const auto end = [plugin_gone](generator_feed& feed)
        {
            return plugin_gone ? feed.on_plugin_gone() : feed.stop();
        };
        const feed_end_reason expected = plugin_gone
                                             ? feed_end_reason::plugin_gone
                                             : feed_end_reason::stopped;
        const auto check_ended = [&](const feed_output& out)
        {
            const auto ended = ended_of(out);
            REQUIRE(ended.has_value());
            REQUIRE(ended->reason == expected);
        };

        // Before it starts.
        {
            fake_port port;
            generator_feed feed{async_options()};
            check_ended(end(feed));
            REQUIRE(feed.state() == feed_state::ended);
            REQUIRE(!feed.start(t0).credit.has_value());
            REQUIRE(feed.state() == feed_state::ended);
            REQUIRE(refusals_of(feed.on_batch(bytes_batch(1), t0, port))
                        .empty());
            REQUIRE(port.writes.empty());
        }

        // Running with nothing held.
        {
            fake_port port;
            generator_feed feed = started(async_options());
            check_ended(end(feed));
            const feed_output out = feed.on_batch(bytes_batch(1), t0, port);
            REQUIRE(out.events.empty());
            REQUIRE(port.writes.empty());
            REQUIRE_EQ(feed.stats().refused_batches, std::uint64_t{1});
        }

        // Running with batches held: they are dropped, never written.
        {
            fake_port port;
            generator_feed feed = started(rules_options());
            feed.on_batch(timed_batch(10, 100ms), t0, port);
            feed.on_batch(timed_batch(10, 200ms), t0, port);
            check_ended(end(feed));
            REQUIRE_EQ(feed.stats().held_batches, std::uint32_t{0});
            REQUIRE_EQ(feed.stats().held_bytes, std::uint64_t{0});
            REQUIRE(!feed.next_wake().has_value());
            port.clock = at_ms(300);
            const feed_output out = feed.on_time(at_ms(300), port);
            REQUIRE_EQ(out.written, std::uint64_t{0});
            REQUIRE(out.events.empty());
            REQUIRE(port.writes.empty());
        }

        // In the middle of a batch.
        {
            fake_port port;
            port.limit = 4;
            generator_feed feed = started(async_options());
            feed.on_batch(bytes_batch(10), t0, port);
            REQUIRE_EQ(port.total(), std::size_t{4});
            check_ended(end(feed));
            port.clock = at_ms(100);
            feed.on_time(at_ms(100), port);
            feed.on_port_ready(at_ms(100), port);
            REQUIRE_EQ(port.total(), std::size_t{4});
        }

        // Draining.
        {
            fake_port port;
            port.not_ready = true;
            generator_feed feed = started(async_options());
            feed.on_batch(bytes_batch(10), t0, port);
            feed.on_done(at_ms(1), port);
            REQUIRE(feed.state() == feed_state::draining);
            check_ended(end(feed));
            port.not_ready = false;
            feed.on_port_ready(at_ms(2), port);
            REQUIRE(port.writes.empty());
        }

        // Already over: nothing more is said.
        {
            fake_port port;
            generator_feed feed = started(async_options());
            check_ended(end(feed));
            const feed_output again = end(feed);
            REQUIRE(again.events.empty());
            REQUIRE(feed.stop().events.empty());
            REQUIRE(feed.on_plugin_gone().events.empty());
            REQUIRE(feed.on_done(t0, port).events.empty());
        }
    }
}

TEST_CASE("generator feed: done before start and done twice are refusals")
{
    fake_port port;
    generator_feed feed{async_options()};
    REQUIRE(only_refusal(feed.on_done(t0, port)) == feed_refusal::not_running);

    port.not_ready = true;
    generator_feed other = started(async_options());
    other.on_batch(bytes_batch(1), t0, port);
    REQUIRE(refusals_of(other.on_done(t0, port)).empty());
    REQUIRE(only_refusal(other.on_done(t0, port)) ==
            feed_refusal::not_running);
}

namespace
{

/// A small generator of numbers that is the same on every machine.
class lcg
{
public:
    explicit lcg(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next()
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }

    std::uint64_t below(std::uint64_t limit) { return next() % limit; }

private:
    std::uint64_t state_;
};

struct accepted_batch
{
    std::size_t first = 0;
    std::size_t size = 0;
    std::optional<feed_time> due;
};

/// One run of random calls.  Every call is followed by the checks that must
/// hold in any state.
void random_run(std::uint64_t seed)
{
    lcg random{seed * 7919 + 13};
    const bool rules = (seed % 2) == 1;
    feed_options options = rules ? rules_options() : async_options();
    options.window_bytes = 64 + random.below(2000);
    options.window_batches = static_cast<std::uint32_t>(1 + random.below(12));
    options.max_batch_bytes = 1 + random.below(options.window_bytes);
    options.grant_threshold = 1 + random.below(options.window_bytes);
    options.refusals_to_end = static_cast<std::uint32_t>(2 + random.below(40));
    options.time_scale = {static_cast<std::uint32_t>(1 + random.below(3)),
                          static_cast<std::uint32_t>(1 + random.below(3))};
    options.retry_after = std::chrono::milliseconds{1 + random.below(15)};
    generator_feed feed{options};
    fake_port port;

    feed_time now = t0;
    std::chrono::nanoseconds last_offset{0};
    std::vector<byte> expected;
    std::vector<accepted_batch> accepted;
    std::size_t ended_events = 0;
    bool was_ended = false;
    std::uint64_t counter = 0;
    std::uint64_t window_bytes = options.window_bytes;
    feed_time run_start = t0;
    bool started_once = false;
    if (seed % 5 != 0)
    {
        feed.start(t0);
        started_once = true;
    }

    for (int step = 0; step < 300; ++step)
    {
        now += std::chrono::duration_cast<feed_time::duration>(
            std::chrono::microseconds{random.below(20'000)});
        port.clock = now;
        port.not_ready = random.below(4) == 0;
        port.limit = random.below(3) == 0
                         ? static_cast<std::size_t>(1 + random.below(300))
                         : std::numeric_limits<std::size_t>::max();
        port.failed = random.below(400) == 0;

        feed_output out;
        const std::uint64_t pick = random.below(100);
        const std::uint64_t accepted_before = feed.stats().accepted_batches;
        std::optional<feed_time> due;
        std::size_t size = 0;
        if (pick < 4)
        {
            out = feed.start(now);
            if (feed.state() == feed_state::running && !started_once)
            {
                started_once = true;
                run_start = now;
            }
        }
        else if (pick < 60)
        {
            // Mostly good batches, some that are not.
            size = random.below(10) == 0
                       ? static_cast<std::size_t>(random.below(5000))
                       : static_cast<std::size_t>(1 + random.below(200));
            feed_batch batch;
            for (std::size_t index = 0; index < size; ++index)
            {
                batch.data.push_back(static_cast<byte>(counter++ & 0xff));
            }
            if (rules && random.below(12) != 0)
            {
                std::int64_t delta =
                    static_cast<std::int64_t>(random.below(30'000'000));
                if (random.below(10) == 0)
                {
                    delta = -delta;
                }
                last_offset += std::chrono::nanoseconds{delta};
                last_offset = std::max(last_offset, std::chrono::nanoseconds{0});
                batch.offset = last_offset;
            }
            else if (!rules && random.below(12) == 0)
            {
                batch.offset = 1ms;
            }
            if (batch.offset.has_value() && rules)
            {
                const std::uint64_t scaled =
                    static_cast<std::uint64_t>(
                        std::max<std::int64_t>(batch.offset->count(), 0)) *
                    options.time_scale.num / options.time_scale.den;
                due = run_start + std::chrono::duration_cast<feed_time::duration>(
                               std::chrono::nanoseconds{
                                   static_cast<std::int64_t>(scaled)});
            }
            const byte_buffer copy = batch.data;
            out = feed.on_batch(std::move(batch), now, port);
            if (feed.stats().accepted_batches > accepted_before)
            {
                accepted_batch item;
                item.first = expected.size();
                item.size = copy.size();
                item.due = due;
                accepted.push_back(item);
                expected.insert(expected.end(), copy.begin(), copy.end());
            }
        }
        else if (pick < 80)
        {
            out = feed.on_time(now, port);
        }
        else if (pick < 88)
        {
            out = feed.on_port_ready(now, port);
        }
        else if (pick < 93)
        {
            out = feed.on_done(now, port);
        }
        else if (pick < 94)
        {
            out = feed.stop();
        }
        else if (pick < 95)
        {
            out = feed.on_plugin_gone();
        }
        else
        {
            // Time passes to the time the feed asked for.
            if (feed.next_wake().has_value() && *feed.next_wake() > now)
            {
                now = *feed.next_wake();
                port.clock = now;
            }
            out = feed.on_time(now, port);
        }

        // What must hold after any call.
        const feed_stats stats = feed.stats();
        for (const feed_event& event : out.events)
        {
            if (std::holds_alternative<feed_ended_event>(event))
            {
                ++ended_events;
            }
        }
        REQUIRE(ended_events <= 1);
        if (was_ended)
        {
            REQUIRE(feed.state() == feed_state::ended);
            REQUIRE(out.events.empty());
            REQUIRE_EQ(out.written, std::uint64_t{0});
            REQUIRE(!out.credit.has_value());
        }
        was_ended = feed.state() == feed_state::ended;
        REQUIRE((ended_events == 1) == was_ended);
        if (out.credit.has_value())
        {
            REQUIRE(out.credit->bytes <= window_bytes);
            REQUIRE(out.credit->batches <= options.window_batches);
        }
        REQUIRE(stats.held_batches <= options.window_batches);
        REQUIRE(stats.held_bytes <= window_bytes);
        if (feed.state() == feed_state::running ||
            feed.state() == feed_state::draining)
        {
            REQUIRE_EQ(stats.credit_bytes + stats.held_bytes + stats.owed_bytes,
                       window_bytes);
            REQUIRE_EQ(std::uint64_t{stats.credit_batches} +
                           stats.held_batches + stats.owed_batches,
                       std::uint64_t{options.window_batches});
        }
        if (feed.state() == feed_state::ended)
        {
            REQUIRE_EQ(stats.held_bytes, std::uint64_t{0});
            REQUIRE_EQ(stats.held_batches, std::uint32_t{0});
            REQUIRE(!feed.next_wake().has_value());
        }
        REQUIRE(stats.written_bytes <= stats.accepted_bytes);
        REQUIRE(stats.refused_batches >= refusals_of(out).size());
    }

    // What reached the port is the front of what was accepted, in order.
    std::size_t position = 0;
    for (const auto& entry : port.writes)
    {
        REQUIRE(position + entry.data.size() <= expected.size());
        REQUIRE(std::equal(entry.data.begin(), entry.data.end(),
                           expected.begin() + std::ptrdiff_t(position)));
        // A write is part of one batch, and never before its time.
        const auto found = std::find_if(
            accepted.begin(), accepted.end(), [&](const accepted_batch& item)
            { return position >= item.first && position < item.first + item.size; });
        REQUIRE(found != accepted.end());
        REQUIRE(position + entry.data.size() <= found->first + found->size);
        if (found->due.has_value())
        {
            REQUIRE(entry.at >= *found->due);
        }
        position += entry.data.size();
    }
    REQUIRE_EQ(std::uint64_t{position}, feed.stats().written_bytes);
}

} // namespace

TEST_CASE("generator feed: random sequences never leave it undefined")
{
    for (std::uint64_t seed = 1; seed <= 300; ++seed)
    {
        random_run(seed);
    }
}
