//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/studio_backend.hpp"

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
