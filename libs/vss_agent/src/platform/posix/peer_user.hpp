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
#include <string>

namespace vss::agent::platform
{

/// The effective user id of the process at the other end of the connected
/// Unix socket `descriptor`, as it was when the connection was made.
result<std::uint32_t> peer_user(int descriptor);

/// The process at the other end of the connected Unix socket `descriptor`, as
/// it was when the connection was made, or 0 when the system does not say.
std::uint64_t peer_process(int descriptor) noexcept;

/// The path of the program that process `process` runs, or nothing when it
/// cannot be read.  The process may have ended and its number been given to
/// another since the connection was made, so this is for a log only.
std::string process_program(std::uint64_t process);

} // namespace vss::agent::platform
