//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timers here are already fine enough, so there is no second timer: the
// caller's own is used.
//

#include "platform/precise_timer_platform.hpp"

namespace vss::platform
{

struct precise_timer::state
{
};

std::chrono::nanoseconds timer_accuracy() noexcept
{
    return std::chrono::microseconds(200);
}

precise_timer::precise_timer(const boost::asio::any_io_executor&) {}
precise_timer::~precise_timer() = default;

bool precise_timer::available() const noexcept
{
    return false;
}

void precise_timer::expires_after(std::chrono::nanoseconds) {}

void precise_timer::async_wait(
    std::function<void(boost::system::error_code)>)
{
}

void precise_timer::cancel() {}

} // namespace vss::platform
