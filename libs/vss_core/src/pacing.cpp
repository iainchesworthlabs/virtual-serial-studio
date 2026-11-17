//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/pacing.hpp>

#include "platform/precise_timer_platform.hpp"

namespace vss
{

std::chrono::nanoseconds timer_accuracy() noexcept
{
    return platform::timer_accuracy();
}

} // namespace vss
