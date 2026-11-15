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

/// `vss_cli plugin run <id> ...`.  `arguments` is everything after the program
/// name, starting with "plugin".  Returns the exit code: 0 when the plugin
/// reached running and was stopped by `--seconds` or the user, 1 when it never
/// reached running or failed too often, and 2 for a command line it does not
/// understand or an id that is not installed.
int command_plugin_run(const std::vector<std::string>& arguments);

/// The text of `plugin run` in `--help`.
const char* plugin_run_usage() noexcept;

} // namespace vss_cli
