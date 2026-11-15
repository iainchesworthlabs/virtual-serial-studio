//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "stop_signals.hpp"

#include <csignal>

namespace vss_cli::platform
{

void catch_terminal_close(void (*handler)(int))
{
    // The C runtime raises SIGBREAK for Ctrl-Break and when the console window
    // is closed; SIGINT and SIGTERM do not cover either.
    std::signal(SIGBREAK, handler);
}

} // namespace vss_cli::platform
