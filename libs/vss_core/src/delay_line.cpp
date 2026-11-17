//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "delay_line.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/serial_settings.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace vss {

namespace
{

/// How often a line on a clock that is not the steady clock looks at it, as
/// in bus_wire: a timer cannot be asked to wait for a clock a test moves by
/// hand.
constexpr std::chrono::milliseconds foreign_clock_poll{1};

} // namespace

delay_line::delay_line(const boost::asio::any_io_executor& executor,
                       std::shared_ptr<endpoint> target,
                       std::uint32_t delay_ms, std::uint32_t jitter_ms,
                       clock_function clock, forward_function forward,
                       wire_timing_options timing,
                       std::shared_ptr<endpoint> source,
                       std::shared_ptr<pacing_counters> counters,
                       std::shared_ptr<spin_budget> budget, note_function note,
                       std::chrono::nanoseconds accuracy)
    : strand_(boost::asio::make_strand(executor))
    , timer_(strand_)
    , target_(std::move(target))
    , delay_ms_(delay_ms)
    , jitter_ms_(std::min(jitter_ms, delay_ms))
    , clock_(std::move(clock))
    , forward_(std::move(forward))
    , timing_(timing)
    , source_(std::move(source))
    , counters_(counters ? std::move(counters)
                         : std::make_shared<pacing_counters>())
    , budget_(std::move(budget))
    , note_(std::move(note))
    , accuracy_(accuracy.count() > 0 ? accuracy : platform::timer_accuracy())
    , random_(std::random_device{}())
{
    if (timing_.enabled && !clock_)
    {
        precise_ = std::make_unique<platform::precise_timer>(strand_);
        if (!precise_->available())
            precise_.reset();
    }
}

std::optional<wire_rate> delay_line::resolve_rate() const
{
    if (source_)
    {
        // What the program on the sender port set.
        if (const auto format = source_->peer_format();
            format && format->baud_rate != 0)
            return wire_rate{format->baud_rate, framing_of(*format)};
        // The own setting of a real line.  The setting of a virtual port is
        // not a choice anyone made for the wire, so it does not count.
        const auto kind = source_->kind();
        if (kind == endpoint_kind::physical || kind == endpoint_kind::rfc2217)
        {
            const serial_settings& serial = source_->options().serial;
            if (serial.baud_rate != 0)
                return wire_rate{serial.baud_rate,
                                 {serial.data_bits, serial.parity,
                                  serial.stop_bits}};
        }
    }
    if (timing_.bit_rate != 0)
        return wire_rate{timing_.bit_rate,
                         timing_.framing.value_or(wire_framing{})};
    return std::nullopt;
}

void delay_line::note_unpaced()
{
    if (note_logged_)
        return;
    note_logged_ = true;
    if (note_)
        note_("no rate for this link: wire timing is on, but neither the "
              "sender nor the link has a baud rate, so its traffic is not "
              "paced");
}

bool delay_line::spin_until(time_point due)
{
    // A clock a test moves cannot be spun on, and without a budget there
    // is no allowance to spend.
    if (clock_ || !budget_)
        return false;
    const auto remaining = due - clock_type::now();
    if (remaining <= clock_type::duration::zero() ||
        remaining > max_spin_per_release)
        return false;
    if (!budget_->take(remaining, clock_type::now()))
    {
        counters_->spin_refused.fetch_add(1);
        exact_wait_ = true;
        return false;
    }
    while (clock_type::now() < due)
    {
    }
    return true;
}

time_point delay_line::now() const
{
    return clock_ ? clock_() : clock_type::now();
}

void delay_line::push(const stream_item& item, visited_set visited)
{
    owned_item owned;
    if (const auto* bytes = std::get_if<bytes_view>(&item))
    {
        if (bytes->empty())
            return;
        owned = byte_buffer(bytes->begin(), bytes->end());
    }
    else
    {
        owned = std::get<line_break>(item);
    }

    VSS_PROFILE_ZONE("link.delay_push");

    bool was_empty = false;
    {
        std::lock_guard lock(mutex_);
        // A chunk the hub was routing when the route was removed.
        if (closed_)
            return;
        std::int64_t latency = delay_ms_;
        if (jitter_ms_ != 0)
        {
            std::uniform_int_distribution<std::int64_t> spread(
                -static_cast<std::int64_t>(jitter_ms_),
                static_cast<std::int64_t>(jitter_ms_));
            latency += spread(random_);
        }

        // A chunk is never due before the one ahead of it, so a short draw
        // after a long one waits for it instead of overtaking it.
        const time_point due = std::max(
            now() + std::chrono::milliseconds(latency), last_due_);
        was_empty = queue_.empty();

        if (!timing_.enabled)
        {
            last_due_ = due;
            queue_.push_back(
                entry{due, false, std::move(owned), std::move(visited)});
        }
        else
        {
            const std::optional<wire_rate> rate = resolve_rate();
            if (!rate)
                note_unpaced();
            const wire_rate known = rate.value_or(wire_rate{});
            if (auto* buffer = std::get_if<byte_buffer>(&owned))
            {
                std::vector<paced_run> runs;
                if (rate)
                    runs = pacer_.plan_bytes(due, buffer->size(), known,
                                             accuracy_);
                else
                    runs.push_back(
                        {pacer_.plan_unpaced(due), 0, buffer->size()});
                for (const paced_run& run : runs)
                {
                    byte_buffer part =
                        runs.size() == 1
                            ? std::move(*buffer)
                            : byte_buffer(buffer->data() + run.offset,
                                          buffer->data() + run.offset +
                                              run.length);
                    last_due_ = std::max(last_due_, run.release);
                    queue_.push_back(entry{run.release, rate.has_value(),
                                           std::move(part), visited});
                }
            }
            else
            {
                const auto& pause = std::get<line_break>(owned);
                const time_point release =
                    rate ? pacer_.plan_break(due, pause.ends_break(),
                                             pause.measured, pause.duration,
                                             known)
                         : pacer_.plan_unpaced(due);
                last_due_ = std::max(last_due_, release);
                queue_.push_back(entry{release, rate.has_value(),
                                       std::move(owned), std::move(visited)});
            }
        }
        VSS_PROFILE_PLOT("link.delay_queue", queue_.size());
    }

    if (!was_empty)
        return;

    // The line holds the timer's strand, which holds the line: the handlers
    // keep only a weak reference, so a route that goes away takes its queue
    // with it.
    boost::asio::post(strand_, [weak = weak_from_this()] {
        if (const auto self = weak.lock())
            self->arm();
    });
}

