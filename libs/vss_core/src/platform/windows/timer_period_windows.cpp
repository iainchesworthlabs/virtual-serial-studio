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

timer_period::timer_period() noexcept
    : held_(::timeBeginPeriod(period_ms) == TIMERR_NOERROR)
{
}

timer_period::~timer_period()
{
    // Every request is given back with the period it asked for.
    if (held_)
        ::timeEndPeriod(period_ms);
}

} // namespace vss::platform
