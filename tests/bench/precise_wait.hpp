//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The platform seam of the timing benchmark: a wait that is as precise as the
// operating system allows, and the system timer period.  CMake picks the file
// that implements it (bench/platform/<os>/).
//

#pragma once

#include <chrono>

namespace vss_bench
{

/// Blocks the calling thread in the operating system until `due`, or returns
/// at once when `due` has passed.  It never spins.
void precise_wait_until(std::chrono::steady_clock::time_point due);

/// Raises the system timer resolution to 1 ms for its lifetime where the
/// platform has such a thing, and does nothing elsewhere.
struct timer_period_scope
{
    timer_period_scope();
    ~timer_period_scope();

    timer_period_scope(const timer_period_scope&) = delete;
    timer_period_scope& operator=(const timer_period_scope&) = delete;
};

/// The current system timer resolution in units of 100 ns, or 0 when the
/// platform cannot say.
unsigned long current_timer_resolution_100ns();

} // namespace vss_bench
