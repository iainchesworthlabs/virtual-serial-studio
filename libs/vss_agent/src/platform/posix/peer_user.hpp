//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one question about a Unix socket that Linux and macOS answer with
// different calls: who is at the other end?  Linux reads SO_PEERCRED
// (src/platform/linux) and macOS calls getpeereid (src/platform/macos).
//

#pragma once

#include <vss/core/error.hpp>

#include <cstdint>

namespace vss::agent::platform
{

/// The effective user id of the process at the other end of the connected
/// Unix socket `descriptor`, as it was when the connection was made.
result<std::uint32_t> peer_user(int descriptor);

} // namespace vss::agent::platform
