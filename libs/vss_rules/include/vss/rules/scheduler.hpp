//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One rule set running at one place: the matcher, the actions it planned
// and the timer that carries them out when they are due.  It is shaped like
// the link's delay line: one timer on a strand of its own, handlers that
// keep only a weak reference, and a close that waits for an action under
// way.
//
// The scheduler is put on an endpoint as its read stage, so it is shown
// every chunk on the endpoint's strand before the chunk is routed.  It
// matches there, under a lock nothing holds for long, and does no I/O
// there: what a rule does is done later, on the scheduler's own strand,
// through the `act` hook the engine gives it.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>
#include <vss/rules/compile.hpp>
#include <vss/rules/machine.hpp>
#include <vss/rules/model.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace vss::rules {

/// The most actions that may wait at one place.  One more is not scheduled:
/// it is counted (`overrun`) and never done.
inline constexpr std::size_t max_waiting_actions = 1024;

/// One action that was carried out, for the log.
struct action_report
{
    /// The rule, or #no_index for the timeline; the step of its sequence
    /// (the entry of the timeline); which match of the rule it was.
    std::size_t rule = no_index;
    std::size_t step = 0;
    std::uint64_t ordinal = 0;
    /// The action's place in the step's list, and what it was.
    std::size_t index = 0;
    action_kind kind = action_kind::mark;
    /// How many bytes it sent, counting each copy.
    std::size_t bytes = 0;
    /// When it was due and when it was handed on.  `done` is never before
    /// `due`.
    time_point due{};
    time_point done{};
    /// It was later than the set's `late_after_us`.
    bool late = false;
};

struct scheduler_stats
{
    machine_stats machine;
    /// Actions given a time, done, done later than `late_after_us`, refused
    /// because #max_waiting_actions were waiting, and dropped by #close.
    std::uint64_t scheduled = 0;
    std::uint64_t done = 0;
    std::uint64_t late = 0;
    std::uint64_t overrun = 0;
    std::uint64_t cancelled = 0;
    /// The most any action was done after it was due.
    clock_type::duration worst_lateness{0};
};

/// What the engine gives a scheduler.
struct scheduler_hooks
{
    /// The hub's clock (#hub::now).  Required.
    std::function<time_point()> now;
    /// The clock is one a test moves by hand.  A timer cannot wait for such
    /// a clock, so the scheduler looks at it every millisecond while it has
    /// something to wait for.
    bool poll_clock = false;
    /// Carries out one action whose time has come: a `reply` (the bytes,
    /// `copies` times), `set_lines`, `raise_error`, `mark`.  Called on the
    /// scheduler's strand, one at a time, in the order the actions are due.
    /// It must not block: it hands the work to the port.  Required.
    std::function<void(const planned_action&)> act;
    /// Told of each action once `act` has returned.  Same strand.  Optional.
    std::function<void(const action_report&)> done;
};

class scheduler final : public read_stage,
                        public std::enable_shared_from_this<scheduler>
{
public:
    /// `run_seed` is used when the set has no seed of its own.
    scheduler(const boost::asio::any_io_executor& executor,
              std::shared_ptr<const compiled_rule_set> set,
              std::uint64_t run_seed, scheduler_hooks hooks);

    scheduler(const scheduler&) = delete;
    scheduler& operator=(const scheduler&) = delete;

    /// Starts the set: fixes the start of its run at `origin` and schedules
    /// its timeline.  Before this, and after #close, the scheduler ignores
    /// what it is shown.  Call it once.
    void start(time_point origin);

    /// The read stage: matches the chunk and schedules what its rules plan.
    void on_read(endpoint_id from, bytes_view data,
                 time_point when) noexcept override;

    /// The lines of the place changed, or a program opened or closed the
    /// port.  Safe from any thread.
    void lines_changed(control_lines before, control_lines after,
                       time_point when);
    void port_changed(port_event what, time_point when);

    /// The owner of the set raised a signal (see `machine::signal`).
    /// `errc::not_found` for an index the set does not have.  Safe from any
    /// thread.
    result<void> signal(std::size_t index, bytes_view carried,
                        time_point cause, time_point when);

    /// Stops the set.  What is waiting is discarded and counted, and what
    /// comes later is ignored.  When it returns the scheduler has carried
    /// out its last action: one that was under way has finished and none
    /// will start.  Safe from any thread except inside `act`.
    void close();

    const compiled_rule_set& set() const noexcept { return *set_; }
    std::uint64_t seed() const;
    scheduler_stats stats() const;
    /// How many actions are waiting.
    std::size_t waiting() const;

private:
    struct item
    {
        time_point due{};
        /// The order the actions were planned in, which settles a tie.
        std::uint64_t sequence = 0;
        std::size_t rule = no_index;
        std::size_t step = 0;
        std::uint64_t ordinal = 0;
        planned_action action;
    };

    /// True when `a` is done after `b`.
    static bool later(const item& a, const item& b) noexcept;

    void schedule(std::vector<firing>& firings);
    std::optional<time_point> next_wake() const;
    void wake_if_sooner();
    void arm();
    void fire();

    const boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    const std::shared_ptr<const compiled_rule_set> set_;
    const scheduler_hooks hooks_;

    /// Held while actions that are due are carried out, and by #close, so
    /// that a close waits for one under way.  Always taken before `mutex_`.
    std::mutex deliver_mutex_;
    mutable std::mutex mutex_;
    machine machine_;
    bool started_ = false;
    bool closed_ = false;
    /// A heap on #later: the next action due is at the front.
    std::vector<item> queue_;
    std::uint64_t sequence_ = 0;
    /// The time the timer was last asked to wake for, if it is waiting.
    std::optional<time_point> armed_for_;
    /// Kept between calls so that the path of the bytes does not allocate
    /// one each time.
    std::vector<firing> scratch_;
    scheduler_stats stats_;
};

} // namespace vss::rules
