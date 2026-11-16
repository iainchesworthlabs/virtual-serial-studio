//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The product's folders on macOS:
//
//   machine   /Library/Application Support/Virtual Serial Studio
//   user      $HOME/Library/Application Support/Virtual Serial Studio
//
// $HOME counts only when it is an absolute path.
//

#include <vss/machine/folders.hpp>

#include "platform/posix/posix_system.hpp"

#include <filesystem>

namespace vss::machine
{

namespace fs = std::filesystem;

fs::path machine_folder()
{
    return fs::path("/Library/Application Support/Virtual Serial Studio");
}

fs::path user_folder()
{
    const fs::path home = platform::absolute_environment_path("HOME");
    if (home.empty())
        return {};
    return home / "Library" / "Application Support" / "Virtual Serial Studio";
}

} // namespace vss::machine
