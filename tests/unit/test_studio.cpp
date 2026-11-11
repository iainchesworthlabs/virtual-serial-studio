//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/loopback_transport.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/studio/studio.hpp>
#include <vss/testkit/harness.hpp>

#include "support/test_support.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

TEST_CASE("studio: loopback virtual ports advertise their path")
{
    studio app;

    const auto created =
        app.create_virtual_port("v1", virtual_port_kind::loopback);
    REQUIRE(created);
    REQUIRE_EQ((*created)->name(), std::string("v1"));
    REQUIRE_EQ(app.advertised_path((*created)->id()),
               std::string("loopback:v1"));
    REQUIRE_EQ(app.find("v1")->id(), (*created)->id());

    const auto infos = app.endpoint_infos();
    REQUIRE_EQ(infos.size(), std::size_t{1});
    REQUIRE_EQ(infos[0].path, std::string("loopback:v1"));
    REQUIRE_EQ(infos[0].kind, endpoint_kind::virtual_port);
}

TEST_CASE("studio: duplicate names are made unique")
{
    studio app;
    const auto first =
        app.create_virtual_port("port", virtual_port_kind::loopback);
    const auto second =
        app.create_virtual_port("port", virtual_port_kind::loopback);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_EQ((*first)->name(), std::string("port"));
    REQUIRE_EQ((*second)->name(), std::string("port-2"));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{2});
}

TEST_CASE("studio: a renamed port keeps its links and its saved name")
{
    studio app;
    const auto first =
        app.create_virtual_port("first", virtual_port_kind::loopback);
    const auto second =
        app.create_virtual_port("second", virtual_port_kind::loopback);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(app.connect("first", "second"));

    REQUIRE(app.rename_port((*first)->id(), "renamed"));
    REQUIRE_EQ((*first)->name(), std::string("renamed"));
    REQUIRE(app.find("first") == nullptr);
    REQUIRE_EQ(app.find("renamed")->id(), (*first)->id());
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{2});
    // A loopback port is advertised by the name it was made with.
    REQUIRE_EQ(app.advertised_path((*first)->id()),
               std::string("loopback:first"));

    // The saved setup uses the new name in the port list and in the link.
    const auto document = app.snapshot();
    REQUIRE_EQ(document.ports.size(), std::size_t{2});
    bool found = false;
    for (const auto& port : document.ports)
    {
        REQUIRE(port.name != "first");
        found = found || port.name == "renamed";
    }
    REQUIRE(found);
    REQUIRE_EQ(document.links.size(), std::size_t{1});
    bool linked = false;
    for (const auto& name : document.links.front().sources)
        linked = linked || name == "renamed";
    for (const auto& name : document.links.front().targets)
        linked = linked || name == "renamed";
    REQUIRE(linked);

    // A saved setup can be applied again under the new name.
    studio other;
    REQUIRE(other.apply(document));
    REQUIRE(other.find("renamed") != nullptr);
    REQUIRE(other.find("first") == nullptr);
}

