//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/licence_store.hpp"

#include <QDir>
#include <QProcessEnvironment>

namespace vss::gui
{

QString machine_licence_directory()
{
    // %ProgramData%, which an administrator writes and every user reads.
    const QString root = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("ProgramData"));
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(
        QStringLiteral("Virtual Serial Studio/licences"));
}

} // namespace vss::gui
