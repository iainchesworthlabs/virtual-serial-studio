//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/licence_text.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

#include <chrono>

namespace vss::gui::support
{
namespace
{

namespace lic = vss::license;

QString expiry_date(const lic::status& status)
{
    return status.value ? licence_date(status.value->expires_at) : QString();
}

QString start_date(const lic::status& status)
{
    return status.value ? licence_date(status.value->starts_at) : QString();
}

QString grace_end_date(const lic::status& status)
{
    return status.value ? licence_date(status.grace_ends) : QString();
}

QString edition_word(const lic::status& status)
{
    if (status.value && status.value->edition == lic::edition::enterprise)
        return QCoreApplication::translate("LicenceChip", "Enterprise");
    return QCoreApplication::translate("LicenceChip", "Pro");
}

/// The sentence for a licence that is not valid, by reason.
QString invalid_sentence(lic::reason reason, const QString& computer_name)
{
    switch (reason)
    {
    case lic::reason::newer_version:
        return QCoreApplication::translate(
            "LicenceDialog",
            "This licence needs a newer version of Studio. Install the latest "
            "version, then import the licence again.");
    case lic::reason::revoked:
        return QCoreApplication::translate(
            "LicenceDialog", "This licence was withdrawn. Contact support.");
    case lic::reason::wrong_computer:
        return QCoreApplication::translate(
                   "LicenceDialog",
                   "This licence is for other computers. This computer is %1. "
                   "Ask your administrator for a licence that names it.")
            .arg(computer_name);
    case lic::reason::malformed:
    case lic::reason::bad_signature:
    case lic::reason::ok:
    case lic::reason::no_licence:
    case lic::reason::expired:
    case lic::reason::not_yet_valid:
    case lic::reason::clock_suspect:
    case lic::reason::not_in_licence:
        break;
    }
    return QCoreApplication::translate(
        "LicenceDialog",
        "This file is not a valid Virtual Serial Studio licence. It may be "
        "damaged or incomplete. Ask for the file again.");
}

} // namespace

QString support_address()
{
    // This name is reserved (RFC 2606) and cannot receive mail. The owner sets
    // the real address here, and only here, before the first licence is
    // issued.
    return QStringLiteral("support@virtual-serial-studio.invalid");
}

QString documents_address()
{
    return QStringLiteral(
        "https://iainchesworthlabs.github.io/virtual-serial-studio/");
}

QString issues_address()
{
    return QStringLiteral(
        "https://github.com/iainchesworthlabs/virtual-serial-studio/issues");
}

QDate licence_local_date(lic::time_point value)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             value.time_since_epoch())
                             .count();
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds))
        .toLocalTime()
        .date();
}

QString licence_date(lic::time_point value)
{
    // The locale's long form without the day of the week: "13 November
    // 2027", not "Saturday, 13 November 2027". A term reads better without
    // it, and the name of the day is the only part taken out.
    const QLocale locale;
    static const QRegularExpression weekday(QStringLiteral("\\s*dddd,?\\s*"));
    const QString format = locale.dateFormat(QLocale::LongFormat)
                               .replace(weekday, QStringLiteral(" "))
                               .trimmed();
    return locale.toString(licence_local_date(value), format);
}

bool is_trial(const lic::status& status)
{
    return status.value && status.value->kind == lic::licence_kind::trial;
}

// -- edition -----------------------------------------------------------------

QString edition_label(const lic::status& status)
{
    if (!status.active() || !status.value)
        return QCoreApplication::translate("LicenceChip", "Community");
    if (is_trial(status))
        return QCoreApplication::translate("LicenceChip", "Pro trial");
    return edition_word(status);
}

QString edition_value(const lic::status& status)
{
    if (!status.active() || !status.value)
        return QCoreApplication::translate("LicenceDialog", "Community");
    if (is_trial(status))
        return QCoreApplication::translate("LicenceDialog", "Pro (trial)");
    if (status.value->edition == lic::edition::enterprise)
        return QCoreApplication::translate("LicenceDialog", "Enterprise");
    return QCoreApplication::translate("LicenceDialog", "Pro");
}

