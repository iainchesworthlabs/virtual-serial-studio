//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/lifecycle.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace vss::plugin
{

namespace
{

/// The next number of a small generator (SplitMix64).  The waits need
/// numbers that differ between plugins, not numbers nobody can guess.
std::uint64_t next_random(std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

} // namespace

plugin_run::plugin_run(restart_options options, std::uint64_t seed)
    : options_(std::move(options)), random_(seed)
{
    options_.jitter_percent = std::min(options_.jitter_percent, 100U);
    for (auto& wait : options_.waits)
        wait = std::max(wait, std::chrono::milliseconds{0});
}

run_state plugin_run::state() const noexcept
{
    return state_;
}

run_step plugin_run::step(bool start_now, bool stop_now) const
{
    run_step result;
    result.state = state_;
    result.start_now = start_now;
    result.stop_now = stop_now;
    result.wake_at = next_start_;
    return result;
}

void plugin_run::forget_old(run_time now)
{
    while (!failures_.empty() && now - failures_.front() >= options_.window)
        failures_.pop_front();
}

std::chrono::milliseconds plugin_run::jitter(std::chrono::milliseconds wait)
{
    // A part of the wait from nothing to `jitter_percent` of it, in steps
    // of a thousandth of that.  The numbers stay small: a wait of weeks
    // times 100 times 1000 still fits 64 bits.
    const auto roll = static_cast<std::chrono::milliseconds::rep>(
        next_random(random_) % 1001U);
    const auto percent =
        static_cast<std::chrono::milliseconds::rep>(options_.jitter_percent);
    const auto most = std::chrono::milliseconds::max().count() / 100000;
    const auto base = std::min(wait.count(), most);
    return std::chrono::milliseconds{base * percent * roll / 100000};
}

run_step plugin_run::start(run_time now)
{
    switch (state_)
    {
    case run_state::quarantined:
        failures_.clear();
        last_failure_ = error_code{};
        break;
    case run_state::stopped:
    case run_state::backing_off:
        break;
    case run_state::starting:
    case run_state::ready:
    case run_state::stopping:
        return step();
    }
    forget_old(now);
    next_start_.reset();
    ready_since_.reset();
    state_ = run_state::starting;
    return step(true);
}

run_step plugin_run::on_ready(run_time now)
{
    if (state_ != run_state::starting)
        return step();
    ready_since_ = now;
    state_ = run_state::ready;
    return step();
}

run_step plugin_run::on_failed(error_code reason, run_time now)
{
    if (state_ == run_state::stopping)
    {
        ready_since_.reset();
        state_ = run_state::stopped;
        return step();
    }
    if (state_ != run_state::starting && state_ != run_state::ready)
        return step();

    if (ready_since_.has_value() &&
        now - *ready_since_ >= options_.reset_after_ready)
        failures_.clear();
    ready_since_.reset();
    forget_old(now);
    failures_.push_back(now);
    last_failure_ = reason;

    if (failures_.size() > options_.waits.size())
    {
        state_ = run_state::quarantined;
        return step();
    }
    const std::chrono::milliseconds wait = options_.waits[failures_.size() - 1];
    next_start_ = now + wait + jitter(wait);
    state_ = run_state::backing_off;
    return step();
}

run_step plugin_run::stop(run_time)
{
    switch (state_)
    {
    case run_state::starting:
    case run_state::ready:
        state_ = run_state::stopping;
        return step(false, true);
    case run_state::quarantined:
        failures_.clear();
        last_failure_ = error_code{};
        [[fallthrough]];
    case run_state::backing_off:
        next_start_.reset();
        state_ = run_state::stopped;
        return step();
    case run_state::stopped:
    case run_state::stopping:
        break;
    }
    return step();
}

run_step plugin_run::on_stopped(run_time)
{
    if (state_ != run_state::stopping)
        return step();
    ready_since_.reset();
    state_ = run_state::stopped;
    return step();
}

run_step plugin_run::on_time(run_time now)
{
    if (state_ != run_state::backing_off || !next_start_.has_value() ||
        now < *next_start_)
        return step();
    forget_old(now);
    next_start_.reset();
    state_ = run_state::starting;
    return step(true);
}

std::size_t plugin_run::failures(run_time now) const noexcept
{
    const auto counts = [&](run_time when)
    { return now - when < options_.window; };
    return static_cast<std::size_t>(
        std::count_if(failures_.begin(), failures_.end(), counts));
}

std::size_t plugin_run::failure_limit() const noexcept
{
    return options_.waits.size() + 1;
}

error_code plugin_run::last_failure() const noexcept
{
    return last_failure_;
}

std::optional<run_time> plugin_run::next_start() const noexcept
{
    return next_start_;
}

} // namespace vss::plugin
