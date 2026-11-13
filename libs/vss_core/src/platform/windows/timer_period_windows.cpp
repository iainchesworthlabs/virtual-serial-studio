//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform/timer_period_platform.hpp"

#include <windows.h>

#include <timeapi.h>

namespace vss::platform
{

namespace
{

/// The finest period the multimedia timer interface takes.
constexpr UINT period_ms = 1;

} // namespace

bool begin_timer_period() noexcept
{
    return ::timeBeginPeriod(period_ms) == TIMERR_NOERROR;
}

void end_timer_period() noexcept
{
    // A request is given back with the period it asked for.
    ::timeEndPeriod(period_ms);
}

} // namespace vss::platform
