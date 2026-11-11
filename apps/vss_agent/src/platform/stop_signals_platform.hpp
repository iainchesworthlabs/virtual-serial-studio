//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signals that ask the agent to stop, apart from SIGINT and SIGTERM,
// which every system has.  One implementation per platform family in
// src/platform/<windows|posix>; CMake picks the file.
//

#pragma once

#include <boost/asio/signal_set.hpp>

namespace vss_agent::platform
{

/// Adds this system's own stop signals to `signals`: Ctrl-Break and a closed
/// console on Windows, a closed terminal elsewhere.
void add_stop_signals(boost::asio::signal_set& signals);

} // namespace vss_agent::platform
