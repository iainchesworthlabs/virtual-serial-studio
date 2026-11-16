//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are on Linux:
//
//   machine   /usr/local/share/virtual-serial-studio/plugins
//   user      $XDG_DATA_HOME/virtual-serial-studio/plugins, or
//             $HOME/.local/share/virtual-serial-studio/plugins when
//             XDG_DATA_HOME is unset, empty or not an absolute path
//   policy    /etc/virtual-serial-studio/policy.json
//
// The product's folders are `vss::machine`'s.  $XDG_DATA_HOME and $HOME
// count only when they are absolute paths.
//

#include "platform/locations_platform.hpp"

#include <vss/machine/folders.hpp>

#include <filesystem>

namespace vss::plugin::platform
{

namespace fs = std::filesystem;

fs::path machine_plugin_folder()
{
    return fs::path("/usr/local/share/virtual-serial-studio/plugins");
}

fs::path user_plugin_folder()
{
    const fs::path folder = machine::user_folder();
    if (folder.empty())
        return {};
    return folder / "plugins";
}

fs::path policy_file_path()
{
    return machine::machine_folder() / "policy.json";
}

} // namespace vss::plugin::platform
