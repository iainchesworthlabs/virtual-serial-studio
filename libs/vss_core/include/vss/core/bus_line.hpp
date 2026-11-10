//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/link.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace vss {

/// One chunk on the wire of a half-duplex bus, shared by the line that judged
/// it and by the copy waiting to be delivered.
struct bus_transmission
{
    /// Set when the bytes are to reach the receivers inverted.  The line sets
    /// it, possibly after the chunk was queued (a later chunk ran into it), so
    /// whoever delivers the bytes reads it then and not before.
    std::atomic<bool> garble{false};
};

/// What the line decided about one chunk.
struct bus_verdict
{
    bus_fault fault = bus_fault::none;
    /// The member whose bytes or RTS were in the way; 0 when none was.
    endpoint_id other = 0;
    /// False when the bytes never reached the wire (`transmitter_off`,
    /// `overrun`): nobody hears them.
    bool carried = true;
    /// When the last byte has left the wire.  The other members hear the
    /// chunk then.
    time_point ends{};
    /// Null when the chunk was not carried.
    std::shared_ptr<bus_transmission> transmission;
};

/// Who holds the wire of a half-duplex bus, and until when.
///
/// This is the whole model and it has no clock: every call is given the time,
/// so the same calls always give the same answers.  The hub keeps one for
/// each half-duplex bus and passes the time it read a chunk at.  All members
/// are safe to call from any thread; a call does arithmetic under a mutex of
/// its own and nothing else.
class bus_line
{
public:
    /// `options` must satisfy #valid_half_duplex.
    explicit bus_line(const half_duplex_options& options);

    bus_line(const bus_line&) = delete;
    bus_line& operator=(const bus_line&) = delete;

    /// Judges a chunk of `bytes` bytes that `member` sent at `now`, and takes
    /// the wire for it when it is carried.
    bus_verdict transmit(endpoint_id member, std::size_t bytes, time_point now);

    /// Says what the hub knows of a member's RTS: high, low, or nothing at
    /// all (the member has no RTS the hub can follow, so it always sends and
    /// never holds the line).  Without `rts_gating` the line ignores it.
    void set_rts(endpoint_id member, std::optional<bool> high);

    /// Takes a member off the line.  Its chunks still on the wire stop
    /// counting against the others, as a station that was unplugged stops
    /// driving the pair.
    void remove(endpoint_id member);

    /// The chunks that began while the line was not free.
    std::uint64_t collisions() const noexcept { return collisions_.load(); }

    /// The bytes delivered inverted, for whoever delivers them to add to.
    const std::shared_ptr<std::atomic<std::uint64_t>>& garbled() const noexcept
    {
        return garbled_;
    }

    /// How long `bytes` bytes take at `bit_rate` bits per second.
    static clock_type::duration wire_time(std::uint32_t bit_rate,
                                          std::size_t bytes) noexcept;

private:
    struct span
    {
        endpoint_id member = 0;
        time_point start{};
        time_point end{};
        std::shared_ptr<bus_transmission> transmission;
    };

    const half_duplex_options options_;
    const clock_type::duration turnaround_;

    mutable std::mutex mutex_;
    /// The chunks on the wire, and those that left it less than the
    /// turn-around time ago.
    std::vector<span> active_;
    /// The RTS of each member that has one, by id so that the member named
    /// in a verdict does not depend on a hash.
    std::map<endpoint_id, bool> rts_;

    std::atomic<std::uint64_t> collisions_{0};
    const std::shared_ptr<std::atomic<std::uint64_t>> garbled_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
};

} // namespace vss
