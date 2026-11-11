//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/bus_line.hpp>

#include <algorithm>
#include <chrono>

namespace vss
{

bus_line::bus_line(const half_duplex_options& options)
    : options_(options),
      turnaround_(std::chrono::milliseconds(options.turnaround_ms))
{
}

clock_type::duration bus_line::wire_time(std::uint32_t bit_rate,
                                         std::size_t bytes) noexcept
{
    if (bit_rate == 0)
        return {};
    // In nanoseconds.  A chunk is at most a read buffer, so the product
    // stays far inside 64 bits.
    const std::uint64_t bits =
        static_cast<std::uint64_t>(bytes) * bus_bits_per_byte;
    const std::uint64_t nanoseconds = bits * 1'000'000'000ULL / bit_rate;
    return std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds(static_cast<std::int64_t>(nanoseconds)));
}

bus_verdict bus_line::transmit(endpoint_id member, std::size_t bytes,
                               time_point now)
{
    std::lock_guard lock(mutex_);

    // What left the wire longer ago than the turn-around no longer matters.
    std::erase_if(active_, [&](const span& entry)
                  { return entry.end + turnaround_ <= now; });

    bus_verdict verdict;
    verdict.ends = now;

    if (options_.rts_gating)
    {
        const auto own = rts_.find(member);
        if (own != rts_.end() && !own->second)
        {
            verdict.fault = bus_fault::transmitter_off;
            verdict.carried = false;
            return verdict;
        }
    }

    // A member's own bytes leave one after another: a chunk waits for the
    // one before it.
    time_point start = now;
    for (const span& entry : active_)
    {
        if (entry.member == member)
            start = std::max(start, entry.end);
    }
    if (start - now > std::chrono::milliseconds(max_bus_backlog_ms))
    {
        verdict.fault = bus_fault::overrun;
        verdict.carried = false;
        return verdict;
    }
    const time_point end = start + wire_time(options_.bit_rate, bytes);

    const bool garble = options_.on_collision == collision_action::garble;
    for (const span& entry : active_)
    {
        if (entry.member == member)
            continue;
        if (entry.start < end && start < entry.end)
        {
            // Both were on the wire at once, so both are spoilt.
            if (verdict.fault != bus_fault::overlap)
            {
                verdict.fault = bus_fault::overlap;
                verdict.other = entry.member;
            }
            if (garble)
                entry.transmission->garble.store(true);
        }
        else if (verdict.fault == bus_fault::none && entry.end <= start &&
                 start < entry.end + turnaround_)
        {
            // The other had finished, so its bytes are whole; this one
            // started before the line was let go.
            verdict.fault = bus_fault::too_soon;
            verdict.other = entry.member;
        }
    }

    if (options_.rts_gating && verdict.fault == bus_fault::none)
    {
        for (const auto& [other, high] : rts_)
        {
            if (other != member && high)
            {
                verdict.fault = bus_fault::line_held;
                verdict.other = other;
                break;
            }
        }
    }

    verdict.ends = end;
    verdict.transmission = std::make_shared<bus_transmission>();
    if (verdict.fault != bus_fault::none)
    {
        collisions_.fetch_add(1);
        if (garble)
            verdict.transmission->garble.store(true);
    }
    active_.push_back(span{member, start, end, verdict.transmission});
    return verdict;
}

void bus_line::set_rts(endpoint_id member, std::optional<bool> high)
{
    std::lock_guard lock(mutex_);
    if (high)
        rts_[member] = *high;
    else
        rts_.erase(member);
}

void bus_line::remove(endpoint_id member)
{
    std::lock_guard lock(mutex_);
    std::erase_if(active_, [member](const span& entry)
                  { return entry.member == member; });
    rts_.erase(member);
}

} // namespace vss
