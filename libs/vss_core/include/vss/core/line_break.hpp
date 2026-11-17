//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_LINE_BREAK_HPP
#define VSS_CORE_LINE_BREAK_HPP

#include <vss/core/bytes.hpp>
#include <vss/core/stats.hpp>

#include <chrono>
#include <variant>

namespace vss {

/// Which part of a break an item says.  See docs/break-event-plan.md.
enum class break_phase
{
    /// The line went to break at `start`.  It has no length yet.
    begin,
    /// The line came back.  `start` is when the break began and `duration`
    /// is the time it lasted.
    end,
    /// A begin and an end that the source knows together: a short break, or
    /// a break a real device reported after the fact.  `start` and
    /// `duration` are both set.
    complete
};

/// A serial BREAK as an item of the stream a port reads or is written, in
/// order with the data around it.
///
/// Times are on the hub's clock (#hub::now).  For a `begin` and a `complete`
/// the time of the item is `start`; for an `end` it is `start + duration`.
struct line_break
{
    break_phase phase = break_phase::complete;
    time_point start{};
    /// Zero for a `begin`.
    std::chrono::nanoseconds duration{0};
    /// False when `duration` is not a measurement but the character time a
    /// real device's break is known to be at least (a device reports a break
    /// and not its length).  A decoder must not trust an unmeasured length.
    bool measured = false;

    /// When the item happened: `start`, or the end of the break for `end`.
    time_point when() const noexcept
    {
        return phase == break_phase::end
                   ? start + std::chrono::duration_cast<
                                 clock_type::duration>(duration)
                   : start;
    }

    /// True when the item ends a break (an `end` or a `complete`), which is
    /// when a break is counted, shown as a record and acted on by a port
    /// that can only show the program a whole break.
    bool ends_break() const noexcept { return phase != break_phase::begin; }

    friend bool operator==(const line_break&, const line_break&) = default;
};

/// What a route carries: bytes, or a break.  Every stage that keeps the
/// order of chunks (the read stage, the router, the delay line of a link,
/// the wire of a bus, the write queue of an endpoint) keeps the order of
/// both.
using stream_item = std::variant<bytes_view, line_break>;

const char* to_string(break_phase value) noexcept;

} // namespace vss

#endif // VSS_CORE_LINE_BREAK_HPP
