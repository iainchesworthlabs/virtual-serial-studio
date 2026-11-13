//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "support/app_identity.hpp"

#include <QString>

using namespace vss::gui::support;

TEST_CASE("app identity: the names are the organisation and the program")
{
    REQUIRE_EQ(organisation_name(), QStringLiteral("iainchesworthlabs"));
    REQUIRE_EQ(application_name(), QStringLiteral("Virtual Serial Studio"));
}

TEST_CASE("app identity: the organisation is not repeated as the application")
{
    // Both names equal gave HKCU\Software\X\X on Windows.
    REQUIRE_NE(organisation_name(), application_name());
}

TEST_CASE("app identity: the settings sit under the organisation, then the "
          "application")
{
    const QString location = settings_location();
    const auto organisation = location.indexOf(organisation_name());
    const auto application = location.indexOf(application_name());
    REQUIRE(organisation >= 0);
    REQUIRE(application > organisation);
    // The organisation appears once: the name is not nested twice.
    REQUIRE_EQ(location.count(organisation_name()), 1);
    REQUIRE_EQ(location.count(application_name()), 1);
}

TEST_CASE("app identity: the Windows registry key is the one documented")
{
    const QString location = settings_location();
    if (!location.startsWith(QLatin1Char('\\')))
        return;
    REQUIRE_EQ(location,
               QStringLiteral("\\HKEY_CURRENT_USER\\Software\\iainchesworthlabs"
                              "\\Virtual Serial Studio"));
}