TEST_CASE("studio: rename refuses a bad name and an unknown port")
{
    studio app;
    const auto first =
        app.create_virtual_port("first", virtual_port_kind::loopback);
    const auto second =
        app.create_virtual_port("second", virtual_port_kind::loopback);
    REQUIRE(first);
    REQUIRE(second);

    REQUIRE_EQ(app.rename_port((*first)->id(), ""),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(app.rename_port((*first)->id(), "second"),
               make_error_code(errc::already_exists));
    REQUIRE_EQ(app.rename_port(endpoint_id{9999}, "x"),
               make_error_code(errc::not_found));
    // The same name is a no-op, not an error.
    REQUIRE(app.rename_port((*first)->id(), "first"));
    REQUIRE_EQ((*first)->name(), std::string("first"));
    REQUIRE_EQ((*second)->name(), std::string("second"));
}

TEST_CASE("studio: mock ports are scriptable")
{
    studio app;
    const auto created = app.create_mock_port("device");
    REQUIRE(created);
    REQUIRE_EQ((*created)->kind(), endpoint_kind::mock);
    REQUIRE_EQ((*created)->stats().bytes_rx, std::uint64_t{0});
}

TEST_CASE("studio: every fault stops a fixture with an error")
{
    using namespace std::chrono_literals;

    const fault_kind faults[] = {
        fault_kind::parity,  fault_kind::framing,
        fault_kind::overrun, fault_kind::break_condition,
        fault_kind::hangup,  fault_kind::disconnect,
    };
    for (const fault_kind fault : faults)
    {
        studio app;
        const auto created = app.create_mock_port("device");
        REQUIRE(created);
        const auto& endpoint = *created;
        REQUIRE_EQ(endpoint->state(), endpoint_state::open);

        REQUIRE(app.inject_fault(endpoint->id(), fault));
        REQUIRE(wait_until(
            [&] { return endpoint->state() != endpoint_state::open; }, 2000ms));
    }
}

TEST_CASE("studio: faults are refused for other ports")
{
    studio app;
    const auto loopback =
        app.create_virtual_port("v", virtual_port_kind::loopback);
    REQUIRE(loopback);

    // Neither a fixture nor a driver port: no kind of fault applies, and the
    // port is left running.
    const fault_kind faults[] = {
        fault_kind::parity,  fault_kind::framing,
        fault_kind::overrun, fault_kind::break_condition,
        fault_kind::hangup,  fault_kind::disconnect,
    };
    for (const fault_kind fault : faults)
    {
        REQUIRE_EQ(app.inject_fault((*loopback)->id(), fault),
                   make_error_code(errc::unsupported));
        REQUIRE_EQ(app.inject_fault(endpoint_id{4242}, fault),
                   make_error_code(errc::not_found));
    }
    REQUIRE_EQ((*loopback)->state(), endpoint_state::open);

    const auto fixture = app.create_mock_port("device");
    REQUIRE(fixture);
    REQUIRE(app.remove_port((*fixture)->id()));
    REQUIRE_EQ(app.inject_fault((*fixture)->id(), fault_kind::parity),
               make_error_code(errc::not_found));
}

namespace
{

/// The bytes of every data event on `port` going `direction`, joined.
std::string data_on(const event_recorder& events, const std::string& port,
                    data_direction direction)
{
    std::string joined;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::data && event.name == port &&
            event.direction == direction && event.data)
            joined.append(event.data->begin(), event.data->end());
    }
    return joined;
}

} // namespace

TEST_CASE("studio: a fixture speaks and the hub carries it along its links")
{
    using namespace std::chrono_literals;

    studio app;
    const auto fixture = app.create_mock_port("device");
    const auto peer =
        app.create_virtual_port("peer", virtual_port_kind::loopback);
    REQUIRE(fixture);
    REQUIRE(peer);
    REQUIRE(app.connect("device", "peer"));

    event_recorder events;
    app.engine().subscribe({}, [events](const endpoint_event& e)
                           { events.record(e); });

    const std::string text = "hello";
    REQUIRE(app.inject_data((*fixture)->id(), as_bytes(text)));
    REQUIRE(wait_until(
        [&] { return data_on(events, "device", data_direction::rx) == text; },
        2000ms));
    // The hub wrote it to the other end of the link.
    REQUIRE(wait_until(
        [&] { return data_on(events, "peer", data_direction::tx) == text; },
        2000ms));
}

