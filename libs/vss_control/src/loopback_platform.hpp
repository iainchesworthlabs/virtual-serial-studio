//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <cstdint>

namespace vss::control::platform
{

/// Makes a listening socket's port its own: no other program can bind the
/// same address and port beside it, whatever options that program sets.
/// Windows has to be told; elsewhere it is so unless a socket asks for the
/// opposite, which this code never does.  A socket that cannot be set so is
/// left as it is: the bind that follows fails or succeeds on its own.
void make_listener_exclusive(std::uintptr_t native_socket) noexcept;

} // namespace vss::control::platform
