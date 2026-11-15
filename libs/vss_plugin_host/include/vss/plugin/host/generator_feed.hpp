//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's side of a generator: the rules by which a plugin that produces
// bytes may send them, and how they reach a port.  docs/plugins.md, section
// 7.4 (the credit flow), 6.4 (the timing class), 7.9 (limits) and 19.4
// (replay: the time scale, and "never early").
//
// A feed does no I/O and has no clock and no thread, as `host_session` does
// not.  Its owner gives it each batch of bytes the plugin sent and the time,
// and it answers with what happened and what it wants next.  The feed never
// touches a port: it offers bytes to the `port_writer` the owner passes in,
// and the owner says how many the port took.  So every rule can be tested
// from memory.
//
// The credit is the whole of the flow control.  The plugin has a window of
// credit, in bytes and in batches.  Accepting a batch spends credit, and
// credit is given back only when the bytes have left for the port, so the
// credit left, the bytes held and the bytes owed back always add up to the
// window.  A plugin can never make the host hold more than the window,
// whatever it sends and however slow the port is.
//
// Everything a plugin sends is untrusted.  A batch the plugin may not send
// is refused and counted; enough refusals end the feed with a reason, and a
// feed that has ended accepts nothing more.  The text of an end never repeats
// what the plugin sent.
//
// One thread at a time may use a feed.  Which thread is the owner's business.
//

#pragma once

#include <vss/core/bytes.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

/// A time for a feed: any clock that only goes forward.  The owner passes
/// the same clock to every call.  It is the type of `session_time`.
using feed_time = std::chrono::steady_clock::time_point;

/// How a generator's instance declared its timing in the manifest (section
/// 6.4).
enum class feed_timing
{
    /// "async": the plugin's bytes go out when the port takes them, and the
    /// feed does not pace them.  A batch that names a time is refused with
    /// `not_declared`.
    async,
    /// "rules": every batch names the time it is due, as the recording's
    /// offset, and the feed holds it until then.  A batch without one would
    /// be timed by the plugin, and is refused with `not_declared`.
    rules
};

/// `time_scale` of a replay (`docs/response-timing.md`) as a fraction, so
/// that the schedule is whole nanoseconds, rounded down, on every machine:
/// a recorded offset becomes `offset * num / den`.  1/1 is the original
/// timing, 1/2 is twice as fast (`time_scale` 0.5) and 10/1 a tenth of the
/// speed.  Each part is 1 to `max_feed_scale_part`; a value outside that is
/// taken as 1.
struct feed_time_scale
{
    std::uint32_t num = 1;
    std::uint32_t den = 1;
};

inline constexpr std::uint32_t max_feed_scale_part = 1'000'000;

/// The most credit a feed gives a plugin at once, in bytes: the window a
/// generator starts with (`max_generator_credit` of the plugin API).
inline constexpr std::uint64_t max_feed_window = std::uint64_t{64} * 1024;

struct feed_options
{
    /// The endpoint the bytes go to, as `generator.start` gave it.
    std::uint64_t port = 0;
    feed_timing timing = feed_timing::async;
    feed_time_scale time_scale;

    /// The window of credit in bytes, 1 to `max_feed_window`.
    std::uint64_t window_bytes = max_feed_window;
    /// The window in batches: how many batches may be held or waiting for
    /// the port at once, and so the size of the holding queue.  At least 1.
    std::uint32_t window_batches = 64;
    /// The most bytes in one batch (`max_generator_write`).  It is at most
    /// the window.
    std::uint64_t max_batch_bytes = max_feed_window;
    /// Credit given back is held until at least this many bytes are owed, or
    /// until the plugin would otherwise be left without credit or the queue
    /// is empty.  So a slow trickle is not a message per write.
    std::uint64_t grant_threshold = 16 * 1024;
    /// How many refused batches end the feed as `misbehaving`.  The first
    /// is answered `busy` by the owner and the next ones are counted.
    std::uint32_t refusals_to_end = 3;
    /// How far a batch's scaled offset may be from the start, in `rules`
    /// timing.  A larger one is refused with `offset_out_of_range`.
    std::chrono::nanoseconds max_run = std::chrono::hours{24};
    /// A batch counts as late when it is written more than this after its
    /// time.
    std::chrono::nanoseconds late_tolerance{0};
    /// How long to wait before offering bytes again to a port that said it
    /// was not ready, when the owner does not call `on_port_ready` sooner.
    std::chrono::milliseconds retry_after{10};
};

