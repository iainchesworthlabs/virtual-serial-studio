//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This computer's name, as a licence's `machines` list is compared with it.
//

#pragma once

#include <string>
#include <string_view>

namespace vss::machine
{

/// This computer's name, cut at the first '.', in UTF-8.  Empty when the
/// system gives none.
///
/// The system's name is `GetComputerNameExW(ComputerNameDnsHostname)` on
/// Windows, `gethostname` on Linux, and on macOS the name in System Settings
/// (Sharing, "Local hostname") with `gethostname` when that gives nothing.
/// Cutting at the first '.' leaves `lab-rig-07` of `lab-rig-07.local` and of
/// `lab-rig-07.corp.example.com`, so the name does not change with the
/// network.
std::string computer_name();

/// `full` up to its first '.'; all of it when it has none.  Exposed for its
/// test.
std::string short_computer_name(std::string_view full);

} // namespace vss::machine
