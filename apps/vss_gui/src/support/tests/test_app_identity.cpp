//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "support/app_identity.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUrl>

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

namespace
{

/// Sets the program's identity as `main` does and puts the test program's own
/// back afterwards, so the other tests keep their names and their settings.
class program_identity
{
public:
    program_identity()
        : organisation_(QCoreApplication::organizationName()),
          domain_(QCoreApplication::organizationDomain()),
          application_(QCoreApplication::applicationName()),
          format_(QSettings::defaultFormat())
    {
        apply_application_identity();
        // `main` never changes the default format, so it is the native one.
        QSettings::setDefaultFormat(QSettings::NativeFormat);
    }

    program_identity(const program_identity&) = delete;
    program_identity& operator=(const program_identity&) = delete;

    ~program_identity()
    {
        QSettings::setDefaultFormat(format_);
        QCoreApplication::setOrganizationName(organisation_);
        QCoreApplication::setOrganizationDomain(domain_);
        QCoreApplication::setApplicationName(application_);
    }

private:
    QString organisation_;
    QString domain_;
    QString application_;
    QSettings::Format format_;
};

/// True when the name ends in a label that is reserved (RFC 2606, RFC 6761)
/// or is an example name, so it cannot be a real address.
bool is_reserved_name(const QString& host)
{
    const QStringList reserved{
        QStringLiteral("invalid"), QStringLiteral("example"),
        QStringLiteral("test"), QStringLiteral("localhost"),
        QStringLiteral("local")};
    const QStringList labels = host.toLower().split(QLatin1Char('.'));
    if (labels.size() < 2 || labels.contains(QString{}))
        return true;
    if (reserved.contains(labels.last()))
        return true;
    // example.com, example.net and example.org are reserved as well.
    return labels.at(labels.size() - 2) == QLatin1String("example");
}

QString host_of_address(const QString& address)
{
    const auto at = address.lastIndexOf(QLatin1Char('@'));
    return at < 0 ? QString{} : address.mid(at + 1);
}

} // namespace

TEST_CASE("app identity: the reserved-name check rejects reserved names")
{
    REQUIRE(is_reserved_name(QStringLiteral("virtual-serial-studio.invalid")));
    REQUIRE(is_reserved_name(QStringLiteral("virtualserial.example")));
    REQUIRE(is_reserved_name(QStringLiteral("studio.test")));
    REQUIRE(is_reserved_name(QStringLiteral("studio.localhost")));
    REQUIRE(is_reserved_name(QStringLiteral("example.com")));
    REQUIRE(is_reserved_name(QStringLiteral("localhost")));
    REQUIRE(is_reserved_name(QString{}));
    REQUIRE(!is_reserved_name(QStringLiteral("virtual-serial-studio.com")));
    REQUIRE(!is_reserved_name(QStringLiteral("iainchesworthlabs.com")));
}

TEST_CASE("app identity: no address the program shows is a reserved name")
{
    const QString support = support_address();
    REQUIRE_EQ(support.count(QLatin1Char('@')), 1);
    REQUIRE(!support.startsWith(QLatin1Char('@')));
    REQUIRE(!is_reserved_name(host_of_address(support)));

    const QUrl documents(documents_address());
    REQUIRE(documents.isValid());
    REQUIRE_EQ(documents.scheme(), QStringLiteral("https"));
    REQUIRE(!is_reserved_name(documents.host()));

    REQUIRE(!is_reserved_name(organisation_domain()));
}

TEST_CASE("app identity: the addresses are the owner's")
{
    REQUIRE_EQ(support_address(),
               QStringLiteral("support@virtual-serial-studio.com"));
    // `site_url` in mkdocs.yml.
    REQUIRE_EQ(documents_address(),
               QStringLiteral("https://virtual-serial-studio.com/docs/"));
    // Matches the macOS bundle identifier
    // com.iainchesworthlabs.virtualserialstudio.
    REQUIRE_EQ(organisation_domain(), QStringLiteral("iainchesworthlabs.com"));
}

TEST_CASE("app identity: main's identity is set on the application")
{
    const program_identity identity;
    REQUIRE_EQ(QCoreApplication::organizationName(), organisation_name());
    REQUIRE_EQ(QCoreApplication::organizationDomain(), organisation_domain());
    REQUIRE_EQ(QCoreApplication::applicationName(), application_name());
}

TEST_CASE("app identity: settings_location is where QSettings really write")
{
    const program_identity identity;
    const QString location = settings_location();
    REQUIRE_EQ(location, QSettings().fileName());
    REQUIRE(!location.isEmpty());

    if (location.endsWith(QStringLiteral(".plist")))
    {
        // macOS follows the organisation domain, reversed.
        REQUIRE(location.endsWith(
            QStringLiteral("/com.iainchesworthlabs.Virtual Serial Studio."
                           "plist")));
        return;
    }
    if (location.startsWith(QLatin1Char('\\')))
    {
        // The Windows registry key is the one documented.
        REQUIRE_EQ(location,
                   QStringLiteral("\\HKEY_CURRENT_USER\\Software\\"
                                  "iainchesworthlabs\\Virtual Serial Studio"));
        return;
    }
    // Linux and other platforms: the organisation, then the application,
    // each once.
    const auto organisation = location.indexOf(organisation_name());
    const auto application = location.indexOf(application_name());
    REQUIRE(organisation >= 0);
    REQUIRE(application > organisation);
    REQUIRE_EQ(location.count(organisation_name()), 1);
    REQUIRE_EQ(location.count(application_name()), 1);
}
