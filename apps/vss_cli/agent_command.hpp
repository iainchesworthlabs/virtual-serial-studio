//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <string>
#include <vector>

namespace vss_cli
{

/// `vss_cli agent ...`: a client of this user's `vss_agent`.  `arguments` is
/// everything after the program name, starting with "agent".  Returns the
/// process exit code: 0 for success, 1 for a request the agent refused, 2 for
/// a command line it does not understand and 3 when no agent is running.
int command_agent(const std::vector<std::string>& arguments);

/// The text of `agent` in `--help`.
const char* agent_usage() noexcept;

} // namespace vss_cli