TEST_CASE("studio: data can only be injected into a fixture")
{
    studio app;
    const auto loopback =
        app.create_virtual_port("v", virtual_port_kind::loopback);
    REQUIRE(loopback);
    const std::string text = "x";
    REQUIRE_EQ(app.inject_data((*loopback)->id(), as_bytes(text)),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(app.inject_data(endpoint_id{4242}, as_bytes(text)),
               make_error_code(errc::not_found));
    REQUIRE_EQ(app.set_fixture_echo((*loopback)->id(), true),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(app.set_fixture_echo(endpoint_id{4242}, true),
               make_error_code(errc::not_found));
    REQUIRE(!app.fixture_echo((*loopback)->id()));
}

TEST_CASE("studio: an echoing fixture sends back what was written to it")
{
    using namespace std::chrono_literals;

    studio app;
    const auto fixture = app.create_mock_port("device");
    REQUIRE(fixture);
    const auto id = (*fixture)->id();

    event_recorder events;
    app.engine().subscribe({}, [events](const endpoint_event& e)
                           { events.record(e); });

    REQUIRE(!app.fixture_echo(id));
    REQUIRE(app.set_fixture_echo(id, true));
    REQUIRE(app.fixture_echo(id));
    // Asking again changes nothing.
    REQUIRE(app.set_fixture_echo(id, true));

    const std::string text = "ping";
    (*fixture)->write(as_bytes(text));
    REQUIRE(wait_until(
        [&] { return data_on(events, "device", data_direction::rx) == text; },
        2000ms));

    // Off: a later write is not answered.
    REQUIRE(app.set_fixture_echo(id, false));
    REQUIRE(!app.fixture_echo(id));
    (*fixture)->write(as_bytes(text));
    REQUIRE(wait_until(
        [&]
        { return data_on(events, "device", data_direction::tx).size() == 8; },
        2000ms));
    std::this_thread::sleep_for(150ms);
    REQUIRE_EQ(data_on(events, "device", data_direction::rx), text);

    // Removing the fixture takes the echo with it.
    REQUIRE(app.set_fixture_echo(id, true));
    REQUIRE(app.remove_port(id));
    REQUIRE(!app.fixture_echo(id));
}

TEST_CASE("studio: a fixture takes new settings and keeps its links and echo")
{
    using namespace std::chrono_literals;

    studio app;
    const auto fixture = app.create_mock_port("device");
    // A plain fixture: a loopback would send the echo straight back.
    const auto other = app.create_mock_port("tool");
    REQUIRE(fixture);
    REQUIRE(other);
    const auto id = (*fixture)->id();
    REQUIRE(app.engine().connect(id, (*other)->id()));
    REQUIRE(app.set_fixture_echo(id, true));

    serial_settings slow;
    slow.baud_rate = 9600;
    slow.data_bits = 7;
    slow.parity = parity_mode::even;
    REQUIRE(app.reconfigure_port(id, slow));
    REQUIRE(app.port_settings(id).has_value());
    REQUIRE(vss_test::require_value(app.port_settings(id)) == slow);
    REQUIRE(app.fixture_echo(id));
    REQUIRE_EQ(app.engine().links().size(), std::size_t{1});

    // The new settings are what a saved setup keeps.
    const auto saved = app.snapshot();
    const auto entry = std::find_if(saved.ports.begin(), saved.ports.end(),
                                    [](const config::port_entry& port)
                                    { return port.name == "device"; });
    REQUIRE(entry != saved.ports.end());
    REQUIRE_EQ(entry->serial.baud_rate, 9600u);

    // The echo still answers, from the new fixture.
    event_recorder events;
    app.engine().subscribe({}, [events](const endpoint_event& e)
                           { events.record(e); });
    const std::string text = "ping";
    (*fixture)->write(as_bytes(text));
    REQUIRE(wait_until(
        [&] { return data_on(events, "device", data_direction::rx) == text; },
        2000ms));
}

TEST_CASE(
    "studio: reconfigure saves a virtual port's values and refuses bad ones")
{
    studio app;
    const auto loop = app.create_virtual_port("v", virtual_port_kind::loopback);
    REQUIRE(loop);
    const auto id = (*loop)->id();

    serial_settings settings;
    settings.baud_rate = 19200;
    REQUIRE(app.reconfigure_port(id, settings));
    REQUIRE_EQ(vss_test::require_value(app.port_settings(id)).baud_rate, 19200u);

    serial_settings bad;
    bad.data_bits = 3;
    REQUIRE_EQ(app.reconfigure_port(id, bad),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(vss_test::require_value(app.port_settings(id)).baud_rate, 19200u);
    REQUIRE_EQ(app.reconfigure_port(endpoint_id{4242}, settings),
               make_error_code(errc::not_found));
    REQUIRE(!app.port_settings(endpoint_id{4242}).has_value());
}

TEST_CASE("studio: removal by id and by name")
{
    studio app;
    const auto created =
        app.create_virtual_port("v", virtual_port_kind::loopback);
    REQUIRE(created);

    REQUIRE(app.remove_port("v"));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
    REQUIRE_EQ(app.remove_port("v"), make_error_code(errc::not_found));

    const auto again =
        app.create_virtual_port("v", virtual_port_kind::loopback);
    REQUIRE(again);
    REQUIRE(app.remove_port((*again)->id()));
    REQUIRE_EQ(app.remove_port(endpoint_id{4242}),
               make_error_code(errc::not_found));
}

TEST_CASE("studio: topology helpers resolve names")
{
    studio app;
    REQUIRE(app.create_virtual_port("a", virtual_port_kind::loopback));
    REQUIRE(app.create_virtual_port("b", virtual_port_kind::loopback));
    REQUIRE(app.create_virtual_port("c", virtual_port_kind::loopback));

    const auto duplex = app.connect("a", "b");
    REQUIRE(duplex);

    const auto split = app.split("a", {"b", "c"});
    REQUIRE(!split);
    REQUIRE_EQ(split.error(), make_error_code(errc::cycle_detected));

    const auto one_way = app.redirect("b", "c");
    REQUIRE(one_way);

    const auto monitor = app.monitor("c", "a");
    REQUIRE(!monitor);

    REQUIRE_EQ(app.connect("a", "ghost"), make_error_code(errc::not_found));
    REQUIRE_EQ(app.split("ghost", {"a"}), make_error_code(errc::not_found));
    REQUIRE_EQ(app.join({"ghost"}, "a"), make_error_code(errc::not_found));
    REQUIRE_EQ(app.redirect("ghost", "a"), make_error_code(errc::not_found));
    REQUIRE_EQ(app.monitor("a", "ghost"), make_error_code(errc::not_found));

    REQUIRE(app.disconnect(*duplex));
    REQUIRE_EQ(app.disconnect(*duplex), make_error_code(errc::not_found));

    REQUIRE_EQ(app.link_infos().size(), std::size_t{1});
}

TEST_CASE("studio: documents apply and snapshot")
{
    config::document document;
    document.name = "unit";

    config::port_entry first;
    first.name = "left";
    first.kind = "virtual";
    first.virtual_kind = "loopback";
    document.ports.push_back(first);

    config::port_entry second;
    second.name = "right";
    second.kind = "virtual";
    second.virtual_kind = "loopback";
    document.ports.push_back(second);

    config::link_entry link;
    link.mode = "redirect";
    link.sources = {"left"};
    link.targets = {"right"};
    document.links.push_back(link);

    studio app;
    REQUIRE(app.apply(document));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{2});
    REQUIRE_EQ(app.link_infos().size(), std::size_t{1});

    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.ports.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.links.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.links[0].mode, std::string("redirect"));
    REQUIRE_EQ(snapshot.links[0].sources[0], std::string("left"));
    REQUIRE_EQ(snapshot.ports[0].virtual_kind, std::string("loopback"));
}

TEST_CASE("studio: a link's delay and jitter apply and snapshot, on a bus and "
          "a monitor")
{
    config::document document;
    document.name = "unit";
    for (const char* name : {"a", "b", "c", "tap"})
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        document.ports.push_back(port);
    }

    config::link_entry bus;
    bus.mode = "bus";
    bus.members = {"a", "b", "c"};
    bus.emulate_modem_lines = false;
    bus.delay_ms = 190;
    bus.jitter_ms = 10;
    document.links.push_back(bus);

    config::link_entry monitor;
    monitor.mode = "monitor";
    monitor.sources = {"a"};
    monitor.targets = {"tap"};
    monitor.emulate_modem_lines = false;
    monitor.delay_ms = 25;
    document.links.push_back(monitor);

    studio app;
    REQUIRE(app.apply(document));

    const auto infos = app.link_infos();
    REQUIRE_EQ(infos.size(), std::size_t{2});
    REQUIRE_EQ(infos[0].options.delay_ms, std::uint32_t{190});
    REQUIRE_EQ(infos[0].options.jitter_ms, std::uint32_t{10});
    REQUIRE_EQ(infos[1].options.delay_ms, std::uint32_t{25});

    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.links.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.links[0].delay_ms, std::uint32_t{190});
    REQUIRE_EQ(snapshot.links[0].jitter_ms, std::uint32_t{10});
    REQUIRE_EQ(snapshot.links[1].mode, std::string("monitor"));
    REQUIRE_EQ(snapshot.links[1].delay_ms, std::uint32_t{25});

    // Saved and loaded again, the delays survive.
    const auto text = config::to_json(snapshot);
    REQUIRE(text);
    const auto reloaded = config::from_json(*text);
    REQUIRE(reloaded);
    REQUIRE(reloaded->links == snapshot.links);

    // A bus made in code takes the same options.
    studio direct;
    config::document pair;
    pair.name = "x";
    pair.ports = {document.ports[0], document.ports[1]};
    REQUIRE(direct.apply(pair));
    link_options options;
    options.delay_ms = 40;
    const auto made = direct.bus({"a", "b"}, options);
    REQUIRE(made);
    REQUIRE_EQ(direct.link_infos()[0].options.delay_ms, std::uint32_t{40});
}

