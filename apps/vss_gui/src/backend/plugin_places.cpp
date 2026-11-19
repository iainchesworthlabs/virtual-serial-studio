//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/plugin_places.hpp"

#include <vss/core/serial_settings.hpp>

#include <QStringList>

#include <unordered_map>

namespace vss::gui
{
namespace
{

namespace host = vss::plugin::host;

QString names_of(const QVector<vss::endpoint_id>& ends,
                 const std::unordered_map<vss::endpoint_id, QString>& names)
{
    QStringList parts;
    for (const vss::endpoint_id id : ends)
    {
        const auto found = names.find(id);
        if (found != names.end())
            parts << found->second;
    }
    return parts.join(QLatin1Char('+'));
}

} // namespace

std::shared_ptr<const plugin_place_table>
make_plugin_place_table(const StudioBackend& backend)
{
    auto table = std::make_shared<plugin_place_table>();
    std::unordered_map<vss::endpoint_id, QString> names;
    for (const port_view& port : backend.ports())
    {
        names.emplace(port.id, port.name);
        host::capture_place place;
        place.tap.name = port.name.toStdString();
        place.tap.endpoints = {port.id};
        place.kind = host::place_kind::port;
        place.settings = vss::to_label(port.serial);
        place.lines = port.lines;
        table->emplace(place.tap.name, std::move(place));
    }
    for (const link_view& link : backend.links())
    {
        const QString left = names_of(link.sources, names);
        const QString right = names_of(link.targets, names);
        if (left.isEmpty() || right.isEmpty())
            continue;
        host::capture_place place;
        place.tap.name = (left + QStringLiteral(" - ") + right).toStdString();
        for (const vss::endpoint_id id : link.sources)
            place.tap.endpoints.push_back(id);
        for (const vss::endpoint_id id : link.targets)
            place.tap.endpoints.push_back(id);
        place.tap.link = link.id;
        place.kind = host::place_kind::link;
        table->emplace(place.tap.name, std::move(place));
    }
    return table;
}

std::shared_ptr<const plugin_endpoint_table>
make_plugin_endpoint_table(const StudioBackend& backend, const vss::hub& source)
{
    auto table = std::make_shared<plugin_endpoint_table>();
    for (const port_view& port : backend.ports())
    {
        if (auto endpoint = source.find_endpoint(port.id))
            table->emplace(port.name.toStdString(), std::move(endpoint));
    }
    return table;
}

} // namespace vss::gui
