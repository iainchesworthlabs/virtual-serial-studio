//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QString>
#include <QVector>

namespace vss::gui::support
{

/// One labelled line in the Build information and Environment tabs and in the
/// reports.  The label is the English text; the tabs translate it.
struct environment_row
{
    QString label;
    QString value;
};

/// Which build this is, and what it runs on.  The build part comes from the
/// file the build system generates before every build (build_stamp.hpp), the
/// environment part from Qt and the operating system at run time.  Anything
/// not known is empty or "unknown", never a guess.
struct build_info
{
    // Build information.
    QString version; ///< Semantic version, for example "0.1.0-rc.3".
    QString tag;     ///< The git tag, empty when the build saw none.
    int commits_since_tag = 0;
    QString commit;      ///< Short revision.
    QString commit_full; ///< Full revision.
    QString branch;
    bool dirty = false;
    QString build_number; ///< The CI run number, empty outside CI.
    QString build_time;   ///< UTC, ISO 8601.
    QString build_type;
    QString compiler;
    QString qt_build;
    QString boost;

    // Environment.
    QString qt_runtime;
    QString operating_system;
    QString architecture;
    QString kernel;
    QString platform_plugin;
    QString display;
    QString profiling;
    /// What draws the canvas now: hardware (GPU) or software (CPU).
    QString renderer;
    /// The language chosen at start-up and the languages the system offered.
    QString language;

    /// The single version string, for example
    /// "v0.1.0-rc.3+build.6083-3de4617".
    QString full_version() const;
};

/// "v<version>+build.<number>-<commit>", the way the other Iain Chesworth
/// Labs applications write it.  The build number part is left out when there
/// is none, the commit part when it is unknown, and "-dirty" follows the
/// commit when the tree differed from it.
QString compose_version(const QString& version, const QString& build_number,
                        const QString& commit, bool dirty);

/// The full version string of this binary, without gathering anything else.
QString current_version_string();

/// The build this binary was produced from, on the machine it runs on.
build_info current_build_info();

/// The rows of the Build information tab.
QVector<environment_row> build_information_rows(const build_info& info);

/// The rows of the Environment tab.
QVector<environment_row> environment_rows(const build_info& info);

/// "Copy build information": plain text for a bug report.
QString build_information_report(const build_info& info);

/// "Copy environment": platform facts only, never the process environment, so
/// it cannot leak a secret.
QString environment_report(const build_info& info);

/// "Copy diagnostic information", and `environment.txt` in the diagnostic
/// bundle: both reports together.
QString diagnostic_report(const build_info& info);

/// Convenience overload using current_build_info().
QString diagnostic_report();

} // namespace vss::gui::support