TEST_CASE("studio: a failed apply rolls back")
{
    config::document document;
    document.name = "broken";

    config::port_entry good;
    good.name = "good";
    good.kind = "virtual";
    good.virtual_kind = "loopback";
    document.ports.push_back(good);

    config::port_entry bad;
    bad.name = "bad";
    bad.kind = "physical";
    bad.uri = "serial://no-such-device-for-tests";
    document.ports.push_back(bad);

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(!applied);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});

    // Without rollback the good port stays.
    const auto retained = app.apply(document, false);
    REQUIRE(!retained);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{1});
}

TEST_CASE("studio: invalid documents are rejected before any work")
{
    studio app;
    config::document document;
    document.name = "invalid";
    config::port_entry port;
    port.name = "x";
    port.kind = "nonsense";
    document.ports.push_back(port);

    REQUIRE_EQ(app.apply(document), make_error_code(errc::config_error));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE("studio: bridge reports bad URIs")
{
    studio app;
    const auto bridged = app.bridge("serial://", "tcp://127.0.0.1:1");
    REQUIRE(!bridged);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE("studio: share_serial validates its listener count")
{
    studio app;
    REQUIRE_EQ(app.share_serial("serial://COM99", 0),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(app.share_serial("serial://COM99", 65),
               make_error_code(errc::invalid_argument));
}

TEST_CASE("studio: environment queries are safe")
{
    studio app;
    const auto devices = app.serial_devices();
    for (const auto& device : devices)
        REQUIRE(!device.path.empty());

    const auto installation = app.detect_com0com();
    if (installation.found)
        REQUIRE(!installation.setupc_path.empty());
}

TEST_CASE("studio: join_ports reports a bad source")
{
    studio app;
    REQUIRE(!app.join_ports({"serial://no-such-source"},
                            "serial://no-such-target"));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});

    REQUIRE(!app.join_ports({}, "serial://no-such-target"));
}

TEST_CASE("studio: unknown identifiers are handled")
{
    studio app;
    REQUIRE_EQ(app.advertised_path(4242), std::string());
    REQUIRE(app.find("nothing") == nullptr);
    REQUIRE_EQ(app.remove_port(4242), make_error_code(errc::not_found));

    const auto options = app.default_options();
    REQUIRE(options.capture_traffic);
    REQUIRE(app.options().start_hub);
}

TEST_CASE("studio: applying a mock and an automatic virtual port")
{
    config::document document;
    document.name = "mixed";

    config::port_entry mock;
    mock.name = "fake";
    mock.kind = "mock";
    mock.mock_target = "modem";
    document.ports.push_back(mock);

    config::port_entry automatic;
    automatic.name = "auto";
    automatic.kind = "virtual";
    automatic.virtual_kind = "auto";
    document.ports.push_back(automatic);

    config::link_entry link;
    link.mode = "duplex";
    link.sources = {"fake"};
    link.targets = {"auto"};
    link.emulate_modem_lines = false;
    document.links.push_back(link);

    studio app;
    REQUIRE(app.apply(document));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{2});
    REQUIRE_EQ(app.link_infos().size(), std::size_t{1});

    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.links[0].mode, std::string("duplex"));
    REQUIRE(!snapshot.links[0].emulate_modem_lines);
}

