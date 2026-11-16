//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli plugin verify`, `install` and `remove`.
//

#pragma once

#include <string>
#include <vector>

namespace vss_cli
{

/// Each takes everything after the program name, starting with "plugin", and
/// returns the exit code: 0 for success, 1 when the plugin is refused or the
/// work failed, 2 for a command line it does not understand.
int command_plugin_verify(const std::vector<std::string>& arguments);
int command_plugin_install(const std::vector<std::string>& arguments);
int command_plugin_remove(const std::vector<std::string>& arguments);

/// The text of these three commands in `--help`.
const char* plugin_install_usage() noexcept;

} // namespace vss_cli
