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

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <optional>

namespace vss::gui::support
{

/// A file's size as the update screens write it: in MB, with no decimals
/// from 10 MB up and one below, in the user's locale ("48 MB", "3.2 MB").
QString update_size_text(std::uint64_t bytes);

/// How this copy was installed, as the About dialog says it after the
/// version: "installed for everyone on this computer", "unpacked from an
/// archive" or "copied from the disk image".
QString install_kind_text(vss::update::file_kind kind);

/// The status sentence of the Updates tab, the whole table "The status
/// sentence" of the spec. `running_version` is the version of this program,
/// which the `current` sentence names; the sentence of an offer names the
/// offer's own version. A developer build that has checked says what the
/// newest release is, or reads as `current` when the feed named none.
QString update_status_text(const updates::snapshot& state,
                           const QString& running_version);

/// "This version" row: "<version>, <kind of install>", or the version alone
/// while the kind of install is not known.
QString
update_this_version_text(const QString& running_version,
                         const std::optional<vss::update::file_kind>& kind);

/// "Last checked" row: the time in the user's locale and local time, or
/// "Never" when `when` is not a time.
QString update_last_checked_text(const QDateTime& when);

/// The question of the first start, as one sentence.
QString update_question_text();
/// "Virtual Serial Studio <version> is available."
QString update_notice_text(const QString& version);
/// The toast that follows an answer of the question.
QString update_answer_toast(bool turned_on);
/// The words of the status bar chip, and its accessible name.
QString update_chip_text();
QString update_chip_name(const QString& version);

} // namespace vss::gui::support
