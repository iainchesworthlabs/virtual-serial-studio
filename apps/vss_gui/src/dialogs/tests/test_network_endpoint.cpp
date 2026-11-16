//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/network_uri.hpp"
#include "widgets/notes.hpp"

#include <boost/asio/error.hpp>

#include <QApplication>
#include <QPushButton>
#include <QRadioButton>

namespace
{

void show(QDialog& dialog)
{
    dialog.show();
    QApplication::processEvents();
}

vss::gui::device_view device(const QString& path)
{
    vss::gui::device_view view;
    view.path = path;
    view.description = QStringLiteral("Serial device");
    return view;
}

void configure(vss::gui::FakeStudioBackend& backend, bool server = false)
{
    vss::gui::capabilities caps;
    caps.network_server = server;
    backend.set_capabilities(caps);
    backend.set_devices({device(QStringLiteral("COM3"))});
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    backend.set_facts(facts);
}

} // namespace

TEST_CASE("network URI: builds, parses and rejects every transport")
{
    REQUIRE_EQ(vss::gui::build_network_uri(vss::gui::network_transport::tcp,
                                           QStringLiteral("0.0.0.0"), 4001),
               QStringLiteral("tcp://0.0.0.0:4001"));
    REQUIRE_EQ(vss::gui::build_network_uri(vss::gui::network_transport::udp,
                                           QStringLiteral("192.168.1.50"),
                                           4002),
               QStringLiteral("udp://192.168.1.50:4002"));
    REQUIRE_EQ(vss::gui::build_network_uri(vss::gui::network_transport::rfc2217,
                                           QStringLiteral("lab-host"), 7001),
               QStringLiteral("rfc2217://lab-host:7001"));
    REQUIRE_EQ(vss::gui::build_network_uri(vss::gui::network_transport::tcp,
                                           QStringLiteral("::1"), 4001),
               QStringLiteral("tcp://[::1]:4001"));

    const auto parsed =
        vss::gui::parse_network_uri(QStringLiteral("udp://10.0.0.5:5000"));
    REQUIRE(parsed.has_value());
    REQUIRE_EQ(parsed->transport, vss::gui::network_transport::udp);
    REQUIRE_EQ(parsed->host, QStringLiteral("10.0.0.5"));
    REQUIRE_EQ(parsed->port, 5000);

    const auto ipv6 =
        vss::gui::parse_network_uri(QStringLiteral("rfc2217://[::1]:7001"));
    REQUIRE(ipv6.has_value());
    REQUIRE_EQ(ipv6->host, QStringLiteral("::1"));

    // The server role waits for others, so it has its own schemes.
    using vss::gui::network_role;
    using vss::gui::network_transport;
    REQUIRE_EQ(vss::gui::build_network_uri(network_transport::tcp,
                                           network_role::server,
                                           QStringLiteral("0.0.0.0"), 4001),
               QStringLiteral("tcp-listen://0.0.0.0:4001"));
    REQUIRE_EQ(vss::gui::build_network_uri(network_transport::udp,
                                           network_role::server,
                                           QStringLiteral("::1"), 4002),
               QStringLiteral("udp-listen://[::1]:4002"));
    REQUIRE_EQ(vss::gui::build_network_uri(network_transport::tcp,
                                           network_role::client,
                                           QStringLiteral("lab"), 4001),
               QStringLiteral("tcp://lab:4001"));
    REQUIRE_EQ(vss::gui::build_network_uri(network_transport::rfc2217,
                                           network_role::server,
                                           QStringLiteral("lab"), 7001),
               QStringLiteral("rfc2217-listen://lab:7001"));

    const auto listening = vss::gui::parse_network_uri(
        QStringLiteral("tcp-listen://127.0.0.1:4001"));
    REQUIRE(listening.has_value());
    REQUIRE_EQ(listening->transport, network_transport::tcp);
    REQUIRE_EQ(listening->role, network_role::server);
    REQUIRE_EQ(listening->host, QStringLiteral("127.0.0.1"));
    REQUIRE_EQ(listening->port, 4001);
    REQUIRE_EQ(vss_test::require_value(vss::gui::parse_network_uri(
                                 QStringLiteral("udp-listen://[::1]:5")))
                   .role,
               network_role::server);
    REQUIRE_EQ(parsed->role, network_role::client);
    const auto rfc_server =
        vss::gui::parse_network_uri(QStringLiteral("rfc2217-listen://h:1"));
    REQUIRE(rfc_server.has_value());
    REQUIRE_EQ(rfc_server->transport, network_transport::rfc2217);
    REQUIRE_EQ(rfc_server->role, network_role::server);

    REQUIRE(!vss::gui::valid_port(0));
    REQUIRE(!vss::gui::valid_port(65536));
    REQUIRE(vss::gui::valid_port(1));
    REQUIRE(vss::gui::valid_port(65535));

    for (const char* bad :
         {"", "tcp://host", "tcp://host:0", "tcp://host:65536",
          "ftp://host:4001", "tcp://:4001", "tcp://[::1:4001"})
    {
        QString error;
        REQUIRE(!vss::gui::parse_network_uri(QString::fromLatin1(bad), &error)
                     .has_value());
        REQUIRE(!error.isEmpty());
    }
}

