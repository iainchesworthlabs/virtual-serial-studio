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

/// One component that is shipped in, or compiled into, a build.
struct third_party_component
{
    /// The name used in THIRD_PARTY_NOTICES.md.
    QString notice;
    /// The name the Licences tab shows.
    QString name;
    /// The licence as an SPDX expression.
    QString spdx;
    /// The vcpkg package, when the component comes from one.
    QString vcpkg;
    /// Where in the build it ends up.
    QString shipped;
    /// "profiling" when the component is in only if Tracy is compiled in.
    QString condition;
};

/// One font family shipped with the program.
struct third_party_font
{
    QString family;
    QString version;
    /// The licence as an SPDX identifier.
    QString spdx;
    /// The licence text, as a path from the repository root.
    QString licence_text;
    /// Where the font comes from.
    QString url;
};

/// The fonts third_party.json lists; empty while none is bundled.
QVector<third_party_font> third_party_fonts();

/// Everything third_party.json lists as shipped, whatever the build.
QVector<third_party_component> third_party_components();

/// What third_party.json says is installed or used but is not shipped.
QStringList third_party_not_shipped();

/// The components this build contains: the unconditional ones, plus those
/// whose condition holds.
QVector<third_party_component> bundled_components(bool profiling_compiled);

/// True when this build has the profiler compiled in.
bool profiling_compiled();

/// The licence's name as the Licences tab words it, translated; the SPDX
/// text when this file has no wording for it.
QString licence_text(const QString& spdx);

/// The one-line note under a component, translated; empty for none.
QString licence_note(const QString& notice);

} // namespace vss::gui::support
