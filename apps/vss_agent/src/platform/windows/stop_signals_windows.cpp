//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform/stop_signals_platform.hpp"

#include <csignal>

namespace vss_agent::platform
{

void add_stop_signals(boost::asio::signal_set& signals)
{
    // The C runtime raises SIGBREAK for Ctrl-Break and when the console
    // window is closed.
    boost::system::error_code ignored;
    signals.add(SIGBREAK, ignored);
}

} // namespace vss_agent::platform
