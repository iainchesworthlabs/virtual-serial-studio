//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/stats.hpp>
#include <vss/core/wire_time.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vss
{

/// The rate and framing of a sender's wire.
struct wire_rate
{
    std::uint32_t bit_rate = 0;
    wire_framing framing;
};

/// One part of a chunk, to be released at `release`.
struct paced_run
{
    time_point release;
    std::size_t offset = 0;
    std::size_t length = 0;
};

/// The release times of one route.  It has no clock: each call is given the
/// time the item would be released without pacing, so the same calls always
/// give the same answers.
///
/// An item is released at
/// `max(due, previous_release + previous_wire_time)`: a chunk holds the next
/// item back for as long as it takes on the wire, and a break holds it for
/// its own length.
class wire_pacer
{
public:
    /// Where the wire is free again: the previous release plus its wire time.
    time_point free_at() const noexcept { return free_at_; }

    /// Plans `bytes` bytes that are due at `due`.  A chunk whose wire time
    /// is within `accuracy` is one run.  A longer one is split at character
    /// boundaries into runs that each take at most the larger of `accuracy`
    /// and one character, so a long frame is a few releases and not one for
    /// each byte.  Run `n` is released when its first character begins.
    std::vector<paced_run> plan_bytes(time_point due, std::size_t bytes,
                                      const wire_rate& rate,
                                      clock_type::duration accuracy)
    {
        std::vector<paced_run> runs;
        if (bytes == 0)
            return runs;
        const time_point release = std::max(due, free_at_);
        const clock_type::duration total =
            wire_time(rate.bit_rate, rate.framing, bytes);
        free_at_ = release + total;

        const clock_type::duration one =
            wire_time(rate.bit_rate, rate.framing, 1);
        if (total <= accuracy || one.count() <= 0)
        {
            runs.push_back({release, 0, bytes});
            return runs;
        }
        const std::size_t per_run = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::max(accuracy, one) / one));
        for (std::size_t offset = 0; offset < bytes; offset += per_run)
        {
            const std::size_t length = std::min(per_run, bytes - offset);
            runs.push_back({release + wire_time(rate.bit_rate, rate.framing,
                                                offset),
                            offset, length});
        }
        return runs;
    }

    /// Plans a break due at `due`.  A break that has not ended holds
    /// nothing.  One that has holds the wire for its `length`, or for one
    /// character at `rate` when it was not `measured`.
    time_point plan_break(time_point due, bool ends, bool measured,
                          std::chrono::nanoseconds length,
                          const wire_rate& rate)
    {
        const time_point release = std::max(due, free_at_);
        if (!ends)
        {
            free_at_ = release;
            return release;
        }
        const clock_type::duration held =
            measured ? std::chrono::duration_cast<clock_type::duration>(length)
                     : wire_time(rate.bit_rate, rate.framing, 1);
        free_at_ = release + held;
        return release;
    }

    /// Plans an item sent when no rate is known: not held, and the wire
    /// is taken to be free.
    time_point plan_unpaced(time_point due) noexcept
    {
        const time_point release = std::max(due, free_at_);
        free_at_ = release;
        return release;
    }

private:
    time_point free_at_{};
};

} // namespace vss
