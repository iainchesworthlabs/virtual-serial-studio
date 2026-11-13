//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform/timer_period_platform.hpp"

namespace vss::platform
{

// Linux and macOS time a sleeping thread to well under a millisecond with no
// request from the process.
bool begin_timer_period() noexcept
{
    return false;
}

void end_timer_period() noexcept
{
}

} // namespace vss::platform
