//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform/process_platform.hpp"

#include <windows.h>

namespace vss::agent::platform
{

std::uint64_t process_id() noexcept
{
    return ::GetCurrentProcessId();
}

} // namespace vss::agent::platform
