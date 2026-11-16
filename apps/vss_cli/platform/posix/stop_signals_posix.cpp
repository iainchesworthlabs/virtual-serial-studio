//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "stop_signals.hpp"

#include <signal.h>

namespace vss_cli::platform
{

namespace
{

/// Calls `handler` for `number`, unless the signal was ignored when the
/// program started and `keep_ignored` says it should stay so.
void catch_signal(int number, void (*handler)(int), bool keep_ignored)
{
    struct sigaction current{};
    if (keep_ignored && sigaction(number, nullptr, &current) == 0 &&
        current.sa_handler == SIG_IGN)
        return;

    struct sigaction action{};
    action.sa_handler = handler;
    // A function on Linux, a macro on macOS: no :: in front of it.
    static_cast<void>(sigemptyset(&action.sa_mask));
    action.sa_flags = SA_RESTART;
    static_cast<void>(sigaction(number, &action, nullptr));
}

} // namespace

void catch_stop_signals(void (*request_stop)(int))
{
    catch_signal(SIGINT, request_stop, false);
    catch_signal(SIGTERM, request_stop, false);
    // The terminal went away; `nohup` asks for this to be ignored.
    catch_signal(SIGHUP, request_stop, true);
    // A write to a closed pipe is an error to the command, not its end.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    static_cast<void>(sigemptyset(&ignore.sa_mask));
    static_cast<void>(sigaction(SIGPIPE, &ignore, nullptr));
}

void command_finished() {}

} // namespace vss_cli::platform
