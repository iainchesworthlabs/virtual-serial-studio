//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are on Windows:
//
//   machine   <ProgramData>\Virtual Serial Studio\plugins
//   user      <LocalAppData>\Virtual Serial Studio\plugins
//   policy    <ProgramData>\Virtual Serial Studio\policy.json
//
// The product's folders are `vss::machine`'s: asked of Windows itself (the
// known folders), not read from the environment variables of the same names.
//

#include "platform/locations_platform.hpp"

#include <vss/machine/folders.hpp>

#include <filesystem>

namespace vss::plugin::platform
{

namespace fs = std::filesystem;

fs::path machine_plugin_folder()
{
    const fs::path folder = machine::machine_folder();
    if (folder.empty())
        return {};
    return folder / L"plugins";
}

fs::path user_plugin_folder()
{
    const fs::path folder = machine::user_folder();
    if (folder.empty())
        return {};
    return folder / L"plugins";
}

fs::path policy_file_path()
{
    const fs::path folder = machine::machine_folder();
    if (folder.empty())
        return {};
    return folder / L"policy.json";
}

} // namespace vss::plugin::platform
