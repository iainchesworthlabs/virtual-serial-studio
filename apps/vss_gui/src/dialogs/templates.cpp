//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/templates.hpp"

#include <vss/core/profile.hpp>

#include <QFile>
#include <QStringList>

// Q_INIT_RESOURCE declares the generated symbol in the scope it is used in, so
// this wrapper has to sit at global scope with external linkage; an unnamed
// namespace or a static function would look for another symbol.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void vss_init_template_resources()
{
    Q_INIT_RESOURCE(templates);
}

namespace vss::gui
{

QVector<bundled_template> bundled_templates()
{
    VSS_PROFILE_ZONE("gui.dialogs.templates.list");
    return {
        {QStringLiteral("test-bench"), QObject::tr("Test bench, no hardware"),
         QObject::tr("Two Studio-only ports and a one-way link, so a fresh "
                     "PC can show the whole data path."),
         QObject::tr("Basic"), 2, 1, false,
         QStringLiteral(":/templates/test-bench.json")},
        {QStringLiteral("share-three-programs"),
         QObject::tr("Share a device with three programs"),
         QObject::tr("One real device split to three ports, each ready for "
                     "its own program."),
         QObject::tr("Intermediate"), 4, 1, true,
         QStringLiteral(":/templates/share-three-programs.json")},
        {QStringLiteral("device-on-the-network"),
         QObject::tr("Device on the network"),
         QObject::tr("A serial device bridged to a TCP port, both "
                     "directions visible in the monitor."),
         QObject::tr("Intermediate"), 2, 1, true,
         QStringLiteral(":/templates/device-on-the-network.json")},
        {QStringLiteral("two-programs-owner"),
         QObject::tr("Two programs, one device, one at a time"),
         QObject::tr("Two programs joined into one device, with an owner so "
                     "writes never collide."),
         QObject::tr("Advanced"), 3, 1, false,
         QStringLiteral(":/templates/two-programs-owner.json")},
    };
}

vss::result<vss::config::document> load_bundled_template(const QString& id)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.dialogs.templates.load");
    VSS_PROFILE_ZONE_TEXT(zone, id.toStdString());
    vss_init_template_resources();

    for (const bundled_template& entry : bundled_templates())
    {
        if (entry.id != id)
            continue;
        QFile file(entry.resource);
        if (!file.open(QIODevice::ReadOnly))
            return vss::make_error_code(vss::errc::not_found);
        const QByteArray contents = file.readAll();
        const auto document = vss::config::from_json(std::string_view(
            contents.constData(), static_cast<std::size_t>(contents.size())));
        if (!document)
            return document.error();
        return *document;
    }
    return vss::make_error_code(vss::errc::not_found);
}

} // namespace vss::gui
