//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/stats.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace vss
{

/// What the paced routes of one link count (see #link_stats).  Shared by the
/// link and its delay lines; every member is safe to touch from any thread.
struct pacing_counters
{
    std::atomic<std::uint64_t> releases{0};
    std::atomic<std::uint64_t> late{0};
    std::atomic<std::uint64_t> max_late_us{0};
    std::atomic<std::uint64_t> spin_refused{0};
};

/// How close to its time a timer of this process fires, as far as the
/// platform says (see `src/platform/precise_timer_platform.hpp`): about
/// 0.5 ms on Windows with a high resolution waitable timer, 1 ms on Windows
/// without, 0.2 ms elsewhere.  A paced chunk whose wire time is within it is
/// one release, and a release nearer than this is spun for.
std::chrono::nanoseconds timer_accuracy() noexcept;

/// A release later than this, counted in `link_stats::late_releases`.
inline constexpr std::chrono::microseconds late_release_threshold{250};

/// The most one release spins on a core for the last part of a gap.
inline constexpr std::chrono::microseconds max_spin_per_release{1000};

/// How much of one core the spinning of all paced links together may use:
/// one tenth, so 100 ms of spinning in every second.
inline constexpr std::chrono::milliseconds spin_per_second{100};

/// The spinning allowance, shared by every paced route of a hub.  A release
/// asks for the time it wants to spin; when the allowance is used up it is
/// refused, and the release is made from the timer instead and counted as
/// late if it is.  Time comes in as an argument so a test can move it.
class spin_budget
{
public:
    using duration = std::chrono::nanoseconds;

    explicit spin_budget(duration per_second = spin_per_second) noexcept
        : per_second_(per_second), balance_(per_second)
    {
    }

    /// Takes `wanted` from the allowance and returns true, or returns false
    /// and takes nothing when there is less than that left.  The allowance
    /// fills at `per_second` for every second, up to one second's worth.
    bool take(duration wanted, std::chrono::steady_clock::time_point now)
    {
        std::lock_guard lock(mutex_);
        if (last_ != std::chrono::steady_clock::time_point{} && now > last_)
        {
            const auto passed = now - last_;
            const auto earned = std::chrono::duration_cast<duration>(
                passed * per_second_.count() / 1'000'000'000);
            balance_ = std::min(per_second_, balance_ + earned);
        }
        last_ = now;
        if (balance_ < wanted)
            return false;
        balance_ -= wanted;
        return true;
    }

    /// Gives back what a release did not use.
    void give_back(duration unused) noexcept
    {
        std::lock_guard lock(mutex_);
        balance_ = std::min(per_second_, balance_ + unused);
    }

private:
    const duration per_second_;
    std::mutex mutex_;
    duration balance_;
    std::chrono::steady_clock::time_point last_{};
};

} // namespace vss
