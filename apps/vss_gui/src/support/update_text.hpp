//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of the update screens that more than one screen uses
// (docs/gui-design/docs/screens/12-updates.md). No widgets here, so every
// sentence has a test.
//

#pragma once

#include "updates/update_types.hpp"

#include <vss/update/feed.hpp>

#include <QString>

#include <cstdint>

namespace vss::gui::support
{

/// A file's size as the update screens write it: in MB, with no decimals
/// from 10 MB up and one below, in the user's locale ("48 MB", "3.2 MB").
QString update_size_text(std::uint64_t bytes);

/// How this copy was installed, as the About dialog says it after the
/// version: "installed for everyone on this computer", "unpacked from an
/// archive" or "copied from the disk image".
QString install_kind_text(vss::update::file_kind kind);

} // namespace vss::gui::support
