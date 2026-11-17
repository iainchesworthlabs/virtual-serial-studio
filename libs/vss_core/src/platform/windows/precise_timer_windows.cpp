//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A high resolution waitable timer (Windows 10 version 1803 and later) waited
// for through asio.  Where the system refuses to make one the timer is not
// available and the caller keeps its steady timer.
//

#include "platform/precise_timer_platform.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/windows/object_handle.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace vss::platform
{

namespace
{

// Older SDKs do not declare it; the value is the documented one.
constexpr DWORD high_resolution_flag = 0x00000002;

HANDLE make_timer() noexcept
{
    return ::CreateWaitableTimerExW(nullptr, nullptr, high_resolution_flag,
                                    TIMER_ALL_ACCESS);
}

} // namespace

struct precise_timer::state
{
    explicit state(const boost::asio::any_io_executor& executor)
        : handle(make_timer())
    {
        if (handle != nullptr)
            wait.emplace(executor, handle);
    }

    ~state()
    {
        // The object handle owns the timer's handle once it has it.
        if (wait)
            wait.reset();
        else if (handle != nullptr)
            ::CloseHandle(handle);
    }

    HANDLE handle = nullptr;
    std::optional<boost::asio::windows::object_handle> wait;
};

std::chrono::nanoseconds timer_accuracy() noexcept
{
    static const bool high_resolution = []
    {
        const HANDLE probe = make_timer();
        if (probe == nullptr)
            return false;
        ::CloseHandle(probe);
        return true;
    }();
    return high_resolution ? std::chrono::microseconds(500)
                           : std::chrono::milliseconds(1);
}

precise_timer::precise_timer(const boost::asio::any_io_executor& executor)
    : state_(std::make_unique<state>(executor))
{
}

precise_timer::~precise_timer() = default;

bool precise_timer::available() const noexcept
{
    return state_->wait.has_value();
}

void precise_timer::expires_after(std::chrono::nanoseconds wait)
{
    if (!available())
        return;
    // A negative due time is relative, in units of 100 ns.
    LARGE_INTEGER due;
    due.QuadPart = -std::max<std::int64_t>(1, wait.count() / 100);
    state_->wait->cancel();
    ::SetWaitableTimer(state_->handle, &due, 0, nullptr, nullptr, FALSE);
}

void precise_timer::async_wait(
    std::function<void(boost::system::error_code)> handler)
{
    if (!available())
        return;
    state_->wait->async_wait(std::move(handler));
}

void precise_timer::cancel()
{
    if (!available())
        return;
    ::CancelWaitableTimer(state_->handle);
    state_->wait->cancel();
}

} // namespace vss::platform