TEST_CASE("network dialog: client role, URI preview and port validation")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend);
    vss::gui::NetworkEndpointDialog dialog(backend);
    show(dialog);

    REQUIRE(!dialog.server_available());
    REQUIRE_EQ(dialog.role(), vss::gui::network_role::client);
    REQUIRE_EQ(dialog.device_path(), QStringLiteral("COM3"));
    REQUIRE(!dialog.server_selected());

    QRadioButton* server = nullptr;
    for (QRadioButton* radio : dialog.findChildren<QRadioButton*>())
    {
        if (radio->text() ==
            QStringLiteral("Wait for other computers to connect (server)"))
        {
            server = radio;
        }
    }
    REQUIRE(server != nullptr);
    REQUIRE(!server->isEnabled());
    REQUIRE_EQ(server->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!dialog.test_button()->isEnabled());
    REQUIRE_EQ(dialog.test_button()->toolTip(),
               QStringLiteral("Not available in this build"));

    dialog.set_server_address(QStringLiteral("192.168.1.50"));
    dialog.set_port(4001);
    REQUIRE_EQ(dialog.uri(), QStringLiteral("tcp://192.168.1.50:4001"));
    REQUIRE_EQ(dialog.command(),
               QStringLiteral("vss_cli bridge serial://COM3?baud=115200 "
                              "tcp://192.168.1.50:4001"));
    REQUIRE_EQ(
        dialog.accessible_result_text(),
        QStringLiteral("Studio connects to 192.168.1.50 on port 4001 and talks "
                       "to COM3. Data flows both ways, and the Monitor shows "
                       "both directions."));

    dialog.set_transport(vss::gui::network_transport::udp);
    REQUIRE_EQ(dialog.uri(), QStringLiteral("udp://192.168.1.50:4001"));
    dialog.set_transport(vss::gui::network_transport::rfc2217);
    REQUIRE_EQ(dialog.role(), vss::gui::network_role::client);
    REQUIRE_EQ(dialog.uri(), QStringLiteral("rfc2217://192.168.1.50:4001"));
    dialog.set_transport(vss::gui::network_transport::tcp);

    dialog.set_port(0);
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(dialog.error_body(),
               QStringLiteral("Choose a port from 1 to 65535."));
    REQUIRE(backend.links().empty());

    dialog.set_port(4002);
    REQUIRE(!dialog.error_visible());
    REQUIRE(dialog.submit());
    REQUIRE_EQ(dialog.result_port(), 4002);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
}

