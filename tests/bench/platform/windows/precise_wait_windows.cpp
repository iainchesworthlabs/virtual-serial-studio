//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timing benchmark's wait on Windows: a high resolution waitable timer
// (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803 and later), made
// once per thread.  Where the system cannot make one the wait falls back to
// the standard sleep.  begin_timer_period is timeBeginPeriod(1).
//

#include "precise_wait.hpp"

#include <windows.h>

#include <timeapi.h>

#include <chrono>
#include <thread>

namespace vss_bench
{

namespace
{

/// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, spelt out because the project
/// targets an older Windows version in its headers.
constexpr DWORD high_resolution_flag = 0x00000002;

/// The timer of one thread, closed when the thread ends.
class thread_timer
{
public:
    thread_timer()
        : handle_(CreateWaitableTimerExW(nullptr, nullptr,
                                         high_resolution_flag,
                                         TIMER_ALL_ACCESS))
    {
    }

    ~thread_timer()
    {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }

    thread_timer(const thread_timer&) = delete;
    thread_timer& operator=(const thread_timer&) = delete;

    /// True when the wait was done by the timer.
    bool wait(std::chrono::nanoseconds span) const
    {
        if (handle_ == nullptr)
            return false;
        // A negative due time is relative, in units of 100 ns.
        LARGE_INTEGER due_time;
        due_time.QuadPart = -(span.count() / 100);
        if (!SetWaitableTimer(handle_, &due_time, 0, nullptr, nullptr, FALSE))
            return false;
        return WaitForSingleObject(handle_, INFINITE) == WAIT_OBJECT_0;
    }

private:
    HANDLE handle_;
};

using query_timer_resolution = LONG(NTAPI*)(PULONG, PULONG, PULONG);

} // namespace

void precise_wait_until(std::chrono::steady_clock::time_point due)
{
    thread_local const thread_timer timer;
    const auto span = due - std::chrono::steady_clock::now();
    if (span <= std::chrono::steady_clock::duration::zero())
        return;
    if (!timer.wait(std::chrono::duration_cast<std::chrono::nanoseconds>(span)))
        std::this_thread::sleep_until(due);
}

void begin_timer_period() noexcept
{
    timeBeginPeriod(1);
}

void end_timer_period() noexcept
{
    timeEndPeriod(1);
}

unsigned long current_timer_resolution_100ns()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
        return 0;
    const FARPROC found = GetProcAddress(ntdll, "NtQueryTimerResolution");
    if (found == nullptr)
        return 0;
    const auto query = reinterpret_cast<query_timer_resolution>(found);
    ULONG finest = 0;
    ULONG coarsest = 0;
    ULONG current = 0;
    if (query(&finest, &coarsest, &current) != 0)
        return 0;
    return current;
}

} // namespace vss_bench