/// One `generator.write` as the feed sees it.
struct feed_batch
{
    /// The blob of `generator.write`.
    byte_buffer data;
    /// `rules` timing only: the time of the batch in the recording, from the
    /// start of the run.  The feed applies the time scale to it.
    std::optional<std::chrono::nanoseconds> offset;
};

enum class feed_state
{
    /// Made; `start` has not been called.
    created,
    /// Taking batches.
    running,
    /// The plugin said it is done.  Nothing more is taken, and what is held
    /// is still written.
    draining,
    /// Over.  `feed_ended_event` said why.
    ended
};

/// Why a batch was refused.  A refusal is counted, and enough of them end
/// the feed.
enum class feed_refusal
{
    /// The feed was not started, is draining, or has ended.
    not_running,
    /// No bytes.
    empty,
    /// More than `max_batch_bytes`.
    too_large,
    /// A time given to an `async` instance, or none to a `rules` one.
    not_declared,
    /// The scaled offset is negative, or past `max_run`.
    offset_out_of_range,
    /// The time is earlier than that of the batch before it.
    out_of_order,
    /// More bytes than the credit left.
    no_credit,
    /// The credit in batches is spent.
    no_batch_credit
};

inline constexpr std::size_t feed_refusal_count = 8;

/// Why a feed ended.
enum class feed_end_reason
{
    /// The host stopped it.
    stopped,
    /// The plugin said it was done, and everything it sent was written.
    done,
    /// The plugin broke the rules too often.  The owner ends the plugin as
    /// `misbehaving`.
    misbehaving,
    /// The plugin's process or channel went away.
    plugin_gone,
    /// The port reported that it can take no more bytes.
    port_failed
};

/// A batch was refused.  `count` is how many have been refused so far.
struct feed_refused_event
{
    feed_refusal reason = feed_refusal::not_running;
    std::uint32_t count = 0;
};

/// The feed is over.  It is the last event of a feed, and it comes exactly
/// once.
struct feed_ended_event
{
    feed_end_reason reason = feed_end_reason::stopped;
    /// One line for the log.  It never repeats what the plugin sent.
    std::string detail;
};

using feed_event = std::variant<feed_refused_event, feed_ended_event>;

/// Credit to give the plugin: the owner turns it into `generator.credit`.
struct feed_credit
{
    std::uint64_t bytes = 0;
    std::uint32_t batches = 0;
};

/// What a call to a feed asks of its owner.
struct feed_output
{
    /// Credit to send to the plugin.  Never more than the window.
    std::optional<feed_credit> credit;
    /// What happened, in the order it happened.
    std::vector<feed_event> events;
    /// Bytes this call offered to the port and the port took.
    std::uint64_t written = 0;
    /// When set: call `on_time` at this time.  It is the same as
    /// `next_wake()` after the call.
    std::optional<feed_time> wake;
};

/// What the port did with an offer of bytes.
struct port_write_result
{
    /// How many of the bytes the port took, from the first.  At most the
    /// bytes offered; more is taken as all.
    std::size_t taken = 0;
    /// The port cannot take bytes now.  It is taken as no bytes and a wait
    /// until `on_port_ready` or `retry_after`.
    bool not_ready = false;
    /// The port is gone and will take none.  The feed ends.
    bool failed = false;
};

/// What the owner implements: where the feed's bytes go.  The feed calls it
/// from inside a call of its own, on the owner's thread, and never keeps it.
/// The owner makes a copy of the bytes if it needs them after it returns.
class port_writer
{
public:
    port_writer() = default;
    port_writer(const port_writer&) = delete;
    port_writer& operator=(const port_writer&) = delete;
    virtual ~port_writer() = default;

