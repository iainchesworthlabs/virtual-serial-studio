//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "loopback_platform.hpp"

namespace vss::control::platform
{

void make_listener_exclusive(std::uintptr_t /*native_socket*/) noexcept
{
    // A port is one listener's here unless both ask to share it
    // (SO_REUSEPORT), and the gateway never asks.
}

} // namespace vss::control::platform