licence_chip edition_chip(const lic::status& status)
{
    const QString label = edition_label(status);
    switch (status.state)
    {
    case lic::state::none:
        return {label, licence_tone::neutral};
    case lic::state::valid:
        return {label, licence_tone::ok};
    case lic::state::expiring:
        if (status.days_left <= 0)
            return {QCoreApplication::translate("LicenceChip", "%1, ends today")
                        .arg(label),
                    licence_tone::warn};
        if (status.days_left == 1)
            return {QCoreApplication::translate("LicenceChip", "%1, 1 day left")
                        .arg(label),
                    licence_tone::warn};
        return {QCoreApplication::translate("LicenceChip", "%1, %n days left",
                                            nullptr, status.days_left)
                    .arg(label),
                licence_tone::warn};
    case lic::state::grace:
        return {QCoreApplication::translate("LicenceChip", "%1, licence ended")
                    .arg(label),
                licence_tone::warn};
    case lic::state::expired:
    case lic::state::not_yet_valid:
    case lic::state::clock_suspect:
    case lic::state::invalid:
        break;
    }
    return {QCoreApplication::translate("LicenceChip", "Community"),
            licence_tone::warn};
}

QString edition_chip_name(const licence_chip& chip)
{
    return QCoreApplication::translate("LicenceChip",
                                       "Edition: %1. Open the licence page.")
        .arg(chip.text);
}

// -- the Licence page
// ----------------------------------------------------------

licence_chip state_chip(const lic::status& status)
{
    switch (status.state)
    {
    case lic::state::none:
        return {QCoreApplication::translate("LicenceDialog", "Community"),
                licence_tone::neutral};
    case lic::state::valid:
        return {QCoreApplication::translate("LicenceDialog", "Valid"),
                licence_tone::ok};
    case lic::state::expiring:
        return {QCoreApplication::translate("LicenceDialog", "Ends soon"),
                licence_tone::warn};
    case lic::state::grace:
    case lic::state::expired:
        return {QCoreApplication::translate("LicenceDialog", "Ended"),
                licence_tone::warn};
    case lic::state::not_yet_valid:
        return {QCoreApplication::translate("LicenceDialog", "Not started"),
                licence_tone::neutral_hollow};
    case lic::state::clock_suspect:
        return {QCoreApplication::translate("LicenceDialog", "Check the date"),
                licence_tone::warn};
    case lic::state::invalid:
        break;
    }
    return {QCoreApplication::translate("LicenceDialog", "Not valid"),
            licence_tone::danger};
}

QString state_sentence(const lic::status& status, const QString& computer_name)
{
    switch (status.state)
    {
    case lic::state::none:
        return QCoreApplication::translate(
            "LicenceDialog",
            "No licence is installed. This is the Community edition: free "
            "software, complete for capturing, monitoring and sharing serial "
            "ports.");
    case lic::state::valid:
        return QCoreApplication::translate("LicenceDialog", "Valid until %1.")
            .arg(expiry_date(status));
    case lic::state::expiring:
        if (status.days_left <= 0)
            return QCoreApplication::translate("LicenceDialog",
                                               "Valid until %1. Ends today.")
                .arg(expiry_date(status));
        if (status.days_left == 1)
            return QCoreApplication::translate("LicenceDialog",
                                               "Valid until %1. 1 day left.")
                .arg(expiry_date(status));
        return QCoreApplication::translate("LicenceDialog",
                                           "Valid until %1. %n days left.",
                                           nullptr, status.days_left)
            .arg(expiry_date(status));
    case lic::state::grace:
        return QCoreApplication::translate(
                   "LicenceDialog",
                   "Ended on %1. Official plugins keep working until %2, then "
                   "switch off.")
            .arg(expiry_date(status), grace_end_date(status));
    case lic::state::expired:
        return QCoreApplication::translate(
                   "LicenceDialog",
                   "Ended on %1. Studio is running as the Community edition. "
                   "Your ports, setups and files are unchanged.")
            .arg(expiry_date(status));
    case lic::state::not_yet_valid:
        return QCoreApplication::translate("LicenceDialog", "Starts on %1.")
            .arg(start_date(status));
    case lic::state::clock_suspect:
        return QCoreApplication::translate(
            "LicenceDialog",
            "The date on this computer looks wrong. Official plugins are off "
            "until the date is right. If the date is right, create a support "
            "bundle and contact support.");
    case lic::state::invalid:
        break;
    }
    return invalid_sentence(status.reason, computer_name);
}

