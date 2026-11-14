//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/rules/scheduler.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace vss::rules {

namespace
{

/// How often a scheduler on a clock that is not the steady clock looks at
/// it, as the delay line does.
constexpr std::chrono::milliseconds foreign_clock_poll{1};

} // namespace

scheduler::scheduler(const boost::asio::any_io_executor& executor,
                     std::shared_ptr<const compiled_rule_set> set,
                     std::uint64_t run_seed, scheduler_hooks hooks)
    : strand_(boost::asio::make_strand(executor))
    , timer_(strand_)
    , set_(std::move(set))
    , hooks_(std::move(hooks))
    , machine_(set_, run_seed)
{
    queue_.reserve(max_waiting_actions);
}

bool scheduler::later(const item& a, const item& b) noexcept
{
    if (a.due != b.due)
        return a.due > b.due;
    return a.sequence > b.sequence;
}

void scheduler::start(time_point origin)
{
    {
        std::lock_guard lock(mutex_);
        if (started_ || closed_)
            return;
        started_ = true;
        std::vector<firing> timeline = machine_.start(origin);
        schedule(timeline);
    }
    wake_if_sooner();
}

void scheduler::on_read(endpoint_id, bytes_view data, time_point when) noexcept
{
    // The stage is on the path of the port's bytes and must not throw.  The
    // one thing that can go wrong here is memory, and then the chunk goes on
    // its way unmatched and is counted.
    try
    {
        {
            std::lock_guard lock(mutex_);
            if (!started_ || closed_)
                return;
            scratch_.clear();
            machine_.feed(data, when, scratch_);
            schedule(scratch_);
        }
        wake_if_sooner();
    }
    catch (...)
    {
        missed_chunks_.fetch_add(1, std::memory_order_relaxed);
    }
}

void scheduler::lines_changed(control_lines before, control_lines after,
                              time_point when)
{
    {
        std::lock_guard lock(mutex_);
        if (!started_ || closed_)
            return;
        scratch_.clear();
        machine_.lines_changed(before, after, when, scratch_);
        schedule(scratch_);
    }
    wake_if_sooner();
}

void scheduler::port_changed(port_event what, time_point when)
{
    {
        std::lock_guard lock(mutex_);
        if (!started_ || closed_)
            return;
        scratch_.clear();
        machine_.port_changed(what, when, scratch_);
        schedule(scratch_);
    }
    wake_if_sooner();
}

result<void> scheduler::signal(std::size_t index, bytes_view carried,
                               time_point cause, time_point when)
{
    if (index >= set_->signals.size())
        return make_error_code(errc::not_found);
    {
        std::lock_guard lock(mutex_);
        if (!started_ || closed_)
            return {};
        scratch_.clear();
        machine_.signal(index, carried, cause, when, scratch_);
        schedule(scratch_);
    }
    wake_if_sooner();
    return {};
}

void scheduler::close()
{
    std::lock_guard delivering(deliver_mutex_);
    std::lock_guard lock(mutex_);
    if (closed_)
        return;
    closed_ = true;
    stats_.cancelled += queue_.size();
    queue_.clear();
}

std::uint64_t scheduler::seed() const
{
    std::lock_guard lock(mutex_);
    return machine_.seed();
}

scheduler_stats scheduler::stats() const
{
    std::lock_guard lock(mutex_);
    scheduler_stats out = stats_;
    out.machine = machine_.stats();
    out.missed_chunks = missed_chunks_.load(std::memory_order_relaxed);
    return out;
}

std::size_t scheduler::waiting() const
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

