//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <string>

namespace vss_cli
{

/// The build this program is, as `vss_gui --version` writes it: the version,
/// the commits after its tag, the short commit and the CI run number, for
/// example `v0.1.0-rc.3+18.g3de4617.build.6083`.  It comes from a source file
/// that cmake/VssBuildStamp.cmake writes before every build.
std::string build_version();

} // namespace vss_cli
