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

/// `vss_cli plugin ...`. `arguments` is everything after the program name,
/// starting with "plugin". Returns the process exit code: 0 when the command
/// ran, whatever it found, and 2 for a command line it does not understand.
int command_plugin(const std::vector<std::string>& arguments);

/// The text of `plugin` in `--help`.
const char* plugin_usage() noexcept;

} // namespace vss_cli
