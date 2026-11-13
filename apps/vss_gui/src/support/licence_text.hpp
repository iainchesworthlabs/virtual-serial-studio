//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of the licence screens (docs/gui-design/docs/screens/
// 09-licence-and-support.md). Each function turns a value the backend
// reports into the text and chip of the spec's tables. None of them reads a
// file or a clock and none builds a sentence by joining translated pieces, so
// every state can be tested without a window.
//

#pragma once

#include "backend/licence_view.hpp"

#include <vss/license/license.hpp>

#include <QDate>
#include <QString>

#include <optional>

namespace vss::gui::support
{

/// The look of a chip: the words and which of the shared chip kinds to draw.
/// A tone is always drawn with a dot shape as well, never as colour alone.
enum class licence_tone
{
    neutral,
    /// Neutral with a hollow dot.
    neutral_hollow,
    ok,
    warn,
    danger
};

struct licence_chip
{
    QString text;
    licence_tone tone = licence_tone::neutral;
};

/// The two kinds of note the licence screens show.
enum class licence_note_tone
{
    info,
    warn
};

struct licence_note
{
    licence_note_tone tone = licence_note_tone::info;
    QString text;
    /// Offers "How to renew".
    bool renew = false;
};

/// Where to write about a licence. The one place to change it. The name is a
/// reserved one that cannot receive mail: the owner sets the real address
/// before the first licence is issued.
QString support_address();

/// A date in the user's locale, long form, in local time ("13 November
/// 2027").
QString licence_date(vss::license::time_point value);
QDate licence_local_date(vss::license::time_point value);

/// A trial licence of the Pro edition.
bool is_trial(const vss::license::status& status);

// -- edition -----------------------------------------------------------------

/// "Community", "Pro", "Enterprise" or "Pro trial": what the licence turns
/// on now. A licence that is not in force is the Community edition.
QString edition_label(const vss::license::status& status);

/// The "Edition" row of the summary: as `edition_label`, with "Pro (trial)".
QString edition_value(const vss::license::status& status);

/// The status bar chip.
licence_chip edition_chip(const vss::license::status& status);

/// "Edition: <text>. Open the licence page."
QString edition_chip_name(const licence_chip& chip);

// -- the Licence page
// ----------------------------------------------------------

/// The chip beside the edition: "Valid", "Ends soon" and so on.
licence_chip state_chip(const vss::license::status& status);

/// The state sentence. `computer_name` is what the licence library compared
/// with the licence.
QString state_sentence(const vss::license::status& status,
                       const QString& computer_name);

/// The note under the summary card, for the states that need one.
std::optional<licence_note> state_note(const vss::license::status& status);

/// "<n> seats", "1 seat" or "Site licence".
QString seats_text(int seats);
QString seat_explanation();
/// "<start date> to <expiry date>".
QString term_text(const vss::license::licence& value);

/// The name of a feature the licence service knows ("plugins.official"), or
/// empty for one the page has no name for.
QString feature_name(const QString& feature);
/// The features the table lists, in order.
QStringList known_features();

licence_chip feature_chip(bool granted);
/// The sentence for a feature that is not granted. Empty for `reason::ok`.
/// The same sentence is the tooltip of a disabled control elsewhere.
QString reason_sentence(vss::license::reason reason,
                        const vss::license::status& status);

// -- import
// ---------------------------------------------------------------------

/// "Licence imported. This is now the <edition> edition."
QString imported_toast(const vss::license::status& status);
QString already_installed_toast();
QString not_available_text();

/// The banner of a licence that was imported but turns nothing on.
std::optional<licence_note> import_note(const vss::license::status& status);

/// The body of "Could not import the licence": the sentence for the reason,
/// or for a file that could not be read.
QString import_failure_body(const vss::gui::licence_import_result& result,
                            const QString& file_name,
                            const QString& computer_name);

/// For "Copy details": the file name, the reason code and the library's
/// message. Never the file's contents. Plain English on purpose: it is for
/// the person who answers.
QString import_failure_details(const vss::gui::licence_import_result& result,
                               const QString& file_name);

/// The reason as the licence library names it ("bad_signature").
QString reason_code(vss::license::reason reason);

// -- notices
// ----------------------------------------------------------------------

enum class notice_kind
{
    expiring,
    grace,
    expired,
    clock_suspect
};

struct licence_notice_text
{
    notice_kind kind = notice_kind::expiring;
    licence_note_tone tone = licence_note_tone::info;
    QString text;
};

/// The banner for the main window, or nothing when the state needs none.
std::optional<licence_notice_text>
notice_for(const vss::license::status& status);

/// The word on the second button: "Dismiss" or "Remind me tomorrow".
QString notice_second_action(notice_kind kind);

// -- the renew dialog
// --------------------------------------------------------------

QString renew_title();
QString renew_text(const QString& licence_id);

} // namespace vss::gui::support
