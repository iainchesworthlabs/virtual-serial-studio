//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/build_info.hpp"

#include "language/language_choice.hpp"
#include "support/build_stamp.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QSysInfo>
#include <QtGlobal>

#include <boost/version.hpp>
#include <vss/core/profiling.hpp>

namespace vss::gui::support
{
namespace
{

QString text(std::string_view value)
{
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

QString boost_version()
{
    return QStringLiteral("%1.%2.%3")
        .arg(BOOST_VERSION / 100000)
        .arg((BOOST_VERSION / 100) % 1000)
        .arg(BOOST_VERSION % 100);
}

QString display_description()
{
    const QScreen* primary = QGuiApplication::primaryScreen();
    if (primary == nullptr)
        return QStringLiteral("unknown");
    const QSize size = primary->size();
    QString result = QStringLiteral("%1 x %2 points, scale %3%, %4 dpi")
                         .arg(size.width())
                         .arg(size.height())
                         .arg(qRound(primary->devicePixelRatio() * 100))
                         .arg(qRound(primary->logicalDotsPerInch()));
    const qsizetype screens = QGuiApplication::screens().size();
    if (screens > 1)
        result += QStringLiteral(", %1 screens").arg(screens);
    return result;
}

QString or_none(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("none") : value;
}

QString label(const char* english)
{
    return QString::fromLatin1(english);
}

QString report_text(const QString& heading,
                    const QVector<environment_row>& rows)
{
    QString report = heading + QLatin1Char('\n');
    for (const environment_row& row : rows)
        report += QStringLiteral("%1: %2\n").arg(row.label, row.value);
    return report;
}

} // namespace

QString compose_version(const QString& version, const QString& build_number,
                        const QString& commit, bool dirty)
{
    QString result = QStringLiteral("v") + version;
    const bool has_commit =
        !commit.isEmpty() && commit != QLatin1String("unknown");
    if (build_number.isEmpty() && !has_commit)
        return result;
    result += QLatin1Char('+');
    if (!build_number.isEmpty())
        result += QStringLiteral("build.") + build_number;
    if (has_commit)
    {
        if (!build_number.isEmpty())
            result += QLatin1Char('-');
        result += commit;
        if (dirty)
            result += QStringLiteral("-dirty");
    }
    return result;
}

QString build_info::full_version() const
{
    return compose_version(version, build_number, commit, dirty);
}

QString current_version_string()
{
    const build_stamp& stamp = generated_build_stamp();
    return compose_version(text(stamp.version), text(stamp.build_number),
                           text(stamp.commit), stamp.dirty);
}

build_info current_build_info()
{
    const build_stamp& stamp = generated_build_stamp();
    build_info info;
    info.version = text(stamp.version);
    info.tag = text(stamp.tag);
    info.commits_since_tag = stamp.commits_since_tag;
    info.commit = text(stamp.commit);
    info.commit_full = text(stamp.commit_full);
    info.branch = text(stamp.branch);
    info.dirty = stamp.dirty;
    info.build_number = text(stamp.build_number);
    info.build_time = text(stamp.build_time);
    info.build_type = text(stamp.build_type);
    if (info.build_type.isEmpty())
        info.build_type = QStringLiteral("unknown");
    info.compiler = text(stamp.compiler);
    info.qt_build = QStringLiteral(QT_VERSION_STR);
    info.boost = boost_version();

    info.qt_runtime = QString::fromLatin1(qVersion());
    info.operating_system = QSysInfo::prettyProductName();
    info.architecture = QSysInfo::currentCpuArchitecture();
    info.kernel = QStringLiteral("%1 %2").arg(QSysInfo::kernelType(),
                                              QSysInfo::kernelVersion());
    info.platform_plugin = QGuiApplication::platformName();
    info.display = display_description();
    info.profiling = QString::fromStdString(
        vss::profiling::describe(vss::profiling::current()));
    info.language = vss::gui::language_choice_description();
    return info;
}

QVector<environment_row> build_information_rows(const build_info& info)
{
    QString tag = or_none(info.tag);
    if (!info.tag.isEmpty() && info.commits_since_tag > 0)
        tag = QStringLiteral("%1, %2 commits later")
                  .arg(info.tag)
                  .arg(info.commits_since_tag);
    return {
        {label(QT_TRANSLATE_NOOP("About", "Version")), info.full_version()},
        {label(QT_TRANSLATE_NOOP("About", "Git tag")), tag},
        {label(QT_TRANSLATE_NOOP("About", "Git revision")), info.commit_full},
        {label(QT_TRANSLATE_NOOP("About", "Short revision")), info.commit},
        {label(QT_TRANSLATE_NOOP("About", "Working tree")),
         info.dirty ? QStringLiteral("modified") : QStringLiteral("clean")},
        {label(QT_TRANSLATE_NOOP("About", "Branch")), info.branch},
        {label(QT_TRANSLATE_NOOP("About", "Build number")),
         info.build_number.isEmpty() ? QStringLiteral("none (not a CI build)")
                                     : info.build_number},
        {label(QT_TRANSLATE_NOOP("About", "Built")), or_none(info.build_time)},
        {label(QT_TRANSLATE_NOOP("About", "Build type")), info.build_type},
        {label(QT_TRANSLATE_NOOP("About", "Compiler")), info.compiler},
        {label(QT_TRANSLATE_NOOP("About", "Qt built with")), info.qt_build},
        {label(QT_TRANSLATE_NOOP("About", "Boost")), info.boost},
    };
}

QVector<environment_row> environment_rows(const build_info& info)
{
    return {
        {label(QT_TRANSLATE_NOOP("About", "Operating system")),
         info.operating_system},
        {label(QT_TRANSLATE_NOOP("About", "Architecture")), info.architecture},
        {label(QT_TRANSLATE_NOOP("About", "Kernel")), info.kernel},
        {label(QT_TRANSLATE_NOOP("About", "Qt running")), info.qt_runtime},
        {label(QT_TRANSLATE_NOOP("About", "Platform plugin")),
         info.platform_plugin},
        {label(QT_TRANSLATE_NOOP("About", "Display")), info.display},
        {label(QT_TRANSLATE_NOOP("About", "Language")), info.language},
        {label(QT_TRANSLATE_NOOP("About", "Profiling")), info.profiling},
    };
}

QString build_information_report(const build_info& info)
{
    return report_text(
        QStringLiteral("Virtual Serial Studio build information"),
        build_information_rows(info));
}

QString environment_report(const build_info& info)
{
    return report_text(QStringLiteral("Virtual Serial Studio environment"),
                       environment_rows(info));
}

QString diagnostic_report(const build_info& info)
{
    return build_information_report(info) + QLatin1Char('\n') +
           environment_report(info);
}

QString diagnostic_report()
{
    return diagnostic_report(current_build_info());
}

} // namespace vss::gui::support
