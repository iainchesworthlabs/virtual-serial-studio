//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a screen is told about licences (docs/licensing.md and
// docs/gui-design/docs/screens/09-licence-and-support.md). A screen never
// reads a licence file and never decides a state; it shows these values.
//

#pragma once

#include <vss/license/license.hpp>

#include <QMetaType>
#include <QString>

#include <vector>

namespace vss::gui
{

/// One licence file Studio found.
struct licence_file_view
{
    vss::license::status status;
    /// The file's name without its folder.
    QString file_name;
    /// In the per-machine folder: an administrator put it there. Studio only
    /// reads it, so the page shows no Remove button for it.
    bool machine_wide = false;
    /// The file Studio is using now.
    bool in_use = false;
};

/// The licence Studio is using and the other files it found.
struct licence_view
{
    /// `state::none` when there is no licence: the Community edition.
    vss::license::status status = vss::license::no_licence();
    /// The name the licence library compared with a licence's `machines`.
    QString computer_name;
    /// Every file found, the one in use first. Empty for the Community
    /// edition with no files.
    std::vector<licence_file_view> files;
};

enum class licence_import_outcome
{
    /// The file was copied to the user's licence folder. `status` says what
    /// it is worth: it may be valid, ended or not started yet.
    imported,
    /// The same licence is installed already. Nothing was copied.
    already_installed,
    /// The file is not a licence Studio accepts. Nothing was copied.
    /// `status.reason` says why.
    not_valid,
    /// The file could not be read. Nothing was copied.
    unreadable,
    /// This backend keeps no licences.
    unsupported
};

struct licence_import_result
{
    licence_import_outcome outcome = licence_import_outcome::unsupported;
    /// The imported file's own status. For `not_valid` the reason; unset
    /// otherwise.
    vss::license::status status = vss::license::no_licence();
    /// The library's message for a file that was refused. For "Copy
    /// details"; never shown as the explanation.
    QString detail;
};

} // namespace vss::gui

Q_DECLARE_METATYPE(vss::gui::licence_view)
