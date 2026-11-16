//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/package_install.hpp"

namespace vss::gui::updates
{

QString package_install_command(vss::update::file_kind kind,
                                const QString& path)
{
    // A path with a space or a quote in it is written so that a shell
    // reads it as one word.
    QString quoted = path;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    quoted = QLatin1Char('\'') + quoted + QLatin1Char('\'');
    if (kind == vss::update::file_kind::linux_deb)
        return QStringLiteral("sudo apt install %1").arg(quoted);
    if (kind == vss::update::file_kind::linux_rpm)
        return QStringLiteral("sudo dnf install %1").arg(quoted);
    return {};
}

} // namespace vss::gui::updates
