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

QString organisation_domain()
{
    return QStringLiteral("iainchesworthlabs.com");
}

QString application_name()
{
    return QStringLiteral("Virtual Serial Studio");
}

QString support_address()
{
    return QStringLiteral("support@virtual-serial-studio.com");
}

QString documents_address()
{
    return QStringLiteral("https://virtual-serial-studio.com/docs/");
}

void apply_application_identity()
{
    QCoreApplication::setOrganizationName(organisation_name());
    QCoreApplication::setOrganizationDomain(organisation_domain());
    QCoreApplication::setApplicationName(application_name());
}

QString settings_location()
{
    return QSettings().fileName();
}

} // namespace vss::gui::support
