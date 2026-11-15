//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a plugin that should be running is started, started again after it
// failed, and given up on.  docs/plugins.md, section 5.5, is the design:
// the waits after each failure, the count of failures in a window, the
// reset after a long run and the quarantine.
//
// A `plugin_run` does no I/O and has no clock, no timer and no thread.  Its
// owner tells it what happened and the time, and it answers with the state
// and with what to do: start the process now, or call again at a time.  So
// every sequence of failures can be tested from memory, and the same calls
// with the same seed always give the same result.
//
// It covers the states of section 5.1 from `enabled` on.  Whether a plugin
// may run at all (verified, trusted, licensed, allowed by policy) is decided
// before a `plugin_run` is told to start.  `degraded` is not a state here:
// it is a mark on a plugin that is `ready`, and its owner keeps it.
//
// One thread at a time may use a `plugin_run`.
//

#pragma once

#include <vss/core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace vss::plugin
{

/// A time for a `plugin_run`: any clock that only goes forward.  The owner
/// passes the same clock to every call.
using run_time = std::chrono::steady_clock::time_point;

struct restart_options
{
    /// The wait after the first, second, ... failure in the window.  One
    /// failure more than there are waits quarantines the plugin.
    /// A wait below nothing is taken as nothing.
    std::vector<std::chrono::milliseconds> waits{
        std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000},
        std::chrono::milliseconds{4000}, std::chrono::milliseconds{8000}};
    /// A failure counts for this long.  One that is exactly this old no
    /// longer counts.
    std::chrono::milliseconds window{std::chrono::minutes{10}};
    /// A run that was `ready` for at least this long forgets the failures
    /// before it.
    std::chrono::milliseconds reset_after_ready{std::chrono::minutes{5}};
    /// Each wait is made longer by a random part of up to this many
    /// percent of itself, so that plugins that failed together do not
    /// start together.  At most 100.
    unsigned jitter_percent = 20;
};

enum class run_state
{
    /// Not running, and not meant to be.
    stopped,
    /// The process is being started, or runs and has not said "hello".
    starting,
    /// The handshake is done.
    ready,
    /// The host asked the plugin to stop and waits for it to be gone.
    stopping,
    /// The plugin failed; the next start waits for a time.
    backing_off,
    /// The plugin failed too often.  It is not started again until the
    /// user asks.
    quarantined
};

/// What a call asks of the owner.
struct run_step
{
    /// The state after the call.
    run_state state = run_state::stopped;
    /// Start the plugin's process now.  Only ever set with `starting`.
    bool start_now = false;
    /// Stop the plugin now: tell its session to shut down, and kill the
    /// process when that is over.  Only ever set with `stopping`.
    bool stop_now = false;
    /// Call `on_time` at this time.  Set exactly when the state is
    /// `backing_off`.
    std::optional<run_time> wake_at;
};

class plugin_run
{
public:
    /// `seed` chooses the random parts of the waits.  The same seed gives
    /// the same waits.
    explicit plugin_run(restart_options options = {}, std::uint64_t seed = 0);

    run_state state() const noexcept;

    /// The user, or the agent as it starts, wants the plugin running.
    ///
    /// - From `stopped`: start.
    /// - From `backing_off`: start now, without waiting for the time.  The
    ///   failures so far still count.
    /// - From `quarantined`: start, with no failures counted.
    /// - Otherwise nothing changes.
    run_step start(run_time now);

    /// The plugin said a "hello" that was accepted.  Only means something
    /// while `starting`.
    run_step on_ready(run_time now);

    /// The plugin is gone, or must be treated as gone, and the host had not
    /// asked: it could not be started, it exited, it crashed, its session
    /// closed with a reason.  The owner has killed what was left of it.
    ///
    /// While `starting` or `ready` this is a failure: the state becomes
    /// `backing_off`, or `quarantined` when it is one failure too many.
    /// While `stopping` it ends the stop, and the state is `stopped`; it
    /// is not counted and `reason` is not kept.
    /// Otherwise nothing changes.
    run_step on_failed(error_code reason, run_time now);

    /// The user, a policy or the agent's exit wants the plugin stopped.
    ///
    /// - From `starting` or `ready`: `stopping`, and the owner stops it.
    /// - From `backing_off` or `quarantined`: `stopped` at once.  A plugin
    ///   stopped from quarantine starts with no failures counted and no
    ///   last failure; one stopped while backing off keeps both.
    /// - Otherwise nothing changes.
    run_step stop(run_time now);

    /// The stop the host asked for is over and the process is gone.  Only
    /// means something while `stopping`.
    run_step on_stopped(run_time now);

    /// The time passed.  While `backing_off`, and at or after the time the
    /// last step gave, the plugin is started.  Otherwise nothing changes,
    /// so a call that comes early or late does no harm.
    run_step on_time(run_time now);

    /// How many failures count at `now`.
    std::size_t failures(run_time now) const noexcept;

    /// How many failures in the window quarantine the plugin: one more
    /// than there are waits.
    std::size_t failure_limit() const noexcept;

    /// Why the plugin last failed.  Success when it never has, and after a
    /// start or a stop from quarantine.
    error_code last_failure() const noexcept;

    /// When the next start is due.  Set exactly while `backing_off`.
    std::optional<run_time> next_start() const noexcept;

private:
    run_step step(bool start_now = false, bool stop_now = false) const;
    void forget_old(run_time now);
    std::chrono::milliseconds jitter(std::chrono::milliseconds wait);

    restart_options options_;
    std::uint64_t random_;
    run_state state_ = run_state::stopped;
    /// The times of the failures that may still count, oldest first.
    std::deque<run_time> failures_;
    std::optional<run_time> ready_since_;
    std::optional<run_time> next_start_;
    error_code last_failure_;
};

} // namespace vss::plugin