// Called with `mutex_` held.
void scheduler::schedule(std::vector<firing>& firings)
{
    for (firing& fired : firings)
    {
        const bool timeline = fired.rule == no_index;
        for (planned_action& planned : fired.actions)
        {
            // A rule's state and slot changed at the match, in the machine.
            // On the timeline a state changes when its entry is due.
            const bool instant = planned.kind == action_kind::set_state ||
                                 planned.kind == action_kind::set_slot;
            if (instant && !timeline)
                continue;
            if (queue_.size() >= max_waiting_actions)
            {
                ++stats_.overrun;
                continue;
            }
            item entry;
            entry.due = planned.due;
            entry.sequence = sequence_++;
            entry.rule = fired.rule;
            entry.step = fired.step;
            entry.ordinal = fired.ordinal;
            entry.action = std::move(planned);
            queue_.push_back(std::move(entry));
            std::push_heap(queue_.begin(), queue_.end(), &scheduler::later);
            ++stats_.scheduled;
        }
    }
}

// Called with `mutex_` held.
std::optional<time_point> scheduler::next_wake() const
{
    std::optional<time_point> next = machine_.next_deadline();
    if (!queue_.empty() && (!next || queue_.front().due < *next))
        next = queue_.front().due;
    return next;
}

void scheduler::wake_if_sooner()
{
    {
        std::lock_guard lock(mutex_);
        const std::optional<time_point> next = next_wake();
        if (!next || (armed_for_ && *armed_for_ <= *next))
            return;
        // Claimed here, so that a burst of chunks posts once.
        armed_for_ = next;
    }
    // The scheduler holds the timer's strand, which holds the scheduler: the
    // handlers keep only a weak reference, so a set that is removed takes
    // its queue with it.
    boost::asio::post(strand_, [weak = weak_from_this()]
                      {
                          if (const auto self = weak.lock())
                              self->arm();
                      });
}

void scheduler::arm()
{
    time_point due;
    {
        std::lock_guard lock(mutex_);
        const std::optional<time_point> next = next_wake();
        if (!next)
        {
            armed_for_.reset();
            return;
        }
        armed_for_ = next;
        due = *next;
    }

    if (hooks_.poll_clock)
    {
        const bool ready = due <= hooks_.now();
        timer_.expires_after(ready ? std::chrono::milliseconds(0)
                                   : foreign_clock_poll);
    }
    else
    {
        timer_.expires_at(due);
    }
    timer_.async_wait([weak = weak_from_this()](boost::system::error_code ec)
                      {
                          // Asked again for another time: that wait runs.
                          if (ec)
                              return;
                          if (const auto self = weak.lock())
                              self->fire();
                      });
}

void scheduler::fire()
{
    // Only this strand and close() take it, so it is free unless the set is
    // being removed.
    std::lock_guard delivering(deliver_mutex_);
    std::vector<item> ready;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return;
        const time_point current = hooks_.now();
        // Silence that is complete may fire a rule, whose actions may be
        // due already.
        scratch_.clear();
        machine_.tick(current, scratch_);
        schedule(scratch_);
        while (!queue_.empty() && queue_.front().due <= current)
        {
            std::pop_heap(queue_.begin(), queue_.end(), &scheduler::later);
            item& next = queue_.back();
            if (next.rule == no_index &&
                next.action.kind == action_kind::set_state &&
                next.action.source != nullptr)
                (void)machine_.set_state(next.action.source->state);
            ready.push_back(std::move(next));
            queue_.pop_back();
        }
    }

    for (const item& each : ready)
    {
        if (each.action.kind != action_kind::set_state &&
            each.action.kind != action_kind::set_slot)
            hooks_.act(each.action);

        action_report report;
        report.rule = each.rule;
        report.step = each.step;
        report.ordinal = each.ordinal;
        report.index = each.action.index;
        report.kind = each.action.kind;
        report.bytes = each.action.bytes.size() * each.action.copies;
        report.due = each.due;
        report.done = hooks_.now();
        const clock_type::duration lateness = report.done - report.due;
        report.late = set_->late_after.count() != 0 &&
                      lateness > std::chrono::duration_cast<
                                     clock_type::duration>(set_->late_after);
        {
            std::lock_guard lock(mutex_);
            ++stats_.done;
            if (report.late)
                ++stats_.late;
            stats_.worst_lateness = std::max(stats_.worst_lateness, lateness);
        }
        if (hooks_.done)
            hooks_.done(report);
    }

    arm();
}

} // namespace vss::rules
