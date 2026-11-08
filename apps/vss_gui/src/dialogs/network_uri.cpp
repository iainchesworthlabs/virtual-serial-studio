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