TEST_CASE("network dialog: RFC 2217 can wait for other computers")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend, true);
    vss::gui::NetworkEndpointDialog dialog(backend, false, 0);
    show(dialog);

    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::server);
    dialog.set_listen_address(QStringLiteral("0.0.0.0"));
    dialog.set_port(7001);
    REQUIRE(dialog.server_selected());
    REQUIRE_EQ(dialog.role(), vss::gui::network_role::server);
    REQUIRE_EQ(dialog.uri(), QStringLiteral("rfc2217-listen://0.0.0.0:7001"));

    // Without the capability the same choice falls back to connecting.
    vss::gui::FakeStudioBackend plain;
    configure(plain, false);
    vss::gui::NetworkEndpointDialog client(plain, false, 0);
    show(client);
    client.set_transport(vss::gui::network_transport::rfc2217);
    REQUIRE_EQ(client.role(), vss::gui::network_role::client);
}

TEST_CASE("network dialog: server role appears only with the capability")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend, true);
    vss::gui::NetworkEndpointDialog dialog(backend, false, 0);
    show(dialog);

    REQUIRE(dialog.server_available());
    REQUIRE(dialog.server_selected());
    REQUIRE_EQ(dialog.role(), vss::gui::network_role::server);
    REQUIRE(dialog.test_button()->isEnabled());
    REQUIRE_EQ(
        dialog.accessible_result_text(),
        QStringLiteral("Other computers can connect to this PC on port 4001 "
                       "and talk to COM3. Data flows both ways, and the "
                       "Monitor shows both directions."));

    bool firewall = false;
    for (vss::gui::NoteBanner* note :
         dialog.findChildren<vss::gui::NoteBanner*>())
    {
        if (note->text().contains(QStringLiteral("Windows Firewall")))
            firewall = note->isVisibleTo(&dialog);
    }
    REQUIRE(firewall);

    dialog.test_button()->click();
    QApplication::processEvents();
    REQUIRE_EQ(dialog.listen_address(), QStringLiteral("127.0.0.1"));
    REQUIRE_EQ(dialog.uri(), QStringLiteral("tcp-listen://127.0.0.1:4001"));

    dialog.set_listen_address(QStringLiteral("0.0.0.0"));
    dialog.set_port(4010);
    REQUIRE_EQ(dialog.uri(), QStringLiteral("tcp-listen://0.0.0.0:4010"));
    REQUIRE(dialog.submit());
    REQUIRE_EQ(dialog.result_port(), 4010);
}

TEST_CASE("network dialog: a known bind failure keeps the dialog open")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend);
    vss::gui::NetworkEndpointDialog dialog(backend);
    show(dialog);
    dialog.set_server_address(QStringLiteral("127.0.0.1"));
    dialog.set_port(4001);

    backend.fail_next(boost::asio::error::make_error_code(
        boost::asio::error::address_in_use));
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(dialog.error_title(),
               QStringLiteral("Could not start the bridge"));
    REQUIRE(dialog.error_body().contains(
        QStringLiteral("Port 4001 is already in use")));
    REQUIRE(!dialog.result_link().has_value());

    dialog.set_port(4002);
    REQUIRE(!dialog.error_visible());
    REQUIRE(dialog.submit());
    REQUIRE_EQ(dialog.result_port(), 4002);
}

TEST_CASE("network dialog: Advanced address editing wins over the fields")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend);
    vss::gui::NetworkEndpointDialog dialog(backend, true);
    show(dialog);
    dialog.set_server_address(QStringLiteral("192.168.1.50"));
    dialog.set_port(4001);

    dialog.set_address_override(QStringLiteral("udp://10.0.0.5:5000"));
    REQUIRE(dialog.override_active());
    REQUIRE_EQ(dialog.uri(), QStringLiteral("udp://10.0.0.5:5000"));
    REQUIRE_EQ(dialog.command(),
               QStringLiteral("vss_cli bridge serial://COM3?baud=115200 "
                              "udp://10.0.0.5:5000"));
    REQUIRE(dialog.submit());
    REQUIRE_EQ(dialog.result_port(), 5000);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});

    vss::gui::NetworkEndpointDialog invalid(backend, true);
    show(invalid);
    invalid.set_address_override(QStringLiteral("not an address"));
    REQUIRE(!invalid.submit());
    REQUIRE(invalid.error_visible());
    REQUIRE(invalid.error_body().contains(
        QStringLiteral("Enter an address such as")));
}

