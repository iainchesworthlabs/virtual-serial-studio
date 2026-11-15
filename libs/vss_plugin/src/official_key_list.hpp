//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signed official key list, `keys/official-keys.json`, as text built into
// this library.  The definition is made by CMake (official_key_list.cpp.in).
// This header is not installed.
//

#pragma once

#include <string_view>

namespace vss::plugin::detail
{

std::string_view official_key_list_text() noexcept;

} // namespace vss::plugin::detail
