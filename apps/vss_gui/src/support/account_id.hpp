//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QString>

namespace vss::gui::support
{

/// The account this process runs as, in the form the system itself uses: the
/// security identifier on Windows (`S-1-5-21-...`), the numeric user id on
/// Linux and macOS.  Unlike a user name from the environment it cannot be
/// changed or emptied by whoever starts the program, and two accounts never
/// share one.  Empty when the system cannot be asked.
QString account_id();

} // namespace vss::gui::support
