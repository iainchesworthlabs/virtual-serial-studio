//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// macOS: the numeric user id.

#include "support/account_id.hpp"

#include <unistd.h>

namespace vss::gui::support
{

QString account_id()
{
    return QString::number(::getuid());
}

} // namespace vss::gui::support
