//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A timer that is finer than the system's usual one, for the releases of a
// link with wire timing.  One implementation per platform family in
// src/platform/<windows|posix>; CMake picks the file.
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace vss::platform
{

/// How close to its time a timer of this process fires, as far as the system
/// says: about half a millisecond with a high resolution waitable timer
/// (Windows 10 version 1803 and later), a millisecond with the multimedia
/// timer period alone, and a fifth of a millisecond elsewhere.  A release
/// that is nearer than this is made by spinning on the clock.
[[nodiscard]] std::chrono::nanoseconds timer_accuracy() noexcept;

/// A one-shot timer on an executor.  Where the system has no finer timer than
/// asio's own, `available()` is false and `expires_after` and `async_wait` do
/// nothing: the caller uses its `steady_timer` instead.
class precise_timer
{
public:
    explicit precise_timer(const boost::asio::any_io_executor& executor);
    ~precise_timer();

    precise_timer(const precise_timer&) = delete;
    precise_timer& operator=(const precise_timer&) = delete;

    [[nodiscard]] bool available() const noexcept;

    /// Sets the time to wait for, from now.  Cancels a wait that is pending.
    void expires_after(std::chrono::nanoseconds wait);

    /// Calls `handler` on the executor when the time has passed, or with an
    /// error when `cancel` or `expires_after` took the wait away.
    void async_wait(std::function<void(boost::system::error_code)> handler);

    void cancel();

private:
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace vss::platform