std::optional<licence_banner> state_note(const lic::status& status)
{
    switch (status.state)
    {
    case lic::state::valid:
        if (!is_trial(status))
            return std::nullopt;
        return licence_banner{
            licence_note_tone::info,
            QCoreApplication::translate(
                "LicenceDialog",
                "This is a 14 day trial of the Pro edition. It ends on %1. "
                "Nothing you make during the trial is lost.")
                .arg(expiry_date(status)),
            false};
    case lic::state::expiring:
        return licence_banner{
            licence_note_tone::warn,
            QCoreApplication::translate(
                "LicenceDialog",
                "Your licence ends on %1. Renew to keep official plugins and "
                "support.")
                .arg(expiry_date(status)),
            true};
    case lic::state::grace:
        return licence_banner{
            licence_note_tone::warn,
            QCoreApplication::translate(
                "LicenceDialog",
                "Your licence ended on %1. Official plugins switch off on %2.")
                .arg(expiry_date(status), grace_end_date(status)),
            true};
    case lic::state::expired:
        return licence_banner{
            licence_note_tone::info,
            QCoreApplication::translate(
                "LicenceDialog",
                "Official plugins are off. Nothing you made is lost: every "
                "port, setup and file still opens and saves."),
            true};
    case lic::state::none:
    case lic::state::not_yet_valid:
    case lic::state::clock_suspect:
    case lic::state::invalid:
        break;
    }
    return std::nullopt;
}

QString seats_text(int seats)
{
    if (seats <= 0)
        return QCoreApplication::translate("LicenceDialog",
                                           "Not counted by seat");
    if (seats == 1)
        return QCoreApplication::translate("LicenceDialog", "1 seat");
    return QCoreApplication::translate("LicenceDialog", "%n seats", nullptr,
                                       seats);
}

QString seat_explanation(const lic::licence& value)
{
    // A licence that is not counted by seat has nothing to explain.
    if (value.seats <= 0)
        return {};
    // What a seat covers in an Enterprise licence is part of what was
    // negotiated, so the page does not define it.
    if (value.edition == lic::edition::enterprise)
        return QCoreApplication::translate(
            "LicenceDialog", "As agreed in your Enterprise licence.");
    return QCoreApplication::translate(
        "LicenceDialog",
        "A seat is two computers: the one you work at and one other that you "
        "control or monitor from it. The serial device can be on either.");
}

QString term_text(const lic::licence& value)
{
    return QCoreApplication::translate("LicenceDialog", "%1 to %2")
        .arg(licence_date(value.starts_at), licence_date(value.expires_at));
}

QString feature_name(const QString& feature)
{
    if (feature == QLatin1String("plugins.official"))
        return QCoreApplication::translate("LicenceDialog", "Official plugins");
    if (feature == QLatin1String("driver.signed"))
        return QCoreApplication::translate("LicenceDialog",
                                           "Microsoft-signed driver");
    if (feature == QLatin1String("support"))
        return QCoreApplication::translate("LicenceDialog", "Support");
    return {};
}

QStringList known_features()
{
    return {QStringLiteral("plugins.official"), QStringLiteral("driver.signed"),
            QStringLiteral("support")};
}

