//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/bundle.hpp"

#include "support/build_info.hpp"
#include "support/redact.hpp"
#include "support/setup_facts.hpp"

#include <vss/config/document.hpp>
#include <vss/core/profile.hpp>
#include <vss/license/license.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QSysInfo>

#include <utility>

namespace vss::gui::support
{
namespace
{

namespace lic = vss::license;

QString csv_field(const QString& value)
{
    if (!value.contains(QLatin1Char(',')) &&
        !value.contains(QLatin1Char('"')) && !value.contains(QLatin1Char('\n')))
    {
        return value;
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString counters_csv(const StudioBackend& backend)
{
    QString out =
        QStringLiteral("name,path,state,rx_bytes,tx_bytes,dropped,errors\n");
    for (const port_view& port : backend.ports())
    {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
                   .arg(csv_field(port.name), csv_field(port.path),
                        port_state_label(port.state))
                   .arg(port.stats.bytes_rx)
                   .arg(port.stats.bytes_tx)
                   .arg(port.stats.dropped)
                   .arg(port.stats.errors);
    }
    return out;
}

QString setup_json(const StudioBackend& backend)
{
    const auto document = vss::config::to_json(backend.snapshot());
    if (!document)
    {
        return QCoreApplication::translate("Bundle",
                                           "The setup could not be written: %1")
            .arg(QString::fromStdString(document.error().message()));
    }
    return QString::fromStdString(*document);
}

/// The edition in plain English, for a helper to read.
QString edition_word(const lic::status& status)
{
    if (!status.value || !status.active())
        return QStringLiteral("Community");
    const bool enterprise = status.value->edition == lic::edition::enterprise;
    const bool trial = status.value->kind == lic::licence_kind::trial;
    if (enterprise)
        return QStringLiteral("Enterprise");
    return trial ? QStringLiteral("Pro (trial)") : QStringLiteral("Pro");
}

QString state_word(lic::state value)
{
    switch (value)
    {
    case lic::state::none:
        return QStringLiteral("none");
    case lic::state::valid:
        return QStringLiteral("valid");
    case lic::state::expiring:
        return QStringLiteral("expiring");
    case lic::state::grace:
        return QStringLiteral("grace");
    case lic::state::expired:
        return QStringLiteral("expired");
    case lic::state::not_yet_valid:
        return QStringLiteral("not yet valid");
    case lic::state::clock_suspect:
        return QStringLiteral("clock suspect");
    case lic::state::invalid:
        return QStringLiteral("invalid");
    }
    return QStringLiteral("unknown");
}

QString reason_word(lic::reason value)
{
    switch (value)
    {
    case lic::reason::ok:
        return QStringLiteral("ok");
    case lic::reason::no_licence:
        return QStringLiteral("no licence");
    case lic::reason::expired:
        return QStringLiteral("expired");
    case lic::reason::not_yet_valid:
        return QStringLiteral("not yet valid");
    case lic::reason::clock_suspect:
        return QStringLiteral("clock suspect");
    case lic::reason::malformed:
        return QStringLiteral("malformed");
    case lic::reason::newer_version:
        return QStringLiteral("newer version");
    case lic::reason::bad_signature:
        return QStringLiteral("bad signature");
    case lic::reason::revoked:
        return QStringLiteral("revoked");
    case lic::reason::wrong_computer:
        return QStringLiteral("wrong computer");
    case lic::reason::not_in_licence:
        return QStringLiteral("not in licence");
    }
    return QStringLiteral("unknown");
}

QString environment_text(const StudioBackend& backend)
{
    QString out = diagnostic_report();
    if (!out.endsWith(QLatin1Char('\n')))
        out += QLatin1Char('\n');
    out += QStringLiteral("Edition: %1\n")
               .arg(edition_word(backend.licence().status));
    return out;
}

QString problem_word(driver_problem value)
{
    switch (value)
    {
    case driver_problem::none:
        return QStringLiteral("none");
    case driver_problem::driver_missing:
        return QStringLiteral("driver missing");
    case driver_problem::helper_missing:
        return QStringLiteral("helper service missing");
    case driver_problem::helper_stopped:
        return QStringLiteral("helper service stopped");
    case driver_problem::helper_incompatible:
        return QStringLiteral("helper service incompatible");
    case driver_problem::helper_denied:
        return QStringLiteral("helper service refused this user");
    case driver_problem::unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString yes_no(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

QString driver_text(const StudioBackend& backend)
{
    const setup_facts facts = backend.facts();
    QString out;
    out += QStringLiteral("Platform: %1\n").arg(facts.platform);
    out += QStringLiteral("Studio driver installed: %1\n")
               .arg(yes_no(facts.driver_found));
    out += QStringLiteral("Studio driver version: %1\n")
               .arg(facts.driver_version.isEmpty() ? QStringLiteral("not known")
                                                   : facts.driver_version);
    // Nothing reports how the installed driver is signed, so it is not
    // guessed.
    out += QStringLiteral("Studio driver signing: not known\n");
    out += QStringLiteral("Driver set-up problem: %1\n")
               .arg(problem_word(facts.driver.problem));
    if (!facts.driver.details.isEmpty())
        out += QStringLiteral("Driver set-up details: %1\n")
                   .arg(facts.driver.details);
    out += QStringLiteral("com0com installed: %1\n")
               .arg(yes_no(facts.com0com_found));
    out += QStringLiteral("com0com pairs: %1\n").arg(facts.com0com_pairs);
    out += QStringLiteral("Serial devices: %1\n").arg(facts.serial_devices);
    out += QStringLiteral("Running elevated: %1\n")
               .arg(facts.elevation_known ? yes_no(facts.elevated)
                                          : QStringLiteral("not known"));

    out += QStringLiteral("\nSetup check\n");
    const setup_report report = query_setup_report(backend);
    out += QStringLiteral("Result: %1\n").arg(report.header_chip);
    for (const setup_row& row : report.rows)
    {
        out += QStringLiteral("- %1").arg(row.title);
        if (!row.chip.isEmpty())
            out += QStringLiteral(" [%1]").arg(row.chip);
        if (!row.sentence.isEmpty())
            out += QStringLiteral(": %1").arg(row.sentence);
        out += QLatin1Char('\n');
    }
    return out;
}

/// A device path for the bundle. A by-id path on Linux spells out the
/// serial number of the device, so only its folder is kept.
QString safe_device_path(const QString& path)
{
    const qsizetype by_id = path.indexOf(QStringLiteral("/by-id/"));
    if (by_id >= 0)
        return path.left(by_id) + QStringLiteral("/by-id/[redacted]");
    return path;
}

QString serial_ports_text(const StudioBackend& backend)
{
    QString out = QStringLiteral("# name | description | vendor:product | "
                                 "driver | kind\n");
    const auto devices = backend.devices();
    for (const device_view& device : devices)
    {
        // `capabilities::device_driver_names` is false in every backend:
        // none reports the driver behind a device, and one is never guessed.
        out += QStringLiteral("%1 | %2 | %3 | %4 | %5\n")
                   .arg(safe_device_path(device.path),
                        device.description.isEmpty()
                            ? QStringLiteral("no description")
                            : device.description,
                        bundle_hardware_summary(device.hardware_id),
                        QStringLiteral("not known"),
                        device.virtual_device ? QStringLiteral("virtual")
                                              : QStringLiteral("device"));
    }
    if (devices.empty())
        out += QStringLiteral("No serial ports found.\n");
    return out;
}

QString licence_file_text(const StudioBackend& backend,
                          const bundle_selection& selection)
{
    const licence_view view = backend.licence();
    const lic::status& status = view.status;
    QString out;
    out += QStringLiteral("State: %1\n").arg(state_word(status.state));
    out += QStringLiteral("Reason: %1\n").arg(reason_word(status.reason));
    out += QStringLiteral("Edition in use: %1\n").arg(edition_word(status));
    if (status.value)
    {
        const lic::licence& value = *status.value;
        out += QStringLiteral("Licence number: %1\n")
                   .arg(QString::fromStdString(value.licence_id));
        out +=
            QStringLiteral("Starts: %1\n")
                .arg(QString::fromStdString(lic::format_time(value.starts_at)));
        out += QStringLiteral("Expires: %1\n")
                   .arg(QString::fromStdString(
                       lic::format_time(value.expires_at)));
        out += QStringLiteral("Grace days: %1\n").arg(value.grace_days);
        out += value.seats == 0
                   ? QStringLiteral("Seats: not counted by seat\n")
                   : QStringLiteral("Seats: %1\n").arg(value.seats);
        QStringList features;
        for (const std::string& feature : value.features)
            features << QString::fromStdString(feature);
        out += QStringLiteral("Features: %1\n")
                   .arg(features.join(QStringLiteral(", ")));
        out += QStringLiteral("Signed with key: %1\n")
                   .arg(QString::fromStdString(status.key_id));
        out += QStringLiteral("Names computers: %1\n")
                   .arg(yes_no(!value.machines.empty()));
        if (selection.licensee_name)
            out += QStringLiteral("Licensed to: %1\n")
                       .arg(QString::fromStdString(value.licensee_name));
        if (selection.computer_name && !value.machines.empty())
        {
            QStringList machines;
            for (const std::string& machine : value.machines)
                machines << QString::fromStdString(machine);
            out += QStringLiteral("Computers in the licence: %1\n")
                       .arg(machines.join(QStringLiteral(", ")));
            out +=
                QStringLiteral("This computer: %1\n").arg(view.computer_name);
        }
    }
    out += QStringLiteral("Licence files found: %1\n").arg(view.files.size());
    return out;
}

/// Replaces `word` wherever it stands as a whole word, in any letter case.
void replace_word(QString& text, const QString& word, const QString& with,
                  qsizetype shortest)
{
    if (word.size() < shortest)
        return;
    const QRegularExpression pattern(
        QStringLiteral("(?<![\\p{L}\\p{N}_])%1(?![\\p{L}\\p{N}_])")
            .arg(QRegularExpression::escape(word)),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::UseUnicodePropertiesOption);
    text.replace(pattern, with);
}

/// What every file goes through before it is shown or written: secrets and
/// the home folder first, then the names of the computer and the user.
QByteArray cleaned(QString text, const bundle_identity& identity)
{
    text = redact_secrets(std::move(text));
    replace_word(text, identity.computer_name, QStringLiteral("[computer]"), 2);
    // A very short user name would also be an ordinary word.
    replace_word(text, identity.user_name, QStringLiteral("[user]"), 3);
    return text.toUtf8();
}

bundle_section section(const char* id, const char* title, const char* file,
                       QByteArray data, bool sensitive = false)
{
    bundle_section value;
    value.id = QLatin1String(id);
    value.title = QLatin1String(title);
    value.files.push_back({QLatin1String(file), std::move(data)});
    value.sensitive = sensitive;
    return value;
}

} // namespace

QVector<bundle_file> bundle_contents::files() const
{
    QVector<bundle_file> all;
    all.push_back(manifest);
    for (const bundle_section& part : sections)
        all += part.files;
    return all;
}

bundle_identity current_bundle_identity()
{
    bundle_identity identity;
    identity.computer_name = QSysInfo::machineHostName();
    // The last part of the home folder is the account name on every system
    // Studio runs on, and it is the form that reaches paths and logs.
    identity.user_name = QDir::home().dirName();
    return identity;
}

QString bundle_default_name(const QDate& date)
{
    return QStringLiteral("vss-support-%1.zip")
        .arg(date.toString(QStringLiteral("yyyy-MM-dd")));
}

bool bundle_has_licence(const StudioBackend& backend)
{
    return backend.licence().status.value.has_value();
}

bool bundle_licence_names_computers(const StudioBackend& backend)
{
    const lic::status status = backend.licence().status;
    return status.value && !status.value->machines.empty();
}

QString port_state_label(vss::endpoint_state state)
{
    switch (state)
    {
    case vss::endpoint_state::created:
        return QCoreApplication::translate("Bundle", "Created");
    case vss::endpoint_state::opening:
        return QCoreApplication::translate("Bundle", "Opening");
    case vss::endpoint_state::open:
        return QCoreApplication::translate("Bundle", "Open");
    case vss::endpoint_state::closed:
        return QCoreApplication::translate("Bundle", "Closed");
    case vss::endpoint_state::failed:
        return QCoreApplication::translate("Bundle", "Failed");
    }
    return QCoreApplication::translate("Bundle", "Unknown");
}

QString bundle_hardware_summary(const QString& hardware_id)
{
    // Only what is recognised is copied out. Whatever else the id holds,
    // a serial number among it, is left behind.
    static const QRegularExpression vendor(
        QStringLiteral("VID[_:]?([0-9A-Fa-f]{4})"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression product(
        QStringLiteral("PID[_:]?([0-9A-Fa-f]{4})"),
        QRegularExpression::CaseInsensitiveOption);
    const auto vid = vendor.match(hardware_id);
    const auto pid = product.match(hardware_id);
    if (vid.hasMatch() && pid.hasMatch())
        return QStringLiteral("%1:%2").arg(vid.captured(1).toLower(),
                                           pid.captured(1).toLower());

    // "usb:0403:6001" and the like.
    static const QRegularExpression pair(
        QStringLiteral("^[A-Za-z]{2,8}:([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})"));
    const auto ids = pair.match(hardware_id);
    if (ids.hasMatch())
        return QStringLiteral("%1:%2").arg(ids.captured(1).toLower(),
                                           ids.captured(2).toLower());

    // The bus or enumerator name, such as ACPI or VSSCOM.
    static const QRegularExpression bus(
        QStringLiteral("^([A-Za-z][A-Za-z0-9]{1,15})[\\\\/:]"));
    const auto name = bus.match(hardware_id);
    if (name.hasMatch())
        return name.captured(1);
    return QStringLiteral("not known");
}

bundle_contents build_bundle(const StudioBackend& backend, const EventLog& log,
                             const bundle_selection& selection,
                             const bundle_identity& identity,
                             const QDateTime& created)
{
    VSS_PROFILE_ZONE("gui.support.bundle.build");
    // The computer's name stays out of every file unless the person ticked
    // the row that adds it, and then it is in licence.txt only: the other
    // files are still cleaned of it.
    bundle_contents contents;
    if (selection.environment)
        contents.sections.push_back(
            section("environment", "Version and environment", "environment.txt",
                    cleaned(environment_text(backend), identity)));
    if (selection.driver)
        contents.sections.push_back(
            section("driver", "Driver state", "driver.txt",
                    cleaned(driver_text(backend), identity)));
    if (selection.setup)
        contents.sections.push_back(
            section("setup", "Setup", "setup.json",
                    cleaned(setup_json(backend), identity)));
    if (selection.event_log)
    {
        const QString text =
            log.size() == 0
                ? QCoreApplication::translate("Bundle", "No events recorded.\n")
                : log.text();
        contents.sections.push_back(
            section("log", "Event log", "log.txt", cleaned(text, identity)));
    }
    if (selection.counters)
        contents.sections.push_back(
            section("counters", "Port counters", "counters.csv",
                    cleaned(counters_csv(backend), identity)));
    if (selection.serial_ports)
        contents.sections.push_back(section(
            "serial-ports", "Serial ports on this computer", "serial-ports.txt",
            cleaned(serial_ports_text(backend), identity)));
    if (selection.licence && bundle_has_licence(backend))
    {
        bundle_identity kept = identity;
        const bool names_computer =
            selection.computer_name && bundle_licence_names_computers(backend);
        if (names_computer)
            kept.computer_name.clear();
        // The licensee's name is whatever the licence says. It is written
        // only when its row is ticked, and never met by the cleaning above.
        contents.sections.push_back(
            section("licence", "Licence", "licence.txt",
                    cleaned(licence_file_text(backend, selection), kept),
                    selection.licensee_name || names_computer));
    }

    QJsonArray sections;
    for (const bundle_section& part : contents.sections)
    {
        QJsonArray files;
        for (const bundle_file& file : part.files)
            files.append(file.name);
        QJsonObject entry;
        entry.insert(QLatin1String("id"), part.id);
        entry.insert(QLatin1String("title"), part.title);
        entry.insert(QLatin1String("files"), files);
        entry.insert(QLatin1String("sensitive"), part.sensitive);
        entry.insert(QLatin1String("added_by"), QStringLiteral("studio"));
        sections.append(entry);
    }
    QJsonObject manifest;
    manifest.insert(QLatin1String("bundle_format"), kBundleFormat);
    manifest.insert(QLatin1String("created"),
                    created.toUTC().toString(Qt::ISODate));
    manifest.insert(QLatin1String("studio_version"),
                    current_build_info().version);
    manifest.insert(QLatin1String("sections"), sections);
    contents.manifest = {
        QStringLiteral("manifest.json"),
        QJsonDocument(manifest).toJson(QJsonDocument::Indented)};
    return contents;
}

} // namespace vss::gui::support
