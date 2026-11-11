//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform/process_platform.hpp"

#include <unistd.h>

namespace vss::agent::platform
{

std::uint64_t process_id() noexcept
{
    return static_cast<std::uint64_t>(::getpid());
}

} // namespace vss::agent::platform
