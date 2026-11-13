//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/app_identity.hpp"

#include <QCoreApplication>
#include <QSettings>

namespace vss::gui::support
{

QString organisation_name()
{
    return QStringLiteral("iainchesworthlabs");
}

QString application_name()
{
    return QStringLiteral("Virtual Serial Studio");
}

void apply_application_identity()
{
    QCoreApplication::setOrganizationName(organisation_name());
    QCoreApplication::setApplicationName(application_name());
}

QString settings_location()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope,
                     organisation_name(), application_name())
        .fileName();
}

} // namespace vss::gui::support
