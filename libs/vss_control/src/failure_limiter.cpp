//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The failure limiter of the remote-control gateway: a delay that doubles for a
// key that keeps failing, and one limit on the failures of all keys together.
//
// Per key the table keeps the number of failures, the time of the last one and
// the time before which the key is refused.  The failures of all keys in the
// last second are kept as a short list of times; only the newest
// `global_per_second` matter, so the list never grows past that.
//

#include <vss/control/failure_limiter.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace vss::control
{
namespace
{

constexpr std::chrono::seconds global_window{1};

/// The limits as the table uses them: a count of zero would mean "refuse
/// always" or "remember nothing", so it is taken as one.
failure_limits normalised(failure_limits limits) noexcept
{
    if (limits.free_failures == 0)
        limits.free_failures = 1;
    if (limits.global_per_second == 0)
        limits.global_per_second = 1;
    if (limits.max_keys == 0)
        limits.max_keys = 1;
    return limits;
}

} // namespace

class failure_limiter::impl
{
public:
    using millis = std::chrono::milliseconds;

    explicit impl(failure_limits limits) : limits_(normalised(limits)) {}

    bool allowed(std::string_view key, clock::time_point now)
    {
        forget_old_failures(now);
        if (recent_in_window(now).count >= limits_.global_per_second)
            return false;

        const auto found = keys_.find(key);
        if (found == keys_.end())
            return true;
        if (is_quiet(found->second, now))
        {
            keys_.erase(found);
            return true;
        }
        return now >= found->second.refused_until;
    }

    void failed(std::string_view key, clock::time_point now)
    {
        forget_old_failures(now);
        recent_.push_back(now);
        while (recent_.size() > limits_.global_per_second)
            recent_.pop_front();

        auto found = keys_.find(key);
        if (found != keys_.end() && is_quiet(found->second, now))
        {
            keys_.erase(found);
            found = keys_.end();
        }
        if (found == keys_.end())
        {
            make_room();
            found =
                keys_.emplace(std::string(key), key_state{0, now, now}).first;
        }

        key_state& state = found->second;
        if (state.count < std::numeric_limits<unsigned>::max())
            ++state.count;
        state.last_failure = now;
        if (state.count >= limits_.free_failures)
            state.refused_until = after(now, delay_for(state.count));
    }

    void succeeded(std::string_view key)
    {
        const auto found = keys_.find(key);
        if (found != keys_.end())
            keys_.erase(found);
    }

    clock::time_point retry_at(std::string_view key,
                               clock::time_point now) const
    {
        clock::time_point when = now;
        const auto found = keys_.find(key);
        if (found != keys_.end() && !is_quiet(found->second, now))
            when = std::max(when, found->second.refused_until);

        const window_view window = recent_in_window(now);
        if (window.count >= limits_.global_per_second)
            when = std::max(when, window.oldest + global_window);
        return when;
    }

    std::size_t tracked() const noexcept { return keys_.size(); }

private:
    struct key_state
    {
        unsigned count;
        clock::time_point last_failure;
        clock::time_point refused_until;
    };

    /// The failures that still count against the global rule.
    struct window_view
    {
        std::size_t count;
        /// The earliest of them; the time asked about when there is none.
        clock::time_point oldest;
    };

    using key_table = std::map<std::string, key_state, std::less<>>;

    /// The time `delay` after `start`, or the latest time there is.
    static clock::time_point after(clock::time_point start, millis delay)
    {
        const auto room = std::chrono::duration_cast<millis>(
            clock::time_point::max() - start);
        if (delay >= room)
            return clock::time_point::max();
        return start + delay;
    }

    /// How long a key is refused after its `count`-th failure.
    millis delay_for(unsigned count) const
    {
        millis delay = std::min(limits_.first_delay, limits_.longest_delay);
        if (delay <= millis::zero())
            return millis::zero();
        for (unsigned step = limits_.free_failures;
             step < count && delay < limits_.longest_delay; ++step)
        {
            // Doubling without overflow: clamp when the double would pass the
            // longest delay.
            const millis room = limits_.longest_delay - delay;
            delay = delay >= room ? limits_.longest_delay : delay + delay;
        }
        return delay;
    }

    bool is_quiet(const key_state& state, clock::time_point now) const
    {
        return now - state.last_failure >= limits_.quiet_reset;
    }

    void forget_old_failures(clock::time_point now)
    {
        while (!recent_.empty() && now - recent_.front() >= global_window)
            recent_.pop_front();
    }

    window_view recent_in_window(clock::time_point now) const
    {
        window_view view{0, now};
        for (const clock::time_point failure : recent_)
        {
            if (now - failure >= global_window)
                continue;
            if (view.count == 0)
                view.oldest = failure;
            ++view.count;
        }
        return view;
    }

    /// Drops the keys that failed longest ago until one more fits.
    void make_room()
    {
        while (!keys_.empty() && keys_.size() >= limits_.max_keys)
        {
            const auto oldest = std::min_element(
                keys_.begin(), keys_.end(),
                [](const key_table::value_type& left,
                   const key_table::value_type& right)
                {
                    return left.second.last_failure < right.second.last_failure;
                });
            keys_.erase(oldest);
        }
    }

    const failure_limits limits_;
    key_table keys_;
    std::deque<clock::time_point> recent_;
};

failure_limiter::failure_limiter(failure_limits limits)
    : impl_(std::make_unique<impl>(limits))
{
}

failure_limiter::~failure_limiter() = default;

bool failure_limiter::allowed(std::string_view key, clock::time_point now)
{
    return impl_->allowed(key, now);
}

void failure_limiter::failed(std::string_view key, clock::time_point now)
{
    impl_->failed(key, now);
}

void failure_limiter::succeeded(std::string_view key)
{
    impl_->succeeded(key);
}

failure_limiter::clock::time_point
failure_limiter::retry_at(std::string_view key, clock::time_point now) const
{
    return impl_->retry_at(key, now);
}

std::size_t failure_limiter::tracked() const noexcept
{
    return impl_->tracked();
}

} // namespace vss::control
