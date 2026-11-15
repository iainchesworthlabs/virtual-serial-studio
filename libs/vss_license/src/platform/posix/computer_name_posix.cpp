//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The name of this computer on Linux and macOS: the host name.
//

#include <vss/license/store.hpp>

#include <unistd.h>

#include <array>
#include <string>

namespace vss::license
{

std::string computer_name()
{
    // HOST_NAME_MAX is 64 on Linux and 255 on macOS; one more for the end.
    std::array<char, 256> name{};
    if (::gethostname(name.data(), name.size() - 1) != 0)
        return {};
    return std::string(name.data());
}

} // namespace vss::license
