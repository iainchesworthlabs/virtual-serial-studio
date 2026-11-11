//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the agent asks the operating system about its own process.  One
// implementation per platform family in src/platform/<windows|posix>; CMake
// picks the file.
//

#pragma once

#include <cstdint>

namespace vss::agent::platform
{

/// The operating system's number for this process.
std::uint64_t process_id() noexcept;

} // namespace vss::agent::platform