TEST_CASE("network dialog: a build without bridging disables the action")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend);
    vss::gui::capabilities caps;
    caps.bridge = false;
    backend.set_capabilities(caps);
    vss::gui::NetworkEndpointDialog dialog(backend);
    show(dialog);
    REQUIRE(!dialog.primary_button()->isEnabled());
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(dialog.error_title(),
               QStringLiteral("Could not start the bridge"));
}

TEST_CASE("network URI: which RFC 2217 servers other computers can reach")
{
    using vss::gui::is_loopback_host;
    using vss::gui::rfc2217_server_is_open;
    REQUIRE(is_loopback_host(QStringLiteral("127.0.0.1")));
    REQUIRE(is_loopback_host(QStringLiteral("127.8.0.2")));
    REQUIRE(is_loopback_host(QStringLiteral("::1")));
    REQUIRE(is_loopback_host(QStringLiteral("localhost")));
    REQUIRE(!is_loopback_host(QStringLiteral("0.0.0.0")));
    REQUIRE(!is_loopback_host(QStringLiteral("::")));
    REQUIRE(!is_loopback_host(QStringLiteral("192.168.1.10")));

    REQUIRE(rfc2217_server_is_open(
        QStringLiteral("rfc2217-listen://0.0.0.0:7001")));
    // Any address that is not this computer, not only every interface.
    REQUIRE(rfc2217_server_is_open(
        QStringLiteral("rfc2217-listen://192.168.1.10:7001")));
    REQUIRE(!rfc2217_server_is_open(
        QStringLiteral("rfc2217-listen://127.0.0.1:7001")));
    REQUIRE(
        !rfc2217_server_is_open(QStringLiteral("rfc2217-listen://[::1]:7001")));
    // A list of who may connect is the answer to the warning.
    REQUIRE(!rfc2217_server_is_open(QStringLiteral(
        "rfc2217-listen://192.168.1.10:7001?allow=192.168.1.0/24")));
    // Only the server of this protocol is warned about.
    REQUIRE(
        !rfc2217_server_is_open(QStringLiteral("rfc2217://192.168.1.10:7001")));
    REQUIRE(
        !rfc2217_server_is_open(QStringLiteral("tcp-listen://0.0.0.0:4001")));
    REQUIRE(!rfc2217_server_is_open(QStringLiteral("nonsense")));
}

TEST_CASE("network dialog: an RFC 2217 server reachable from the network is "
          "warned about")
{
    vss::gui::FakeStudioBackend backend;
    configure(backend, true);
    vss::gui::NetworkEndpointDialog dialog(backend, false, 0);
    show(dialog);

    const auto warned = [&]
    {
        for (vss::gui::NoteBanner* note :
             dialog.findChildren<vss::gui::NoteBanner*>())
        {
            if (note->objectName() == QStringLiteral("networkOpenWarning"))
                return note->isVisibleTo(&dialog);
        }
        return false;
    };

    // TCP is not warned about, even on every interface.
    REQUIRE_EQ(dialog.listen_address(), QStringLiteral("0.0.0.0"));
    REQUIRE(!warned());

    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::server);
    dialog.set_port(7001);
    dialog.set_listen_address(QStringLiteral("0.0.0.0"));
    REQUIRE(warned());
    dialog.set_listen_address(QStringLiteral("192.168.1.10"));
    REQUIRE(warned());
    dialog.set_listen_address(QStringLiteral("127.0.0.1"));
    REQUIRE(!warned());

    // The Advanced address wins, and the warning follows it.
    dialog.set_advanced(true);
    dialog.set_address_override(
        QStringLiteral("rfc2217-listen://0.0.0.0:7002"));
    QApplication::processEvents();
    REQUIRE(warned());
    dialog.set_address_override(
        QStringLiteral("rfc2217-listen://0.0.0.0:7002?allow=10.0.0.0/8"));
    QApplication::processEvents();
    REQUIRE(!warned());
    dialog.clear_address_override();
    QApplication::processEvents();
    REQUIRE(!warned());

    // Back to the client role: nothing to warn about.
    dialog.set_listen_address(QStringLiteral("0.0.0.0"));
    REQUIRE(warned());
    dialog.set_role(vss::gui::network_role::client);
    REQUIRE(!warned());
}

