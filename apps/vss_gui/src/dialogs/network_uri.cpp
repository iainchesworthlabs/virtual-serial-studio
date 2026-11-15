//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/network_uri.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace vss::gui
{

QString network_scheme(network_transport transport)
{
    switch (transport)
    {
    case network_transport::tcp:
        return QStringLiteral("tcp");
    case network_transport::udp:
        return QStringLiteral("udp");
    case network_transport::rfc2217:
        return QStringLiteral("rfc2217");
    }
    return QStringLiteral("tcp");
}

QString network_transport_name(network_transport transport)
{
    switch (transport)
    {
    case network_transport::tcp:
        return QStringLiteral("TCP");
    case network_transport::udp:
        return QStringLiteral("UDP");
    case network_transport::rfc2217:
        return QStringLiteral("RFC 2217");
    }
    return QStringLiteral("TCP");
}

QString build_network_uri(network_transport transport, const QString& host,
                          int port)
{
    return build_network_uri(transport, network_role::client, host, port);
}

QString build_network_uri(network_transport transport, network_role role,
                          const QString& host, int port)
{
    QString address = host.trimmed();
    if (address.contains(QLatin1Char(':')) &&
        !address.startsWith(QLatin1Char('[')))
    {
        address = QStringLiteral("[%1]").arg(address);
    }
    QString scheme = network_scheme(transport);
    if (role == network_role::server)
        scheme += QStringLiteral("-listen");
    return QStringLiteral("%1://%2:%3")
        .arg(scheme, address, QString::number(port));
}

namespace
{

bool all_hex(const QString& text)
{
    return std::all_of(text.begin(), text.end(),
                       [](QChar character)
                       {
                           const char16_t code = character.unicode();
                           return (code >= u'0' && code <= u'9') ||
                                  (code >= u'a' && code <= u'f') ||
                                  (code >= u'A' && code <= u'F');
                       });
}

bool dotted_ipv4(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    return std::all_of(parts.begin(), parts.end(),
                       [](const QString& part)
                       {
                           bool ok = false;
                           const int value = part.toInt(&ok);
                           return ok && value >= 0 && value <= 255 &&
                                  part.size() <= 3 &&
                                  std::all_of(part.begin(), part.end(),
                                              [](QChar digit)
                                              { return digit.isDigit(); });
                       });
}

/// An IPv6 address, with an optional `%zone` and an IPv4 tail.
bool ipv6_address(const QString& text)
{
    QString address = text;
    const qsizetype zone = address.indexOf(QLatin1Char('%'));
    if (zone >= 0)
    {
        if (zone + 1 >= address.size())
            return false;
        address.truncate(zone);
    }
    if (!address.contains(QLatin1Char(':')) ||
        address.contains(QStringLiteral(":::")))
        return false;
    const qsizetype gaps = address.count(QStringLiteral("::"));
    if (gaps > 1)
        return false;
    // One colon at an end is not a gap.
    if ((address.startsWith(QLatin1Char(':')) &&
         !address.startsWith(QStringLiteral("::"))) ||
        (address.endsWith(QLatin1Char(':')) &&
         !address.endsWith(QStringLiteral("::"))))
        return false;

    QStringList groups = address.split(QLatin1Char(':'));
    groups.removeAll(QString());
    // `::` alone is the address that means "any", not one a device has.
    if (groups.isEmpty())
        return false;
    int width = 0;
    for (qsizetype index = 0; index < groups.size(); ++index)
    {
        const QString& group = groups[index];
        if (index + 1 == groups.size() && group.contains(QLatin1Char('.')))
        {
            if (!dotted_ipv4(group))
                return false;
            width += 2;
            continue;
        }
        if (group.size() > 4 || !all_hex(group))
            return false;
        ++width;
    }
    return gaps == 1 ? width < 8 : width == 8;
}

} // namespace

bool valid_remote_host(const QString& host)
{
    return !host.isEmpty() && !host.contains(QLatin1Char(' ')) &&
           !host.contains(QLatin1Char('/'));
}

std::optional<device_address> parse_device_address(const QString& text,
                                                   QString* error)
{
    VSS_PROFILE_ZONE("gui.dialogs.network_uri.parse_device_address");
    if (error != nullptr)
        error->clear();
    const auto refuse = [error](const QString& why)
    {
        if (error != nullptr)
            *error = why;
        return std::optional<device_address>{};
    };

    device_address result;
    QString rest = text.trimmed();
    const qsizetype scheme_end = rest.indexOf(QStringLiteral("://"));
    if (scheme_end >= 0)
    {
        const QString scheme = rest.left(scheme_end).toLower();
        if (scheme == QLatin1String("tcp"))
            result.transport = network_transport::tcp;
        else if (scheme == QLatin1String("rfc2217"))
            result.transport = network_transport::rfc2217;
        else
            return refuse(network_address_error());
        rest = rest.mid(scheme_end + 3);
    }
    const auto unfit = [](QChar character)
    {
        return character.isSpace() || character == QLatin1Char('?') ||
               character == QLatin1Char('#') || character == QLatin1Char('\\');
    };
    if (rest.isEmpty() || std::any_of(rest.begin(), rest.end(), unfit))
        return refuse(network_address_error());

    QString host;
    QString port_text;
    bool has_port = false;
    if (rest.startsWith(QLatin1Char('[')))
    {
        const qsizetype close = rest.indexOf(QLatin1Char(']'));
        if (close < 0)
            return refuse(network_address_error());
        host = rest.mid(1, close - 1);
        const QString after = rest.mid(close + 1);
        if (!after.isEmpty())
        {
            if (!after.startsWith(QLatin1Char(':')))
                return refuse(network_address_error());
            has_port = true;
            port_text = after.mid(1);
        }
        // Brackets hold an IPv6 address and nothing else.
        if (!ipv6_address(host))
            return refuse(network_address_error());
    }
    else if (rest.count(QLatin1Char(':')) == 1)
    {
        const qsizetype colon = rest.indexOf(QLatin1Char(':'));
        host = rest.left(colon);
        port_text = rest.mid(colon + 1);
        has_port = true;
    }
    else
    {
        // No colon is a name or an IPv4 address.  Two or more is an IPv6
        // address with no port, so `192.168.1.50:4001:4001` is not one.
        host = std::move(rest);
        if (host.contains(QLatin1Char(':')) && !ipv6_address(host))
            return refuse(network_address_error());
    }

    if (has_port)
    {
        int port = 0;
        // `toInt` takes a sign, so `+80` is checked for here.
        const bool digits =
            !port_text.isEmpty() &&
            std::all_of(port_text.begin(), port_text.end(),
                        [](QChar digit) { return digit.isDigit(); });
        if (!digits || !parse_port(port_text, port))
            return refuse(network_port_error());
        result.port = port;
    }
    constexpr qsizetype kLongestHost = 253;
    if (!valid_remote_host(host) || host.size() > kLongestHost ||
        host.contains(QLatin1Char('[')) || host.contains(QLatin1Char(']')) ||
        host.contains(QLatin1Char('@')))
        return refuse(network_address_error());
    result.host = std::move(host);
    return result;
}

bool valid_port(int port)
{
    return port >= 1 && port <= 65535;
}

bool parse_port(const QString& text, int& port)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    if (!ok || !valid_port(value))
        return false;
    port = value;
    return true;
}