TEST_CASE("studio: apply understands every link shape")
{
    const auto make_port = [](const std::string& name)
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        return port;
    };

    {
        config::document document;
        document.name = "split";
        document.ports = {make_port("a"), make_port("b"), make_port("c")};
        config::link_entry link;
        link.mode = "split";
        link.sources = {"a"};
        link.targets = {"b", "c"};
        document.links.push_back(link);

        studio app;
        REQUIRE(app.apply(document));
        const auto snapshot = app.snapshot();
        REQUIRE_EQ(snapshot.links.size(), std::size_t{1});
        REQUIRE_EQ(snapshot.links[0].mode, std::string("split"));
    }

    {
        config::document document;
        document.name = "join";
        document.ports = {make_port("a"), make_port("b"), make_port("c")};
        config::link_entry link;
        link.mode = "join";
        link.sources = {"a", "b"};
        link.targets = {"c"};
        document.links.push_back(link);

        studio app;
        REQUIRE(app.apply(document));
        REQUIRE_EQ(app.snapshot().links[0].mode, std::string("join"));
    }

    {
        config::document document;
        document.name = "monitor";
        document.ports = {make_port("a"), make_port("b")};
        config::link_entry link;
        link.mode = "monitor";
        link.sources = {"a"};
        link.targets = {"b"};
        document.links.push_back(link);

        studio app;
        REQUIRE(app.apply(document));
        const auto snapshot = app.snapshot();
        REQUIRE_EQ(snapshot.links[0].mode, std::string("monitor"));
        REQUIRE(!snapshot.links[0].emulate_modem_lines);
    }
}