void delay_line::arm()
{
    time_point due;
    bool paced = false;
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty())
            return;
        due = queue_.front().due;
        paced = queue_.front().paced;
    }

    if (clock_)
    {
        const bool ready = due <= clock_();
        timer_.expires_after(ready ? std::chrono::milliseconds(0)
                                   : foreign_clock_poll);
    }
    else
    {
        // A paced release is waited for until the timer's accuracy before
        // it, and spun for after that, within the budget.
        const bool spins = paced && budget_ && !exact_wait_;
        const time_point wake = spins ? due - accuracy_ : due;
        if (precise_)
        {
            precise_->expires_after(std::max(
                wake - clock_type::now(), clock_type::duration::zero()));
            precise_->async_wait(
                [weak = weak_from_this()](boost::system::error_code ec)
                {
                    if (ec)
                        return;
                    if (const auto self = weak.lock())
                        self->fire();
                });
            return;
        }
        timer_.expires_at(wake);
    }
    timer_.async_wait([weak = weak_from_this()](boost::system::error_code ec) {
        if (ec)
            return;
        if (const auto self = weak.lock())
            self->fire();
    });
}

void delay_line::fire()
{
    VSS_PROFILE_ZONE("link.delay_fire");
    // Only this strand and close() take it, so it is free unless the route
    // is being removed.
    std::lock_guard delivering(deliver_mutex_);
    std::vector<entry> ready;
    const auto collect = [&]
    {
        std::lock_guard lock(mutex_);
        const time_point current = now();
        while (!queue_.empty() && queue_.front().due <= current)
        {
            ready.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    };

    // More than one pass only while the next release is near enough to spin
    // for, and the budget allows it: paced runs a few hundred microseconds
    // apart are then delivered one after another here, without a trip
    // through the timer between them.
    for (;;)
    {
        collect();

        // Woken a little before a paced release: spin the rest of the way.
        if (ready.empty() && !clock_)
        {
            time_point head_due{};
            bool paced = false;
            {
                std::lock_guard lock(mutex_);
                if (!queue_.empty())
                {
                    head_due = queue_.front().due;
                    paced = queue_.front().paced;
                }
            }
            if (paced && spin_until(head_due))
                collect();
        }
        if (ready.empty())
            break;
        exact_wait_ = false;

        for (auto& chunk : ready)
            deliver(chunk);
        ready.clear();

        if (clock_ || closing_.load(std::memory_order_acquire))
            break;
    }

    arm();
}

void delay_line::deliver(entry& chunk)
{
    // The write takes the bytes, so the forward goes first and reads them
    // from here; the endpoint's queue is where order is kept.
    if (chunk.visited && forward_)
        forward_(target_->id(), view_of(chunk.data), chunk.visited, now());
    if (chunk.paced)
    {
        // How late the release is, against the time the wire gave it.
        const auto late = std::chrono::duration_cast<std::chrono::microseconds>(
            now() - chunk.due);
        counters_->releases.fetch_add(1);
        if (late > late_release_threshold)
            counters_->late.fetch_add(1);
        const auto late_us = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, late.count()));
        auto seen = counters_->max_late_us.load();
        while (late_us > seen &&
               !counters_->max_late_us.compare_exchange_weak(seen, late_us))
        {
        }
    }
    if (auto* bytes = std::get_if<byte_buffer>(&chunk.data))
        target_->write(std::move(*bytes));
    else
        target_->write_break(std::get<line_break>(chunk.data));
}

void delay_line::close()
{
    // A pass that is spinning from one release to the next stops at its
    // next release, so the lock below is not held for long.
    closing_.store(true, std::memory_order_release);
    std::lock_guard delivering(deliver_mutex_);
    std::lock_guard lock(mutex_);
    closed_ = true;
    queue_.clear();
}

} // namespace vss
