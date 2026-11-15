//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Linux and macOS ask the system in the same way: a path from the
// environment, and the host name.  Only the files of those two systems
// include this header; it is not installed.
//

#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace vss::machine::platform
{

/// The value of an environment variable as a path, when it is set and is an
/// absolute path; an empty path in every other case.  The text comes from
/// outside, and a relative path would depend on the folder the program runs
/// in.
inline std::filesystem::path absolute_environment_path(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return {};
    const std::filesystem::path path{std::string(value)};
    return path.is_absolute() ? path : std::filesystem::path{};
}

/// What `gethostname` gives, or an empty string when it fails.
inline std::string posix_host_name()
{
    // Longer than any host name the systems allow, with room for the end.
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0)
        return {};
    return std::string(buffer.data());
}

} // namespace vss::machine::platform
