//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The product's folders on Linux:
//
//   machine   /etc/virtual-serial-studio
//   user      $XDG_DATA_HOME/virtual-serial-studio, or
//             $HOME/.local/share/virtual-serial-studio when XDG_DATA_HOME
//             is unset, empty or not an absolute path
//

#include <vss/machine/folders.hpp>

#include "platform/posix/posix_system.hpp"

#include <filesystem>

namespace vss::machine
{

namespace fs = std::filesystem;

fs::path machine_folder()
{
    return fs::path("/etc/virtual-serial-studio");
}

fs::path user_folder()
{
    fs::path data = platform::absolute_environment_path("XDG_DATA_HOME");
    if (data.empty())
    {
        const fs::path home = platform::absolute_environment_path("HOME");
        if (home.empty())
            return {};
        data = home / ".local" / "share";
    }
    return data / "virtual-serial-studio";
}

} // namespace vss::machine
