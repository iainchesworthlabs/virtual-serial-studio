//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timing benchmark's wait on POSIX systems: the standard sleep, which is
// as precise as the kernel's timers.  There is no timer period to raise.
//

#include "precise_wait.hpp"

#include <chrono>
#include <thread>

namespace vss_bench
{

void precise_wait_until(std::chrono::steady_clock::time_point due)
{
    std::this_thread::sleep_until(due);
}

timer_period_scope::timer_period_scope() = default;

timer_period_scope::~timer_period_scope() = default;

unsigned long current_timer_resolution_100ns()
{
    return 0;
}

} // namespace vss_bench
