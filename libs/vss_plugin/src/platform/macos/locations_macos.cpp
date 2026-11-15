//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are on macOS:
//
//   machine   /Library/Application Support/Virtual Serial Studio/plugins
//   user      $HOME/Library/Application Support/Virtual Serial Studio/plugins
//   policy    /Library/Application Support/Virtual Serial Studio/policy.json
//
// The product's folders are `vss::machine`'s.  $HOME counts only when it is
// an absolute path.
//

#include "platform/locations_platform.hpp"

#include <vss/machine/folders.hpp>

#include <filesystem>

namespace vss::plugin::platform
{

namespace fs = std::filesystem;

fs::path machine_plugin_folder()
{
    return machine::machine_folder() / "plugins";
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
