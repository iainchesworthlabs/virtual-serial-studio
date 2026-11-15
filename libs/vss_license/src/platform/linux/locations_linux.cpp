//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where licences are on Linux:
//
//   machine   /etc/virtual-serial-studio/licences
//   user      $XDG_DATA_HOME/virtual-serial-studio/licences, or
//             $HOME/.local/share/virtual-serial-studio/licences when
//             XDG_DATA_HOME is unset, empty or not an absolute path
//
// A value read from the environment is untrusted text and counts only when it
// is an absolute path.
//

#include <vss/license/store.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace vss::license
{

namespace
{

namespace fs = std::filesystem;

/// The value of an environment variable as a path, when it is set and is an
/// absolute path; an empty path in every other case. The text comes from
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

fs::path machine_licence_directory()
{
    return fs::path("/etc/virtual-serial-studio/licences");
}

fs::path user_licence_directory()
{
    fs::path data = absolute_environment_path("XDG_DATA_HOME");
    if (data.empty())
    {
        const fs::path home = absolute_environment_path("HOME");
        if (home.empty())
            return {};
        data = home / ".local" / "share";
    }
    return data / "virtual-serial-studio" / "licences";
}

} // namespace vss::license
