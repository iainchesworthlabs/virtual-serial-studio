//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/install_kind.hpp"

#include <QSysInfo>

namespace vss::gui::updates
{

vss::update::architecture built_architecture()
{
    // Qt names the 64-bit ARM build "arm64" on every system.
    return QSysInfo::buildCpuArchitecture() == QLatin1String("arm64")
               ? vss::update::architecture::arm64
               : vss::update::architecture::x86_64;
}

} // namespace vss::gui::updates
