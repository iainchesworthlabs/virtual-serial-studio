//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The support bundle: one zip a person saves and sends to a helper
// (docs/gui-design/docs/screens/09-licence-and-support.md, "Support
// bundle"). This file builds its contents. The dialog previews exactly
// what is built here and the zip holds the same bytes, so the two cannot
// differ. The bundle never holds the bytes a port sent or received.
//

#ifndef VSS_GUI_SUPPORT_BUNDLE_HPP
#define VSS_GUI_SUPPORT_BUNDLE_HPP

#include "backend/studio_backend.hpp"
#include "backend/view_types.hpp"
#include "support/event_log.hpp"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QVector>

namespace vss::gui::support
{

/// The format of the bundle. It rises only for a change an old reader
/// cannot ignore.
inline constexpr int bundle_format = 1;

/// The section id and folder of the Pro plugin "Diagnostics and replay".
/// Studio never writes it.
inline constexpr const char* reserved_traffic_section = "traffic";

/// What the person ticked. A section is the unit of consent: one row in the
/// checklist, one group in the preview, one entry in the manifest.
struct bundle_selection
{
    bool environment = true;
    bool driver = true;
    bool setup = true;
    bool event_log = true;
    bool counters = true;
    bool serial_ports = true;
    /// Licence number, edition, term and state. Ignored when no licence is
    /// installed: the row is then absent. Never the licence file.
    bool licence = true;
    /// Off by default: the name the licence was issued to, in `licence.txt`.
    bool licensee_name = false;
    /// Off by default: this computer's name, in `licence.txt`. Offered only
    /// when the installed licence names computers.
    bool computer_name = false;
};

struct bundle_file
{
    QString name;
    QByteArray data;
};

/// One section of the bundle.
struct bundle_section
{
    /// `environment`, `driver`, `setup`, `log`, `counters`, `serial-ports`
    /// or `licence`.
    QString id;
    /// The English title the manifest carries. The checklist shows its own
    /// translated label.
    QString title;
    QVector<bundle_file> files;
    bool sensitive = false;
};

/// Everything a bundle holds, in the order it is written.
struct bundle_contents
{
    /// `manifest.json`. It lists exactly the sections below.
    bundle_file manifest;
    QVector<bundle_section> sections;

    /// The manifest first, then every file of every section.
    QVector<bundle_file> files() const;
};

/// What identifies the person and the computer, and is taken out of every
/// file. The dialog passes the real values; a test passes markers.
struct bundle_identity
{
    QString computer_name;
    QString user_name;
};

/// The identity of this computer and its user.
bundle_identity current_bundle_identity();

/// `vss-support-<date>.zip`.
QString bundle_default_name(const QDate& date = QDate::currentDate());

/// True when a licence is installed, so the "Licence" row is shown.
bool bundle_has_licence(const StudioBackend& backend);

/// True when the installed licence names computers, so the "This computer's
/// name" row is shown.
bool bundle_licence_names_computers(const StudioBackend& backend);

/// Builds the bundle for a selection. Pure apart from reading the backend,
/// the log and the build information.
bundle_contents
build_bundle(const StudioBackend& backend, const EventLog& log,
             const bundle_selection& selection,
             const bundle_identity& identity = current_bundle_identity(),
             const QDateTime& created = QDateTime::currentDateTimeUtc());

QString port_state_label(vss::endpoint_state state);

/// The vendor and product of a hardware id, such as "0403:6001", or the
/// name of its bus when it has none. Never the rest of the id: that part
/// can be the serial number of a device.
QString bundle_hardware_summary(const QString& hardware_id);

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_BUNDLE_HPP
