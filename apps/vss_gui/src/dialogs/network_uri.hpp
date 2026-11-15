//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_DIALOGS_NETWORK_URI_HPP
#define VSS_GUI_DIALOGS_NETWORK_URI_HPP

#include <QString>

#include <optional>

namespace vss::gui
{

enum class network_transport
{
    tcp,
    udp,
    rfc2217
};

enum class network_role
{
    server,
    client
};

/// One TCP, UDP or RFC 2217 address, before it is turned into a URI.
struct network_endpoint
{
    network_transport transport = network_transport::tcp;
    network_role role = network_role::server;
    QString host;
    int port = 4001;
};

QString network_scheme(network_transport transport);
QString network_transport_name(network_transport transport);

/// The address of an endpoint that connects: `tcp://192.168.1.50:4001`.  IPv6
/// literals are bracketed.
QString build_network_uri(network_transport transport, const QString& host,
                          int port);

/// As above, and for the server role `tcp-listen://0.0.0.0:4001`,
/// `udp-listen://0.0.0.0:4001` or `rfc2217-listen://0.0.0.0:4001`.
QString build_network_uri(network_transport transport, network_role role,
                          const QString& host, int port);

/// True for 1 to 65535 inclusive.
bool valid_port(int port);
bool parse_port(const QString& text, int& port);

/// Parses the schemes the engine's `attach_serial` accepts for a network
/// endpoint: `tcp`, `udp` and `rfc2217` connect (the client role), and
/// `tcp-listen`, `udp-listen` and `rfc2217-listen` wait for others (the server
/// role).
std::optional<network_endpoint> parse_network_uri(const QString& text,
                                                  QString* error = nullptr);

/// True for a host a client can be told to connect to: not empty, with no
/// space and no slash in it.
bool valid_remote_host(const QString& host);

/// What a person typed for the address of a device on the network: a host,
/// which may carry a port and a protocol with it.
struct device_address
{
    /// Set when the text began with `tcp://` or `rfc2217://`.
    std::optional<network_transport> transport;
    /// The host alone: a name, an IPv4 address or an IPv6 address without
    /// brackets.
    QString host;
    /// Set when the text gave a port: `host:port` or `[ipv6]:port`.
    std::optional<int> port;
};

/// Reads `host`, `host:port`, `[ipv6]`, `[ipv6]:port`, a bare IPv6 address
/// such as `fd00::5`, and any of them after `tcp://` or `rfc2217://`.  A
/// second port, a path, a space, an address with two or more colons that is
/// not an IPv6 address, another protocol and a host over 253 characters are
/// refused, with `error` set to the sentence for it.
std::optional<device_address> parse_device_address(const QString& text,
                                                   QString* error = nullptr);

/// True for `127.0.0.0/8`, `::1` and `localhost`: an address only this
/// computer can reach. `0.0.0.0`, `::` and an empty host are not.
bool is_loopback_host(const QString& host);

/// True when the URI is for an RFC 2217 server that other computers can reach
/// and that has no `allow=` list: the case that needs a warning, because
/// RFC 2217 has no password.
bool rfc2217_server_is_open(const QString& uri);

/// The port number at the end of a URI, or 0 when there is none.
int network_port_from_uri(const QString& uri);

/// The sentence used for a validation failure.
QString network_address_error();
QString network_port_error();

} // namespace vss::gui

#endif // VSS_GUI_DIALOGS_NETWORK_URI_HPP
