//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the gateway keeps its state on Linux: `$XDG_STATE_HOME` when it is set
// to an absolute path, otherwise `$HOME/.local/state`, with the product's
// directory `virtual-serial-studio` added.
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
    // The specification asks for the variable to be ignored when it is not an
    // absolute path.
    const std::string state = environment("XDG_STATE_HOME");
    if (!state.empty())
    {
        const std::filesystem::path base(state);
        if (base.is_absolute())
            return base / "virtual-serial-studio";
    }

    const std::string home = environment("HOME");
    if (home.empty())
        return make_error_code(errc::not_found);
    return std::filesystem::path(home) / ".local" / "state" /
           "virtual-serial-studio";
}

} // namespace vss::control::platform