licence_chip feature_chip(bool granted)
{
    if (granted)
        return {QCoreApplication::translate("LicenceDialog", "Available"),
                licence_tone::ok};
    return {QCoreApplication::translate("LicenceDialog", "Not available"),
            licence_tone::neutral_hollow};
}

QString reason_sentence(lic::reason reason, const lic::status& status)
{
    switch (reason)
    {
    case lic::reason::ok:
        return {};
    case lic::reason::no_licence:
        return QCoreApplication::translate("LicenceDialog",
                                           "Needs a Pro licence.");
    case lic::reason::not_in_licence:
        return QCoreApplication::translate(
            "LicenceDialog", "Your licence does not include this.");
    case lic::reason::expired:
        return QCoreApplication::translate("LicenceDialog",
                                           "Your licence ended on %1.")
            .arg(expiry_date(status));
    case lic::reason::not_yet_valid:
        return QCoreApplication::translate("LicenceDialog",
                                           "Your licence starts on %1.")
            .arg(start_date(status));
    case lic::reason::clock_suspect:
        return QCoreApplication::translate(
            "LicenceDialog", "The date on this computer looks wrong.");
    case lic::reason::wrong_computer:
        return QCoreApplication::translate(
            "LicenceDialog", "Your licence is for other computers.");
    case lic::reason::malformed:
    case lic::reason::newer_version:
    case lic::reason::bad_signature:
    case lic::reason::revoked:
        break;
    }
    return QCoreApplication::translate("LicenceDialog",
                                       "The installed licence is not valid.");
}

// -- import
// ---------------------------------------------------------------------

QString imported_toast(const lic::status& status)
{
    // The edition word alone: a trial is still the Pro edition.
    const QString edition =
        status.value && status.value->edition == lic::edition::enterprise
            ? QCoreApplication::translate("LicenceImport", "Enterprise")
            : QCoreApplication::translate("LicenceImport", "Pro");
    return QCoreApplication::translate(
               "LicenceImport", "Licence imported. This is now the %1 edition.")
        .arg(edition);
}

QString already_installed_toast()
{
    return QCoreApplication::translate("LicenceImport",
                                       "That licence is already installed.");
}

QString not_available_text()
{
    return QCoreApplication::translate("LicenceDialog",
                                       "Not available in this build");
}

std::optional<licence_banner> import_note(const lic::status& status)
{
    switch (status.state)
    {
    case lic::state::grace:
    case lic::state::expired:
        return licence_banner{
            licence_note_tone::warn,
            QCoreApplication::translate(
                "LicenceImport",
                "This licence ended on %1. It was imported, but it does not "
                "turn anything on.")
                .arg(expiry_date(status)),
            false};
    case lic::state::not_yet_valid:
        return licence_banner{licence_note_tone::info,
                              QCoreApplication::translate(
                                  "LicenceImport", "This licence starts on %1.")
                                  .arg(start_date(status)),
                              false};
    case lic::state::none:
    case lic::state::valid:
    case lic::state::expiring:
    case lic::state::clock_suspect:
    case lic::state::invalid:
        break;
    }
    return std::nullopt;
}

QString import_failure_body(const vss::gui::licence_import_result& result,
                            const QString& file_name,
                            const QString& computer_name)
{
    if (result.outcome == vss::gui::licence_import_outcome::not_valid)
        return invalid_sentence(result.status.reason, computer_name);
    return QCoreApplication::translate(
               "LicenceImport",
               "Studio could not read %1. Check that the file still exists and "
               "that you are allowed to open it.")
        .arg(file_name);
}

