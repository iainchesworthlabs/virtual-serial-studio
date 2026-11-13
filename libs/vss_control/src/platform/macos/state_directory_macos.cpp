//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the gateway keeps its state on macOS:
// `$HOME/Library/Application Support/Virtual Serial Studio`.
//

#include "platform/private_storage_platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace vss::control::platform
{
namespace
{

std::string environment(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

} // namespace

result<std::filesystem::path> state_directory()
{
    const std::string home = environment("HOME");
    if (home.empty())
        return make_error_code(errc::not_found);
    return std::filesystem::path(home) / "Library" / "Application Support" /
           "Virtual Serial Studio";
}

} // namespace vss::control::platform
