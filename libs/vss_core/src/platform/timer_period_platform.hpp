//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How often the system looks at its timers. One implementation per platform
// family in src/platform/<windows|posix>; CMake picks the file.
//

#pragma once

namespace vss::platform
{

/// While one of these exists, a timer of this process fires within about a
/// millisecond of its time.
///
/// Windows looks at its timers once in 15.6 ms unless a process asks for
/// more, so a 1 ms delay there takes about 15 ms. This asks for 1 ms and
/// gives the request back when it is destroyed. The system counts the
/// requests of a process, so several may exist at once. Elsewhere timers are
/// already that fine and this does nothing.
class timer_period
{
public:
    timer_period() noexcept;
    ~timer_period();

    timer_period(const timer_period&) = delete;
    timer_period& operator=(const timer_period&) = delete;

    /// Whether a request is held. False where none is needed, and where the
    /// system refused it.
    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    bool held_ = false;
};

} // namespace vss::platform
