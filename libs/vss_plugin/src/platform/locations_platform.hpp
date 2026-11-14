//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three plugin locations that depend on the operating system.  One
// implementation for each system, in src/platform/<windows|linux|macos>;
// CMake picks the files.  This header is not installed.
//
// Each function returns an empty path when the place cannot be worked out.
// A value read from the environment is untrusted text and counts only when
// it is an absolute path.
//

#pragma once

#include <filesystem>

namespace vss::plugin::platform
{

/// The plugin folder that an administrator installs plugins into for
/// everyone on this computer.
std::filesystem::path machine_plugin_folder();

/// The plugin folder of the user who runs this program.
std::filesystem::path user_plugin_folder();

/// The administrator's policy file.
std::filesystem::path policy_file_path();

} // namespace vss::plugin::platform
