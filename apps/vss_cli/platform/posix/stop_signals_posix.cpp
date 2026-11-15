//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "stop_signals.hpp"

#include <signal.h>

namespace vss_cli::platform
{

void catch_terminal_close(void (*handler)(int))
{
    struct sigaction current{};
    if (::sigaction(SIGHUP, nullptr, &current) == 0 &&
        current.sa_handler == SIG_IGN)
        return;

    struct sigaction action{};
    action.sa_handler = handler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    static_cast<void>(::sigaction(SIGHUP, &action, nullptr));
}

} // namespace vss_cli::platform
