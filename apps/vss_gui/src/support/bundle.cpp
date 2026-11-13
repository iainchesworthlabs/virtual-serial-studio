//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/bundle.hpp"

#include "support/build_info.hpp"
#include "support/redact.hpp"

#include <vss/config/document.hpp>
#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cstddef>

namespace vss::gui::support
{
namespace
{

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

QByteArray counters_csv(const StudioBackend& backend)
{
    QString out =
        QStringLiteral("name,path,state,rx_bytes,tx_bytes,dropped,errors\n");
    for (const port_view& port : backend.ports())
    {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
                   .arg(csv_field(redact_secrets(port.name)),
                        csv_field(redact_secrets(port.path)),
                        port_state_label(port.state))
                   .arg(port.stats.bytes_rx)
                   .arg(port.stats.bytes_tx)
                   .arg(port.stats.dropped)
                   .arg(port.stats.errors);
    }
    return out.toUtf8();
}

QByteArray setup_json(const StudioBackend& backend)
{
    const auto document = vss::config::to_json(backend.snapshot());
    if (!document)
    {
        return redact_secrets(
                   QCoreApplication::translate(
                       "Bundle", "The setup could not be written: %1")
                       .arg(QString::fromStdString(document.error().message())))
            .toUtf8();
    }
    return redact_secrets(QString::fromStdString(*document)).toUtf8();
}

QString ascii_preview(const QByteArray& data)
{
    QString out;
    out.reserve(data.size());
    for (const char byte : data)
    {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20 && value < 0x7F)
            out += QLatin1Char(static_cast<char>(value));
        else
            out += QLatin1Char('.');
    }
    return out;
}

} // namespace

int bundle_selection::file_count() const
{
    return static_cast<int>(setup) + static_cast<int>(event_log) +
           static_cast<int>(environment) + static_cast<int>(counters) +
           static_cast<int>(traffic);
}

QString bundle_default_name(const QDate& date)
{
    return QStringLiteral("vss-bundle-%1.zip")
        .arg(date.toString(QStringLiteral("yyyy-MM-dd")));
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

QString render_traffic(const std::vector<traffic_view>& traffic)
{
    VSS_PROFILE_ZONE("gui.support.bundle.render_traffic");
    QString out = QCoreApplication::translate(
        "Bundle",
        "# Traffic contents from Virtual Serial Studio. These bytes may "
        "include passwords or personal data.\n");
    int index = 0;
    for (const traffic_view& record : traffic)
    {
        ++index;
        out += QStringLiteral("#%1 %2 %3 %4 bytes\n")
                   .arg(index)
                   .arg(record.name.isEmpty()
                            ? QStringLiteral("port %1").arg(record.endpoint)
                            : record.name,
                        record.direction == vss::data_direction::rx
                            ? QStringLiteral("RX")
                            : QStringLiteral("TX"))
                   .arg(record.data.size());
        for (int offset = 0; offset < record.data.size(); offset += 16)
        {
            const QByteArray chunk = record.data.mid(offset, 16);
            QString hex;
            for (const char byte : chunk)
            {
                if (!hex.isEmpty())
                    hex += QLatin1Char(' ');
                hex += QStringLiteral("%1").arg(
                    static_cast<unsigned char>(byte), 2, 16, QLatin1Char('0'));
            }
            out += QStringLiteral("%1  %2  |%3|\n")
                       .arg(offset, 4, 16, QLatin1Char('0'))
                       .arg(hex, -47)
                       .arg(ascii_preview(chunk));
        }
    }
    if (index == 0)
        out += QCoreApplication::translate("Bundle", "No traffic recorded.\n");
    return redact_secrets(out);
}

QVector<bundle_file> build_bundle_files(const StudioBackend& backend,
                                        const EventLog& log,
                                        const bundle_selection& selection)
{
    VSS_PROFILE_ZONE("gui.support.bundle.build");
    QVector<bundle_file> files;
    if (selection.setup)
        files.push_back({QStringLiteral("setup.json"), setup_json(backend)});
    if (selection.event_log)
    {
        const QString text =
            log.size() == 0
                ? QCoreApplication::translate("Bundle", "No events recorded.\n")
                : log.text();
        files.push_back(
            {QStringLiteral("log.txt"), redact_secrets(text).toUtf8()});
    }
    if (selection.environment)
    {
        files.push_back({QStringLiteral("environment.txt"),
                         redact_secrets(diagnostic_report()).toUtf8()});
    }
    if (selection.counters)
        files.push_back(
            {QStringLiteral("counters.csv"), counters_csv(backend)});
    if (selection.traffic)
    {
        files.push_back({QStringLiteral("traffic.txt"),
                         render_traffic(backend.traffic(1000)).toUtf8()});
    }
    return files;
}

} // namespace vss::gui::support
