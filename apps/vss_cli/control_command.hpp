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

/// `vss_cli control ...`: a client of this user's `vss_gate`.  `arguments` is
/// everything after the program name, starting with "control".  Returns the
/// process exit code: 0 for success, 1 for a request the gateway refused, 2
/// for a command line it does not understand and 3 when no gateway is
/// running.
int command_control(const std::vector<std::string>& arguments);

/// The text of `control` in `--help`.
const char* control_usage() noexcept;

} // namespace vss_cli