QString reason_code(lic::reason reason)
{
    switch (reason)
    {
    case lic::reason::ok:
        return QStringLiteral("ok");
    case lic::reason::no_licence:
        return QStringLiteral("no_licence");
    case lic::reason::expired:
        return QStringLiteral("expired");
    case lic::reason::not_yet_valid:
        return QStringLiteral("not_yet_valid");
    case lic::reason::clock_suspect:
        return QStringLiteral("clock_suspect");
    case lic::reason::malformed:
        return QStringLiteral("malformed");
    case lic::reason::newer_version:
        return QStringLiteral("newer_version");
    case lic::reason::bad_signature:
        return QStringLiteral("bad_signature");
    case lic::reason::revoked:
        return QStringLiteral("revoked");
    case lic::reason::wrong_computer:
        return QStringLiteral("wrong_computer");
    case lic::reason::not_in_licence:
        return QStringLiteral("not_in_licence");
    }
    return QStringLiteral("unknown");
}

QString import_failure_details(const vss::gui::licence_import_result& result,
                               const QString& file_name)
{
    QStringList lines;
    lines << QStringLiteral("File: %1").arg(file_name);
    if (result.outcome == vss::gui::licence_import_outcome::not_valid)
    {
        lines << QStringLiteral("Reason: %1")
                     .arg(reason_code(result.status.reason));
        if (!result.detail.isEmpty())
            lines << QStringLiteral("Message: %1").arg(result.detail);
    }
    else
    {
        lines << QStringLiteral("Reason: unreadable");
    }
    return lines.join(QLatin1Char('\n'));
}

// -- notices
// ----------------------------------------------------------------------

std::optional<licence_notice_text> notice_for(const lic::status& status)
{
    switch (status.state)
    {
    case lic::state::expiring:
        return licence_notice_text{
            notice_kind::expiring, licence_note_tone::info,
            QCoreApplication::translate("LicenceNotice",
                                        "Your licence ends on %1.")
                .arg(expiry_date(status))};
    case lic::state::grace:
    {
        QString text;
        if (status.days_left <= 0)
            text = QCoreApplication::translate(
                       "LicenceNotice",
                       "Your licence ended on %1. Official plugins switch off "
                       "today.")
                       .arg(expiry_date(status));
        else if (status.days_left == 1)
            text = QCoreApplication::translate(
                       "LicenceNotice",
                       "Your licence ended on %1. Official plugins switch off "
                       "tomorrow.")
                       .arg(expiry_date(status));
        else
            text = QCoreApplication::translate(
                       "LicenceNotice",
                       "Your licence ended on %1. Official plugins switch off "
                       "in %n days.",
                       nullptr, status.days_left)
                       .arg(expiry_date(status));
        return licence_notice_text{notice_kind::grace, licence_note_tone::warn,
                                   text};
    }
    case lic::state::expired:
        return licence_notice_text{
            notice_kind::expired, licence_note_tone::info,
            QCoreApplication::translate(
                "LicenceNotice",
                "Your licence has ended, so official plugins are off. Studio "
                "is now the Community edition. Your ports, setups and files "
                "are unchanged.")};
    case lic::state::clock_suspect:
        return licence_notice_text{
            notice_kind::clock_suspect, licence_note_tone::warn,
            QCoreApplication::translate(
                "LicenceNotice",
                "The date on this computer looks wrong, so official plugins "
                "are off.")};
    case lic::state::none:
    case lic::state::valid:
    case lic::state::not_yet_valid:
    case lic::state::invalid:
        break;
    }
    return std::nullopt;
}

QString notice_second_action(notice_kind kind)
{
    if (kind == notice_kind::grace)
        return QCoreApplication::translate("LicenceNotice",
                                           "Remind me tomorrow");
    return QCoreApplication::translate("LicenceNotice", "Dismiss");
}

// -- the renew dialog
// --------------------------------------------------------------

QString renew_title()
{
    return QCoreApplication::translate("LicenceDialog", "Renew your licence");
}

QString renew_text(const QString& licence_id)
{
    return QCoreApplication::translate(
               "LicenceDialog",
               "A licence is renewed by installing a new licence file. Write "
               "to %1 and quote your licence number, %2. Studio does not open "
               "a web page or send anything itself.")
        .arg(support_address(), licence_id);
}

} // namespace vss::gui::support
