//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "loopback_platform.hpp"

#include <winsock2.h>

namespace vss::control::platform
{

void make_listener_exclusive(std::uintptr_t native_socket) noexcept
{
    // Without this another program of any user could bind the same port
    // with SO_REUSEADDR and take connections meant for the gateway.
    const BOOL exclusive = TRUE;
    static_cast<void>(::setsockopt(static_cast<SOCKET>(native_socket),
                                   SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                   reinterpret_cast<const char*>(&exclusive),
                                   static_cast<int>(sizeof(exclusive))));
}

} // namespace vss::control::platform
