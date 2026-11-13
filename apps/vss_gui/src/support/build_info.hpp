//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_BUILD_INFO_HPP
#define VSS_GUI_SUPPORT_BUILD_INFO_HPP

#include <QString>
#include <QVector>

namespace vss::gui::support
{

/// One labelled line in the Environment tab and in `environment.txt`.
struct environment_row
{
    QString label;
    QString value;
};

/// The committed build identity and the platform versions the About window
/// and the diagnostic bundle report.  Values come from compile definitions at
/// configure time and from Qt at run time; anything the build cannot know is
/// "unknown" rather than a guess.
struct build_info
{
    QString version;
    QString commit;
    QString branch;
    QString build_date;
    QString build_type;
    QString compiler;
    QString qt_runtime;
    QString qt_build;
    QString boost;
    QString operating_system;
    QString architecture;
    QString kernel;
    /// The language chosen at start-up and the languages the system offered.
    QString profiling;
    QString language;
    /// What draws the canvas now: hardware (GPU) or software (CPU).
    QString renderer;
};

/// The build this binary was produced from.
build_info current_build_info();

/// The rows the design lists for the Environment tab, plus Boost (the report
/// names every linked first-party and third-party component).
QVector<environment_row> environment_rows(const build_info& info);

/// The plain-text environment block behind "Copy environment" and
/// `environment.txt`.  It contains build and platform facts only, never the
/// process environment, so it cannot leak a secret.
QString environment_report(const build_info& info);

/// Convenience overload using current_build_info().
QString environment_report();

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_BUILD_INFO_HPP