TEST_CASE("studio: apply reports unsupported port kinds")
{
    config::document document;
    document.name = "named-pipe";
    config::port_entry port;
    port.name = "pipe";
    port.kind = "virtual";
    port.virtual_kind = "named-pipe";
    document.ports.push_back(port);

    studio app;
    const auto applied = app.apply(document);
    // Named pipes are not implemented on any platform yet.
    REQUIRE(!applied);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE("studio: a driver topology fails cleanly without the driver")
{
    if (vss::driver_available())
        VSS_SKIP("the COM port driver is installed on this machine");

    config::document document;
    document.name = "driver-port";
    config::port_entry port;
    port.name = "com";
    port.kind = "virtual";
    port.virtual_kind = "windows-driver";
    document.ports.push_back(port);

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(!applied);
    REQUIRE_EQ(applied.error(), make_error_code(errc::unsupported));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE("studio: virtual port kinds round trip through the snapshot")
{
    studio app;
    const auto created =
        app.create_virtual_port("best", virtual_port_kind::pty);
    REQUIRE(created);

    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.ports.size(), std::size_t{1});
    if (supports(virtual_port_kind::pty))
    {
        REQUIRE_EQ(snapshot.ports[0].virtual_kind, std::string("pty"));
    }
    else if (snapshot.ports[0].virtual_kind == "auto")
    {
        // No pseudo terminals here: the request falls back to auto.
        REQUIRE_EQ(app.advertised_path((*created)->id()).find("loopback"),
                   std::string::npos);
    }
}

TEST_CASE("studio: removing a port releases its advertised path")
{
    studio app;
    REQUIRE(app.create_virtual_port("first", virtual_port_kind::loopback));
    REQUIRE(app.remove_port("first"));

    const auto second =
        app.create_virtual_port("second", virtual_port_kind::loopback);
    REQUIRE(second);
    REQUIRE_EQ(app.advertised_path((*second)->id()),
               std::string("loopback:second"));
}

TEST_CASE("studio: a cyclic document rolls back every port it created")
{
    config::document document;
    document.name = "cycle";

    for (const char* name : {"a", "b"})
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        document.ports.push_back(port);
    }

    config::link_entry forward;
    forward.mode = "duplex";
    forward.sources = {"a"};
    forward.targets = {"b"};
    document.links.push_back(forward);

    config::link_entry backward = forward;
    backward.sources = {"b"};
    backward.targets = {"a"};
    document.links.push_back(backward);

    // The document validates (each link is well formed) but the second link
    // closes a cycle, so the hub refuses it and the ports must be removed.
    REQUIRE(!validate(document));

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(!applied);
    REQUIRE_EQ(applied.error(), make_error_code(errc::cycle_detected));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
    REQUIRE_EQ(app.link_infos().size(), std::size_t{0});
}

