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

/// `vss_cli debug ...`: a client of the debug server of a running Studio
/// whose person has turned debug access on (docs/debug-api.md).  `arguments`
/// is everything after the program name, starting with "debug".  Returns the
/// process exit code: 0 for success, 1 for a request the studio refused or
/// the person declined, 2 for a command line it does not understand and 3
/// when no studio with debug access on is running.
int command_debug(const std::vector<std::string>& arguments);

/// The text of `debug` in `--help`.
const char* debug_usage() noexcept;

} // namespace vss_cli
