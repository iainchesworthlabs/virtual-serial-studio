//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/studio_backend.hpp"

#include <QLocale>

#include <algorithm>

namespace vss::gui
{

StudioBackend::StudioBackend(QObject* parent) : QObject(parent) {}

StudioBackend::~StudioBackend() = default;

vss::license::entitlement StudioBackend::entitlement(const QString& feature) const
{
    return vss::license::check(licence().status, feature.toStdString());
}

std::vector<device_view> StudioBackend::other_program_ports() const
{
    std::vector<device_view> out;
    if (caps().external_ports)
        return out;

    const auto held = ports();
    for (auto& device : devices())
    {
        // A port made by the Studio driver has a hardware id that starts with
        // VSSCOM\.  com0com pairs are not listed: they outlive any program
        // and the Attach flow opens them.
        if (!device.virtual_device ||
            !device.hardware_id.startsWith(QStringLiteral("VSSCOM\\"),
                                           Qt::CaseInsensitive))
            continue;
        const bool ours = std::any_of(
            held.begin(), held.end(),
            [&device](const port_view& port)
            {
                return port.path.compare(device.path, Qt::CaseInsensitive) == 0;
            });
        if (!ours)
            out.push_back(std::move(device));
    }
    return out;
}

namespace
{

/// A length as the person reads it: microseconds, milliseconds or seconds,
/// three figures.
QString length_text(std::chrono::nanoseconds length)
{
    const double microseconds = static_cast<double>(length.count()) / 1000.0;
    if (microseconds < 1000.0)
        return QObject::tr("%1 µs")
            .arg(QLocale().toString(microseconds, 'g', 3));
    const double milliseconds = microseconds / 1000.0;
    if (milliseconds < 1000.0)
        return QObject::tr("%1 ms")
            .arg(QLocale().toString(milliseconds, 'g', 3));
    return QObject::tr("%1 s")
        .arg(QLocale().toString(milliseconds / 1000.0, 'g', 3));
}

} // namespace

QString break_detail(vss::break_phase phase, std::chrono::nanoseconds duration,
                     bool measured)
{
    if (phase == vss::break_phase::begin)
        return QObject::tr("running");
    if (!measured)
        return QObject::tr("at least %1").arg(length_text(duration));
    return length_text(duration);
}

QString error_text(const vss::error_code& error)
{
    if (!error)
        return {};
    QString message = QString::fromStdString(error.message());
    if (!message.isEmpty())
        return message;
    return QObject::tr("Error %1").arg(error.value());
}

} // namespace vss::gui