std::optional<network_endpoint> parse_network_uri(const QString& text,
                                                  QString* error)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone, "gui.dialogs.network_uri.parse");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, text.toStdString());
    if (error != nullptr)
        error->clear();
    // Every address needs a scheme followed by ://.
    const QString value = text.trimmed();
    const qsizetype scheme_end = value.indexOf(QStringLiteral("://"));
    if (scheme_end <= 0)
    {
        if (error != nullptr)
            *error = network_address_error();
        return std::nullopt;
    }

    // Map the scheme to a transport. A -listen suffix means this side is the
    // server; otherwise it is the client.
    const QString scheme = value.left(scheme_end);
    network_endpoint endpoint;
    endpoint.role = network_role::client;
    if (scheme == QLatin1String("tcp"))
        endpoint.transport = network_transport::tcp;
    else if (scheme == QLatin1String("udp"))
        endpoint.transport = network_transport::udp;
    else if (scheme == QLatin1String("rfc2217"))
        endpoint.transport = network_transport::rfc2217;
    else if (scheme == QLatin1String("tcp-listen"))
    {
        endpoint.transport = network_transport::tcp;
        endpoint.role = network_role::server;
    }
    else if (scheme == QLatin1String("udp-listen"))
    {
        endpoint.transport = network_transport::udp;
        endpoint.role = network_role::server;
    }
    else if (scheme == QLatin1String("rfc2217-listen"))
    {
        endpoint.transport = network_transport::rfc2217;
        endpoint.role = network_role::server;
    }
    else
    {
        if (error != nullptr)
            *error = network_address_error();
        return std::nullopt;
    }

    // Only the host and port matter here, so ignore any query string such as
    // ?allow=...
    QString rest = value.mid(scheme_end + 3);
    const qsizetype query = rest.indexOf(QLatin1Char('?'));
    if (query >= 0)
        rest = rest.left(query);
    if (rest.isEmpty())
    {
        if (error != nullptr)
            *error = network_address_error();
        return std::nullopt;
    }

    // Split host from port. A bracketed host is IPv6, so its port follows ]:
    // otherwise the port follows the last colon.
    QString host;
    QString port_text;
    if (rest.startsWith(QLatin1Char('[')))
    {
        const qsizetype close = rest.indexOf(QLatin1Char(']'));
        if (close < 0 || close + 1 >= rest.size() ||
            rest.at(close + 1) != QLatin1Char(':'))
        {
            if (error != nullptr)
                *error = network_address_error();
            return std::nullopt;
        }
        host = rest.mid(1, close - 1);
        port_text = rest.mid(close + 2);
    }
    else
    {
        const qsizetype colon = rest.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0)
        {
            if (error != nullptr)
                *error = network_address_error();
            return std::nullopt;
        }
        host = rest.left(colon);
        port_text = rest.mid(colon + 1);
    }

    // Check the port, then reject empty hosts and hosts containing spaces or
    // slashes.
    int port = 0;
    if (!parse_port(port_text, port))
    {
        if (error != nullptr)
            *error = network_port_error();
        return std::nullopt;
    }
    if (host.trimmed().isEmpty() || host.contains(QLatin1Char(' ')) ||
        host.contains(QLatin1Char('/')))
    {
        if (error != nullptr)
            *error = network_address_error();
        return std::nullopt;
    }

    // Only a fully valid address reaches this point.
    endpoint.host = std::move(host);
    endpoint.port = port;
    return endpoint;
}

bool is_loopback_host(const QString& host)
{
    const QString text = host.trimmed().toLower();
    return text == QLatin1String("localhost") || text == QLatin1String("::1") ||
           text.startsWith(QLatin1String("127."));
}

bool rfc2217_server_is_open(const QString& uri)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone,
                           "gui.dialogs.network_uri.rfc2217_probe");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, uri.toStdString());
    const auto parsed = parse_network_uri(uri);
    if (!parsed || parsed->role != network_role::server ||
        parsed->transport != network_transport::rfc2217)
        return false;
    // A list of who may connect is the person's answer to the warning.
    if (uri.contains(QLatin1String("?allow=")))
        return false;
    return !is_loopback_host(parsed->host);
}

int network_port_from_uri(const QString& uri)
{
    const auto parsed = parse_network_uri(uri);
    return parsed ? parsed->port : 0;
}

QString network_address_error()
{
    return QCoreApplication::translate(
        "NetworkEndpoint", "Enter an address such as tcp://192.168.1.50:4001.");
}

QString network_port_error()
{
    return QCoreApplication::translate("NetworkEndpoint",
                                       "Choose a port from 1 to 65535.");
}

} // namespace vss::gui
