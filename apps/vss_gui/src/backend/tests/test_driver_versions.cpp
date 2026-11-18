//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three versions Studio knows of the driver: how the installed package
// compares with the bundled one (older, equal, newer, not known), where the
// bundled package is looked for, and what is said about each case.
//

#include "backend/driver_versions.hpp"

#include "test_framework.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>

using namespace vss::gui;

namespace
{

vss::driver_package_version ver(const char* text)
{
    const auto parsed = vss::parse_driver_ver(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE("driver versions: installed against bundled is older, same, newer or "
          "not known")
{
    const auto bundled = ver("11/18/2026,1.5.1.0");
    REQUIRE(
        assess_driver_versions(ver("11/12/2026,1.1.0.0"), bundled, {}).age ==
        driver_age::older);
    // The same version a day earlier is still older: Windows reads the date
    // first.
    REQUIRE(
        assess_driver_versions(ver("11/17/2026,1.5.1.0"), bundled, {}).age ==
        driver_age::older);
    REQUIRE(
        assess_driver_versions(ver("11/18/2026,1.5.1.0"), bundled, {}).age ==
        driver_age::same);
    REQUIRE(
        assess_driver_versions(ver("11/18/2026,1.5.2.0"), bundled, {}).age ==
        driver_age::newer);
    REQUIRE(assess_driver_versions(std::nullopt, bundled, {}).age ==
            driver_age::unknown);
    REQUIRE(assess_driver_versions(bundled, std::nullopt, {}).age ==
            driver_age::unknown);

    const auto behind = assess_driver_versions(ver("11/12/2026,1.1.0.0"),
                                               bundled, QStringLiteral("4"));
    REQUIRE_EQ(behind.installed, QStringLiteral("1.1.0.0 (2026-11-12)"));
    REQUIRE_EQ(behind.bundled, QStringLiteral("1.5.1.0 (2026-11-18)"));
    REQUIRE_EQ(behind.ports, QStringLiteral("4"));
    REQUIRE(driver_is_behind(behind));
    REQUIRE(!driver_is_behind(assess_driver_versions(bundled, bundled, {})));
    REQUIRE(
        !driver_is_behind(assess_driver_versions(std::nullopt, bundled, {})));
}

TEST_CASE("driver versions: each case is said in its own words, and none is an "
          "alarm unless behind")
{
    const auto bundled = ver("11/18/2026,1.5.1.0");
    const QString older = driver_versions_sentence(
        assess_driver_versions(ver("11/12/2026,1.1.0.0"), bundled, {}));
    const QString same =
        driver_versions_sentence(assess_driver_versions(bundled, bundled, {}));
    const QString newer = driver_versions_sentence(
        assess_driver_versions(ver("12/1/2026,1.6.0.0"), bundled, {}));
    const QString no_files = driver_versions_sentence(
        assess_driver_versions(bundled, std::nullopt, {}));
    const QString unknown = driver_versions_sentence(
        assess_driver_versions(std::nullopt, bundled, {}));
    REQUIRE(older.contains(QStringLiteral("version 1.1.0.0 (2026-11-12)")));
    REQUIRE(older.contains(QStringLiteral("newer version 1.5.1.0")));
    REQUIRE(same.contains(QStringLiteral("the same as this copy")));
    REQUIRE(newer.contains(QStringLiteral("Nothing needs updating")));
    REQUIRE(no_files.contains(QStringLiteral("no driver files")));
    REQUIRE(unknown.contains(QStringLiteral("could not read")));
    // Five cases, five sentences.
    QStringList all{older, same, newer, no_files, unknown};
    REQUIRE_EQ(all.removeDuplicates(), 0);
    // A system with nothing to say says nothing.
    REQUIRE(driver_versions_sentence({}).isEmpty());
}

TEST_CASE("driver versions: the bundled package is the driver folder beside "
          "the program or the one named")
{
    QTemporaryDir package;
    REQUIRE(package.isValid());
    REQUIRE(QDir(package.path()).mkpath(QStringLiteral("driver")));
    QFile inf(package.filePath(QStringLiteral("driver/vsscom_um.inf")));
    REQUIRE(inf.open(QIODevice::WriteOnly));
    inf.write("[Version]\r\nDriverVer=11/18/2026,1.5.1.0\r\n");
    inf.close();

    qputenv("VSS_DRIVER_PACKAGE", package.path().toLocal8Bit());
    REQUIRE(!bundled_driver_inf_path().isEmpty());
    const auto found = bundled_driver_version();
    REQUIRE(found.has_value());
    REQUIRE(*found == ver("11/18/2026,1.5.1.0"));

    // A folder with no package in it is no package.
    QTemporaryDir empty;
    REQUIRE(empty.isValid());
    qputenv("VSS_DRIVER_PACKAGE", empty.path().toLocal8Bit());
    REQUIRE(bundled_driver_inf_path().isEmpty());
    REQUIRE(!bundled_driver_version().has_value());
    qunsetenv("VSS_DRIVER_PACKAGE");
}