    /// Offer `data` to `port`.
    virtual port_write_result write(std::uint64_t port, bytes_view data) = 0;
};

/// Counters of a feed, for the owner's status and for tests.
struct feed_stats
{
    std::uint64_t accepted_batches = 0;
    std::uint64_t accepted_bytes = 0;
    std::uint64_t written_batches = 0;
    std::uint64_t written_bytes = 0;
    /// Batches that were refused, in all and by reason (indexed by
    /// `feed_refusal`).
    std::uint64_t refused_batches = 0;
    std::array<std::uint64_t, feed_refusal_count> refused_by_reason{};
    /// Batches written after their time, and the most they were late by.
    std::uint64_t late_batches = 0;
    std::chrono::nanoseconds max_lateness{0};
    /// Held now: batches and bytes not yet written.
    std::uint32_t held_batches = 0;
    std::uint64_t held_bytes = 0;
    /// Credit the plugin has now.
    std::uint64_t credit_bytes = 0;
    std::uint32_t credit_batches = 0;
    /// Written, and not yet given back as credit.
    std::uint64_t owed_bytes = 0;
    std::uint32_t owed_batches = 0;
};

const char* to_string(feed_refusal reason) noexcept;
const char* to_string(feed_end_reason reason) noexcept;

class generator_feed
{
public:
    explicit generator_feed(feed_options options);

    feed_state state() const noexcept { return state_; }
    const feed_options& options() const noexcept { return options_; }
    feed_stats stats() const noexcept;

    /// Starts the feed at `now`, which is time 0 of a `rules` run, and
    /// gives the plugin its first credit.  Does nothing when the feed is not
    /// `created`.
    feed_output start(feed_time now);

    /// The plugin sent a batch.  A refused batch is in the output's events
    /// and is not kept.  An accepted one is written at once if it is due and
    /// the port takes it, and held otherwise.
    feed_output on_batch(feed_batch batch, feed_time now, port_writer& port);

    /// Time has passed.  The owner calls this at `next_wake()`, or more
    /// often; calling it early does nothing.
    feed_output on_time(feed_time now, port_writer& port);

    /// A port that was not ready can take bytes again.
    feed_output on_port_ready(feed_time now, port_writer& port);

    /// The plugin said `generator.done`.  What is held is still written,
    /// and the feed ends as `done` when it has been.  Counted as a refusal
    /// when the feed is not running.
    feed_output on_done(feed_time now, port_writer& port);

    /// The host stops the feed (`generator.stop`).  What is held is
    /// dropped.  Nothing is written and no credit is given.
    feed_output stop();

    /// The plugin's process or channel went away, mid-batch or not.  What
    /// is held is dropped.
    feed_output on_plugin_gone();

    /// The next time `on_time` has something to do: the time of the batch
    /// at the head of the queue, or the time to try a port again.  Nothing
    /// when the queue is empty or the feed is over.
    std::optional<feed_time> next_wake() const noexcept { return wake_; }

private:
    struct held_batch
    {
        byte_buffer data;
        /// How many bytes of `data` have been written.
        std::size_t written = 0;
        std::optional<feed_time> due;
        /// Lateness has been counted, which happens when the first byte
        /// leaves.
        bool counted = false;
    };

    void refuse(feed_refusal reason, feed_output& out);
    void pump(feed_time now, port_writer& port, feed_output& out);
    void finish(feed_end_reason reason, std::string detail, feed_output& out);
    void grant(feed_output& out);
    std::optional<feed_refusal> schedule(const feed_batch& batch,
                                         std::optional<feed_time>& due) const;

    feed_options options_;
    feed_state state_ = feed_state::created;
    feed_time start_{};
    std::deque<held_batch> queue_;
    std::optional<feed_time> last_due_;
    std::optional<feed_time> wake_;
    /// A port that said not ready is not offered bytes before this time or
    /// `on_port_ready`.
    std::optional<feed_time> retry_at_;
    feed_stats stats_;
};

} // namespace vss::plugin::host
