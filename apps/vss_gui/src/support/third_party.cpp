//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/third_party.hpp"

#include <vss/core/profiling.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

// Q_INIT_RESOURCE declares the generated symbol in the scope it is used in, so
// this wrapper sits at global scope with external linkage.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void vss_init_third_party_resources()
{
    Q_INIT_RESOURCE(third_party);
}

namespace vss::gui::support
{
namespace
{

QJsonObject inventory()
{
    vss_init_third_party_resources();
    QFile file(QStringLiteral(":/support/third_party.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

QVector<third_party_component> third_party_components()
{
    QVector<third_party_component> result;
    const QJsonArray array = inventory().value(QLatin1String("components")).toArray();
    for (const QJsonValue& value : array)
    {
        const QJsonObject object = value.toObject();
        third_party_component component;
        component.notice = object.value(QLatin1String("notice")).toString();
        component.name = object.value(QLatin1String("name")).toString();
        component.spdx = object.value(QLatin1String("spdx")).toString();
        component.vcpkg = object.value(QLatin1String("vcpkg")).toString();
        component.shipped = object.value(QLatin1String("shipped")).toString();
        component.condition = object.value(QLatin1String("condition")).toString();
        result.push_back(component);
    }
    return result;
}

QVector<third_party_font> third_party_fonts()
{
    QVector<third_party_font> result;
    const QJsonArray array = inventory().value(QLatin1String("fonts")).toArray();
    for (const QJsonValue& value : array)
    {
        const QJsonObject object = value.toObject();
        third_party_font font;
        font.family = object.value(QLatin1String("family")).toString();
        font.version = object.value(QLatin1String("version")).toString();
        font.spdx = object.value(QLatin1String("spdx")).toString();
        font.licence_text = object.value(QLatin1String("licence_text")).toString();
        font.url = object.value(QLatin1String("url")).toString();
        result.push_back(font);
    }
    return result;
}

QStringList third_party_not_shipped()
{
    QStringList result;
    const QJsonArray array =
        inventory().value(QLatin1String("not_shipped")).toArray();
    for (const QJsonValue& value : array)
        result << value.toObject().value(QLatin1String("notice")).toString();
    return result;
}

bool profiling_compiled()
{
    return vss::profiling::current().current != vss::profiling::state::not_compiled;
}

QVector<third_party_component> bundled_components(bool profiling_built)
{
    QVector<third_party_component> result;
    for (const third_party_component& component : third_party_components())
    {
        if (component.condition.isEmpty() ||
            (component.condition == QLatin1String("profiling") && profiling_built))
            result.push_back(component);
    }
    return result;
}

QString licence_text(const QString& spdx)
{
    if (spdx == QLatin1String("LGPL-3.0-only OR GPL-3.0-only"))
        return QCoreApplication::translate(
            "About",
            "GNU Lesser General Public License, version 3, or the GNU "
            "General Public License, version 3.");
    if (spdx == QLatin1String("BSL-1.0"))
        return QCoreApplication::translate("About",
                                           "Boost Software License, version 1.0.");
    if (spdx == QLatin1String("OFL-1.1"))
        return QCoreApplication::translate("About",
                                           "SIL Open Font License, version 1.1.");
    if (spdx == QLatin1String("BSD-3-Clause"))
        return QCoreApplication::translate("About", "BSD 3-Clause License.");
    return spdx;
}

QString licence_note(const QString& notice)
{
    if (notice == QLatin1String("Qt"))
        return QCoreApplication::translate(
            "About", "Shipped with Studio as shared libraries.");
    if (notice == QLatin1String("Tracy"))
        return QCoreApplication::translate(
            "About", "Present because this build includes profiling support.");
    return {};
}

} // namespace vss::gui::support
