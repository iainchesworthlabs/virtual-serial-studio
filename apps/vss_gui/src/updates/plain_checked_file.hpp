//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A checked file kept open through `QFile`, for the systems that have no
// way to deny other programs writing to an open file (macOS and Linux).
// There the checked file is handed to a system tool that asks for its own
// confirmation; the order is still open, then hash, then hand over
// (docs/updates.md, section 7.2).
//

#pragma once

#include "updates/checked_file.hpp"

#include <QString>

#include <memory>

namespace vss::gui::updates
{

/// Opens `path` for reading. Null when it cannot be opened, is not a plain
/// file, or is a link.
std::unique_ptr<checked_file> open_plain_checked_file(const QString& path);

} // namespace vss::gui::updates
