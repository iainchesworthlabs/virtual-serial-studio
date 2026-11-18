//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/driver_versions.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <filesystem>

namespace vss::gui
{
namespace
{

QString text_of(const vss::driver_package_version& version)
{
    return QString::fromStdString(vss::describe_driver_package(version));
}

} // namespace

driver_versions assess_driver_versions(
    const std::optional<vss::driver_package_version>& installed,
    const std::optional<vss::driver_package_version>& bundled,
    const QString& ports)
{
    driver_versions out;
    out.ports = ports;
    if (installed)
        out.installed = text_of(*installed);
    if (bundled)
        out.bundled = text_of(*bundled);
    if (installed && bundled)
    {
        switch (vss::compare_driver_packages(*installed, *bundled))
        {
        case vss::driver_package_order::older:
            out.age = driver_age::older;
            break;
        case vss::driver_package_order::same:
            out.age = driver_age::same;
            break;
        case vss::driver_package_order::newer:
            out.age = driver_age::newer;
            break;
        }
    }
    return out;
}

QString bundled_driver_inf_path()
{
    const auto inf_in = [](const QDir& directory)
    {
        const QFileInfo inf(
            directory.filePath(QStringLiteral("driver/vsscom_um.inf")));
        return inf.isFile() ? QDir::toNativeSeparators(inf.absoluteFilePath())
                            : QString();
    };
    const QString named = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("VSS_DRIVER_PACKAGE"));
    if (!named.isEmpty())
    {
        if (const QString path = inf_in(QDir(named)); !path.isEmpty())
            return path;
    }
    // bin\driver\ holds setup-driver.ps1 and the package in its own driver
    // folder, so Studio's `driver` folder has `driver\vsscom_um.inf` in it.
    return inf_in(QDir(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("driver"))));
}

std::optional<vss::driver_package_version> bundled_driver_version()
{
    const QString path = bundled_driver_inf_path();
    if (path.isEmpty())
        return std::nullopt;
    return vss::read_inf_driver_version(
        std::filesystem::path(path.toStdU16String()));
}

bool driver_is_behind(const driver_versions& versions) noexcept
{
    return versions.age == driver_age::older;
}

QString driver_versions_sentence(const driver_versions& versions)
{
    if (versions.installed.isEmpty())
    {
        if (versions.bundled.isEmpty() && versions.ports.isEmpty())
            return {};
        return QCoreApplication::translate(
            "DriverVersions",
            "Studio could not read which version of the driver Windows "
            "holds.");
    }
    if (versions.bundled.isEmpty())
    {
        return QCoreApplication::translate(
                   "DriverVersions",
                   "The driver in Windows is version %1. This copy of Studio "
                   "has no driver files to compare it with.")
            .arg(versions.installed);
    }
    switch (versions.age)
    {
    case driver_age::older:
        return QCoreApplication::translate(
                   "DriverVersions",
                   "The driver in Windows is version %1. This copy of Studio "
                   "carries the newer version %2.")
            .arg(versions.installed, versions.bundled);
    case driver_age::newer:
        return QCoreApplication::translate(
                   "DriverVersions",
                   "The driver in Windows is version %1, newer than the "
                   "version this copy of Studio carries (%2). Nothing needs "
                   "updating.")
            .arg(versions.installed, versions.bundled);
    case driver_age::same:
    case driver_age::unknown:
        break;
    }
    return QCoreApplication::translate(
               "DriverVersions",
               "The driver in Windows is version %1, the same as this copy "
               "of Studio carries.")
        .arg(versions.installed);
}

} // namespace vss::gui
