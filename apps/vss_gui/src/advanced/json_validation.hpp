//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_ADVANCED_JSON_VALIDATION_HPP
#define VSS_GUI_ADVANCED_JSON_VALIDATION_HPP

#include <vss/config/document.hpp>

#include <QString>
#include <QVector>

#include <optional>

namespace vss::gui
{

/// One line in the Checks list.
enum class check_state
{
    passed,
    failed,
    not_checked
};

struct json_check
{
    QString text;
    check_state state = check_state::not_checked;
};

/// A validation problem.  `line` is 1-based, or 0 when the engine reported a
/// schema error without a position (then the UI hides "Go to line").
struct json_problem
{
    QString summary;
    QString detail;
    QString fix_from;
    QString fix_to;
    int line = 0;

    bool has_line() const noexcept { return line > 0; }
    bool has_fix() const noexcept
    {
        return !fix_from.isEmpty() && !fix_to.isEmpty();
    }
};

/// The result of checking the editor text, as the toolbar, the Checks list and
/// Apply read it.
struct json_validation
{
    bool json_valid = false;
    bool schema_valid = false;
    bool valid() const noexcept { return json_valid && schema_valid; }
    int version = 0;
    int ports = 0;
    int links = 0;
    QString name;
    QVector<json_check> checks;
    std::optional<json_problem> problem;
    vss::config::document document;
    QString formatted;

    /// "Valid, version 1", or the problem summary for the status chip.
    QString status_text() const;
};

/// Parses and checks a topology document.
///
/// Syntax offsets come from `QJsonDocument`; schema problems are found with
/// explicit checks so they can name the field, and finally the engine's
/// `vss::config::from_json` decides.  A document is only `valid()` when the
/// engine accepts it.
///
/// `shared_bus` says whether the backend can make a bus link
/// (`capabilities::shared_bus`).  Without it a `"mode": "bus"` link is refused
/// with a message that says so, instead of the engine's generic one.
/// `half_duplex_bus` says the same for a bus that is a half-duplex line
/// (`capabilities::half_duplex_bus`): without it a link with a
/// `"half_duplex"` member is refused, on the line of that member.
json_validation validate_json_document(const QString& text,
                                       bool shared_bus = true,
                                       bool half_duplex_bus = true);

/// The 1-based line of a character offset.  Exposed for tests.
int line_for_offset(const QString& text, int offset);

} // namespace vss::gui

#endif // VSS_GUI_ADVANCED_JSON_VALIDATION_HPP
