//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/licence_store.hpp"

namespace vss::gui
{

QString machine_licence_directory()
{
    return QStringLiteral("/etc/virtual-serial-studio/licences");
}

} // namespace vss::gui
