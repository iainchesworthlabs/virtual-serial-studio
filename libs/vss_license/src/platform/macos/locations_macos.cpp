//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where licences are on macOS:
//
//   machine   /Library/Application Support/Virtual Serial Studio/licences
//   user      $HOME/Library/Application Support/Virtual Serial Studio/licences
//
// $HOME counts only when it is an absolute path.
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
    return fs::path(
        "/Library/Application Support/Virtual Serial Studio/licences");
}

fs::path user_licence_directory()
{
    const fs::path home = absolute_environment_path("HOME");
    if (home.empty())
        return {};
    return home / "Library" / "Application Support" / "Virtual Serial Studio" /
           "licences";
}

} // namespace vss::license