TEST_CASE("studio: share_serial reports an unattachable port")
{
    studio app;
    const auto shared = app.share_serial("serial://no-such-port-for-tests", 2);
    REQUIRE(!shared);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE("studio: endpoints added directly are still described")
{
    studio app;
    const auto added = app.engine().add_endpoint(
        "direct", endpoint_kind::virtual_port,
        loopback_transport::create(app.engine().executor(), "direct"));
    REQUIRE(added);

    const auto infos = app.endpoint_infos();
    REQUIRE_EQ(infos.size(), std::size_t{1});
    REQUIRE_EQ(infos[0].name, std::string("direct"));
    REQUIRE_EQ(infos[0].path, std::string("loopback:direct"));
    REQUIRE(app.snapshot().ports.empty());
}

TEST_CASE("studio: start and stop")
{
    studio_options options;
    options.start_hub = false;
    studio app(options);

    REQUIRE(!app.engine().running());
    app.start();
    REQUIRE(app.engine().running());
    app.stop();
    REQUIRE(!app.engine().running());
}

TEST_CASE("studio: a bus is applied, carried and saved again")
{
    const auto make_port = [](const std::string& name)
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        return port;
    };

    config::document document;
    document.name = "bus";
    document.ports = {make_port("master"), make_port("one"), make_port("two"),
                      make_port("tap")};

    config::link_entry bus;
    bus.mode = "bus";
    bus.members = {"master", "one", "two"};
    bus.emulate_modem_lines = false;
    document.links.push_back(bus);

    config::link_entry tap;
    tap.mode = "join";
    tap.sources = {"master", "one", "two"};
    tap.targets = {"tap"};
    tap.emulate_modem_lines = false;
    document.links.push_back(tap);

    studio app;
    REQUIRE(app.apply(document));

    const auto links = app.link_infos();
    REQUIRE_EQ(links.size(), std::size_t{2});
    REQUIRE_EQ(links[0].mode, link_mode::bus);
    REQUIRE_EQ(links[0].sources.size(), std::size_t{3});

    // What the snapshot saves is what was loaded.
    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.links.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.links[0].mode, std::string("bus"));
    REQUIRE((snapshot.links[0].members ==
             std::vector<std::string>{"master", "one", "two"}));
    REQUIRE(snapshot.links[0].sources.empty());
    REQUIRE(snapshot.links[0].targets.empty());
    REQUIRE(!config::validate(snapshot));

    // A bus made through the facade is the same thing.
    studio again;
    REQUIRE(again.apply(snapshot));
    REQUIRE_EQ(again.link_infos().front().mode, link_mode::bus);
}

TEST_CASE("studio: bus resolves names and reports what the hub refuses")
{
    studio app;
    REQUIRE(app.create_virtual_port("a", virtual_port_kind::loopback));
    REQUIRE(app.create_virtual_port("b", virtual_port_kind::loopback));
    REQUIRE(app.create_virtual_port("c", virtual_port_kind::loopback));

    REQUIRE_EQ(app.bus({"a", "missing"}).error(),
               make_error_code(errc::not_found));
    REQUIRE_EQ(app.bus({"a"}).error(), make_error_code(errc::invalid_argument));
    REQUIRE(app.bus({"a", "b"}));
    REQUIRE_EQ(app.bus({"b", "c"}).error(),
               make_error_code(errc::already_exists));

    REQUIRE(app.redirect("b", "c"));
    REQUIRE_EQ(app.redirect("c", "a").error(),
               make_error_code(errc::cycle_detected));
}

TEST_CASE("studio: a document that loops through a bus rolls back")
{
    config::document document;
    document.name = "loop";
    for (const char* name : {"a", "b", "c"})
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        document.ports.push_back(port);
    }

    config::link_entry bus;
    bus.mode = "bus";
    bus.members = {"a", "b"};
    bus.emulate_modem_lines = false;
    document.links.push_back(bus);

    config::link_entry out;
    out.mode = "redirect";
    out.sources = {"b"};
    out.targets = {"c"};
    document.links.push_back(out);

    config::link_entry back;
    back.mode = "redirect";
    back.sources = {"c"};
    back.targets = {"a"};
    document.links.push_back(back);

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(!applied);
    REQUIRE_EQ(applied.error(), make_error_code(errc::cycle_detected));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE(
    "studio: a COM number is for the driver and is ignored by other ports")
{
    studio app;
    const auto created =
        app.create_virtual_port("plain", virtual_port_kind::loopback, {}, 9);
    REQUIRE(created);

    // Not a driver port: nothing to renumber, and nothing recorded.
    const auto changed = app.set_com_number((*created)->id(), 12);
    REQUIRE(!changed.has_value());
    REQUIRE_EQ(changed.error(), make_error_code(errc::unsupported));
    REQUIRE_EQ(app.set_com_number(9999, 12).error(),
               make_error_code(errc::not_found));
    REQUIRE_EQ(app.snapshot().ports[0].com_number, 0u);
}

TEST_CASE("studio: a topology that asks for a COM number fails cleanly without "
          "the driver")
{
    if (vss::driver_available())
        VSS_SKIP("the COM port driver is installed on this machine");

    config::document document;
    document.name = "numbered";
    config::port_entry port;
    port.name = "com";
    port.kind = "virtual";
    port.virtual_kind = "windows-driver";
    port.com_number = 7;
    document.ports.push_back(port);

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(!applied);
    REQUIRE_EQ(applied.error(), make_error_code(errc::unsupported));
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}