namespace
{

vss::gui::capabilities network_caps()
{
    vss::gui::capabilities caps;
    caps.network_server = true;
    caps.network_edit = true;
    caps.network_waiting = true;
    return caps;
}

} // namespace

TEST_CASE("network dialog: a port in Studio is served as it is, not opened "
          "again by its name")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    backend.set_devices({device(QStringLiteral("COM134"))});
    const auto made =
        backend.create_driver_port(QStringLiteral("port-3"), true, 134);
    REQUIRE(made);
    const auto before = backend.ports().size();

    vss::gui::NetworkEndpointDialog dialog(backend, true, *made);
    show(dialog);
    // The port in Studio is chosen, and the device it holds is not listed
    // again as a real device.
    REQUIRE_EQ(dialog.selected_port(), *made);
    REQUIRE(dialog.source_uri().isEmpty());
    REQUIRE(!dialog.device_paths().contains(QStringLiteral("COM134")));

    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::server);
    dialog.set_port(4001);
    REQUIRE(dialog.submit());
    // One new port, linked to the port that was there: no second COM134.
    REQUIRE_EQ(backend.ports().size(), before + 1);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    for (const auto& port : backend.ports())
        REQUIRE(!port.name.startsWith(QStringLiteral("bridge-")));
}

TEST_CASE("network dialog: a failed create leaves nothing behind")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    const auto made =
        backend.create_driver_port(QStringLiteral("port-3"), true, 134);
    REQUIRE(made);
    const auto before = backend.ports().size();

    vss::gui::NetworkEndpointDialog dialog(backend, true, *made);
    show(dialog);
    dialog.set_port(4001);
    backend.fail_next(vss::make_error_code(vss::errc::permission_denied));
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(backend.ports().size(), before);
    REQUIRE(backend.links().empty());

    // The same dialog works when the cause is gone.
    REQUIRE(dialog.submit());
    REQUIRE_EQ(backend.ports().size(), before + 1);
}

TEST_CASE("network dialog: two ports cannot listen on one address")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    const auto first =
        backend.create_driver_port(QStringLiteral("port-1"), true, 18);
    const auto second =
        backend.create_driver_port(QStringLiteral("port-2"), true, 19);
    REQUIRE(first);
    REQUIRE(second);

    {
        vss::gui::NetworkEndpointDialog dialog(backend, true, *first);
        dialog.set_transport(vss::gui::network_transport::tcp);
        dialog.set_role(vss::gui::network_role::server);
        dialog.set_port(4001);
        REQUIRE(dialog.submit());
    }
    const auto ports = backend.ports().size();
    const auto links = backend.links().size();

    vss::gui::NetworkEndpointDialog dialog(backend, true, *second);
    show(dialog);
    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::server);
    dialog.set_port(4001);
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_body().contains(QStringLiteral(
        "Port 4001 is already used by another port in this setup")));
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().size(), links);

    // Another port number is fine.
    dialog.set_port(4002);
    REQUIRE(dialog.submit());
}

