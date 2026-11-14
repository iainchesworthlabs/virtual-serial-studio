//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are on macOS:
//
//   machine   /Library/Application Support/VirtualSerialStudio/plugins
//   user      $HOME/Library/Application Support/VirtualSerialStudio/plugins
//   policy    /Library/Application Support/VirtualSerialStudio/policy.json
//
// $HOME counts only when it is an absolute path.
//

#include "platform/locations_platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace vss::plugin::platform
{

namespace
{

namespace fs = std::filesystem;

/// The value of an environment variable as a path, when it is set and is an
/// absolute path; an empty path in every other case.  The text comes from
/// outside, and a relative path would depend on the folder the program runs
/// in.
fs::path absolute_environment_path(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return {};
    const fs::path path{std::string(value)};
    return path.is_absolute() ? path : fs::path{};
}

} // namespace

fs::path machine_plugin_folder()
{
    return fs::path("/Library/Application Support/VirtualSerialStudio/plugins");
}

fs::path user_plugin_folder()
{
    const fs::path home = absolute_environment_path("HOME");
    if (home.empty())
        return {};
    return home / "Library" / "Application Support" / "VirtualSerialStudio" /
           "plugins";
}

fs::path policy_file_path()
{
    return fs::path(
        "/Library/Application Support/VirtualSerialStudio/policy.json");
}

} // namespace vss::plugin::platform