TEST_CASE(
    "studio: an automatic port takes a COM number only where the driver is")
{
    if (vss::driver_available())
        VSS_SKIP("the COM port driver is installed on this machine");

    // Without the driver a COM number means nothing: the port is whatever the
    // platform gives, and the number is not saved.
    config::document document;
    document.name = "auto-number";
    config::port_entry port;
    port.name = "any";
    port.kind = "virtual";
    port.virtual_kind = "auto";
    port.com_number = 7;
    document.ports.push_back(port);

    studio app;
    const auto applied = app.apply(document);
    REQUIRE(applied.has_value());
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{1});
    REQUIRE_EQ(app.snapshot().ports[0].com_number, 0u);
}

TEST_CASE("studio: a port that is not a driver port has no holder to release")
{
    studio app;
    const auto created =
        app.create_virtual_port("plain", virtual_port_kind::loopback, {}, 0);
    REQUIRE(created);

    REQUIRE(app.driver_ports().empty());
    REQUIRE(!app.driver_holder((*created)->id()).has_value());
    REQUIRE(!app.driver_holder(9999).has_value());

    const auto revoked = app.revoke_port((*created)->id());
    REQUIRE(!revoked.has_value());
    REQUIRE_EQ(revoked.error(), make_error_code(errc::unsupported));
    REQUIRE_EQ(app.revoke_port(9999).error(), make_error_code(errc::not_found));
}

TEST_CASE("studio: a half-duplex bus is applied and saved again")
{
    const auto make_port = [](const std::string& name)
    {
        config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        return port;
    };

    config::document document;
    document.name = "line";
    document.ports = {make_port("master"), make_port("one"), make_port("two")};

    config::link_entry bus;
    bus.mode = "bus";
    bus.members = {"master", "one", "two"};
    bus.emulate_modem_lines = false;
    bus.delay_ms = 4;
    bus.half_duplex.enabled = true;
    bus.half_duplex.bit_rate = 19200;
    bus.half_duplex.turnaround_ms = 3;
    bus.half_duplex.on_collision = collision_action::garble;
    bus.half_duplex.rts_gating = true;
    document.links.push_back(bus);

    studio app;
    REQUIRE(app.apply(document));

    const auto links = app.link_infos();
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links[0].mode, link_mode::bus);
    REQUIRE(links[0].options.half_duplex == bus.half_duplex);
    REQUIRE_EQ(links[0].options.delay_ms, std::uint32_t{4});
    REQUIRE_EQ(links[0].stats.collisions, std::uint64_t{0});
    REQUIRE_EQ(links[0].stats.garbled, std::uint64_t{0});

    // What the snapshot saves is what was loaded, so the option survives
    // save and open.
    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.links.size(), std::size_t{1});
    REQUIRE(snapshot.links[0] == bus);
    REQUIRE(!config::validate(snapshot));

    studio again;
    REQUIRE(again.apply(snapshot));
    REQUIRE(again.link_infos().front().options.half_duplex == bus.half_duplex);

    // The hub follows the RTS of a loopback port, which is what RTS gating
    // goes by; it is low until someone raises it.
    const auto master = app.engine().find_endpoint("master");
    REQUIRE(master != nullptr);
    REQUIRE(master->peer_rts() == std::optional<bool>(false));
    REQUIRE(!master->set_line(control_line::rts, true));
    REQUIRE(master->peer_rts() == std::optional<bool>(true));
}

TEST_CASE("studio: a bus made through the facade takes a half-duplex line")
{
    studio app;
    REQUIRE(app.create_virtual_port("a", virtual_port_kind::loopback));
    REQUIRE(app.create_virtual_port("b", virtual_port_kind::loopback));

    link_options options;
    options.half_duplex.enabled = true;
    options.half_duplex.bit_rate = min_bus_bit_rate - 1;
    const auto refused = app.bus({"a", "b"}, options);
    REQUIRE(!refused);
    REQUIRE(refused.error() == make_error_code(errc::invalid_argument));

    options.half_duplex.bit_rate = 2400;
    const auto made = app.bus({"a", "b"}, options);
    REQUIRE(made);
    const auto snapshot = app.snapshot();
    REQUIRE_EQ(snapshot.links.size(), std::size_t{1});
    REQUIRE(snapshot.links[0].half_duplex == options.half_duplex);
}