TEST_CASE("network dialog: a client the other computer refuses can be made "
          "anyway")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    const auto made =
        backend.create_driver_port(QStringLiteral("port-1"), true, 18);
    REQUIRE(made);

    vss::gui::NetworkEndpointDialog dialog(backend, true, *made);
    show(dialog);
    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::client);
    dialog.set_server_address(QStringLiteral("192.168.1.50"));
    dialog.set_port(2217);
    REQUIRE(!dialog.anyway_button()->isVisibleTo(&dialog));

    backend.fail_next(boost::asio::error::make_error_code(
        boost::asio::error::connection_refused));
    REQUIRE(!dialog.submit());
    REQUIRE_EQ(dialog.error_title(), QStringLiteral("Could not connect"));
    REQUIRE(dialog.anyway_button()->isVisibleTo(&dialog));
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});

    // The same refusal again, and then the port is made and waits.
    backend.fail_next(boost::asio::error::make_error_code(
        boost::asio::error::connection_refused));
    REQUIRE(dialog.submit_anyway());
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});

    // Without the capability the button is never offered.
    vss::gui::FakeStudioBackend plain;
    vss::gui::capabilities caps = network_caps();
    caps.network_waiting = false;
    plain.set_capabilities(caps);
    vss::gui::NetworkEndpointDialog without(plain, true);
    show(without);
    without.set_role(vss::gui::network_role::client);
    without.set_server_address(QStringLiteral("192.168.1.50"));
    plain.fail_next(boost::asio::error::make_error_code(
        boost::asio::error::connection_refused));
    REQUIRE(!without.submit());
    REQUIRE(!without.anyway_button()->isVisibleTo(&without));
}

TEST_CASE("network dialog: a client needs no COM port of its own")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    vss::gui::NetworkEndpointDialog dialog(backend, true);
    show(dialog);
    REQUIRE(dialog.select_new_port());
    REQUIRE(dialog.new_port_selected());
    dialog.set_transport(vss::gui::network_transport::rfc2217);
    dialog.set_role(vss::gui::network_role::client);
    dialog.set_server_address(QStringLiteral("192.168.1.50"));
    dialog.set_port(2217);
    REQUIRE(dialog.submit());
    // The client and the Studio port made for it, linked.
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
}

TEST_CASE("network dialog: a network port is changed in place and keeps its "
          "links")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(network_caps());
    const auto served =
        backend.create_driver_port(QStringLiteral("port-1"), true, 18);
    REQUIRE(served);
    vss::endpoint_id network = 0;
    {
        vss::gui::NetworkEndpointDialog dialog(backend, true, *served);
        dialog.set_transport(vss::gui::network_transport::tcp);
        dialog.set_role(vss::gui::network_role::server);
        dialog.set_port(4001);
        REQUIRE(dialog.submit());
        for (const auto& port : backend.ports())
        {
            if (port.id != *served)
                network = port.id;
        }
    }
    REQUIRE(network != 0);
    const auto link = backend.links().front().id;

    vss::gui::NetworkEndpointDialog edit(backend, true);
    REQUIRE(edit.edit_port(network));
    show(edit);
    REQUIRE_EQ(edit.editing_port(), network);
    REQUIRE_EQ(edit.role(), vss::gui::network_role::server);
    REQUIRE_EQ(edit.port(), 4001);
    REQUIRE_EQ(edit.transport(), vss::gui::network_transport::tcp);

    // Server to client of another computer, with the same links.
    edit.set_role(vss::gui::network_role::client);
    edit.set_server_address(QStringLiteral("192.168.1.50"));
    edit.set_port(4010);
    REQUIRE(edit.submit());
    const auto after = backend.port(network);
    REQUIRE(after.has_value());
    REQUIRE_EQ(after->path, QStringLiteral("tcp://192.168.1.50:4010"));
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link);

    // A port that is not a network port cannot be edited here.
    vss::gui::NetworkEndpointDialog other(backend, true);
    REQUIRE(!other.edit_port(*served));
}
