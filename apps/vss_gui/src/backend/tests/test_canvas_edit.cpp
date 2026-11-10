//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/canvas_edit.hpp"
#include "backend/driver_error.hpp"
#include "backend/fake_studio_backend.hpp"

#include "test_framework.hpp"

#include <vss/core/error.hpp>

#include <QString>

namespace
{

using namespace vss::gui;

std::optional<port_view> named(const StudioBackend& backend,
                               const QString& name)
{
    for (const auto& port : backend.ports())
    {
        if (port.name == name)
            return port;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("canvas edit: a pair of two fixtures is made in one step")
{
    FakeStudioBackend backend;
    bundle_request request = default_request(canvas_item::pair, {});
    REQUIRE_EQ(request.slots.size(), 2);
    request.slots[0] = {slot_kind::new_fixture, QStringLiteral("rx-sim")};
    request.slots[1] = {slot_kind::new_fixture, QStringLiteral("tx-sim")};

    const auto made = build_bundle(backend, request);
    REQUIRE(made.has_value());
    REQUIRE_EQ(made->created.size(), 2);
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(vss_test::require_value(named(backend, QStringLiteral("rx-sim"))).kind ==
            vss::endpoint_kind::mock);
    REQUIRE(backend.links().front().shape == link_shape::two_way);
}

TEST_CASE("canvas edit: default requests name every slot freshly")
{
    FakeStudioBackend backend;
    REQUIRE(backend.create_auto_port(QStringLiteral("port-1"), true));
    const auto request = default_request(canvas_item::split, backend.ports());
    REQUIRE_EQ(request.slots.size(), 3);
    REQUIRE_EQ(request.slots[0].name, QStringLiteral("port-2"));
    REQUIRE_EQ(request.slots[2].name, QStringLiteral("port-4"));

    const auto bench = default_request(canvas_item::bench, {}, 3);
    REQUIRE_EQ(bench.slots.size(), 6);
    REQUIRE(bench.slots[0].kind == slot_kind::new_fixture);
    REQUIRE(bench.slots[1].kind == slot_kind::new_port);
    REQUIRE_EQ(slot_count(canvas_item::pair, 7), 2);
    REQUIRE_EQ(slot_count(canvas_item::join, 9), 9);
}

TEST_CASE("canvas edit: a failed link takes the new ports away again")
{
    FakeStudioBackend backend;
    REQUIRE(backend.create_auto_port(QStringLiteral("kept"), true));
    bundle_request request =
        default_request(canvas_item::split, backend.ports());
    request.slots[1] = {slot_kind::existing, QStringLiteral("kept")};
    // The first new port is made, then the last slot names no port.
    bundle_request broken = request;
    broken.slots[2] = {slot_kind::existing, QStringLiteral("missing")};
    const auto failed = build_bundle(backend, broken);
    REQUIRE(!failed.has_value());
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});
    REQUIRE(backend.links().empty());

    const auto made = build_bundle(backend, request);
    REQUIRE(made.has_value());
    REQUIRE_EQ(made->created.size(), 2);
    REQUIRE(backend.links().front().shape == link_shape::split);
}

TEST_CASE("canvas edit: a refused driver port names the missing part")
{
    // The Advanced canvas used to show the bare "permission denied".
    FakeStudioBackend backend;
    setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.driver.problem = driver_problem::helper_stopped;
    backend.set_facts(facts);

    const bundle_request request =
        default_request(canvas_item::port, backend.ports());
    backend.fail_next(vss::make_error_code(vss::errc::permission_denied));
    const auto failed = build_bundle(backend, request);
    REQUIRE(!failed.has_value());
    const auto found = driver_problem_of(failed.error());
    REQUIRE(found.has_value());
    REQUIRE(*found == driver_problem::helper_stopped);
    REQUIRE(backend.ports().empty());

    // A fixture needs no driver, so a refusal of that is left as it was.
    bundle_request fixture = default_request(canvas_item::fixture, {});
    backend.fail_next(vss::make_error_code(vss::errc::permission_denied));
    const auto other = build_bundle(backend, fixture);
    REQUIRE(!other.has_value());
    REQUIRE(!driver_problem_of(other.error()).has_value());
}

TEST_CASE("canvas edit: a switch gives the first program the line")
{
    FakeStudioBackend backend;
    const auto made =
        build_bundle(backend, default_request(canvas_item::switch_link, {}));
    REQUIRE(made.has_value());
    const auto link = backend.links().front();
    REQUIRE(link.options.policy == vss::merge_policy::owner);
    REQUIRE(link.shape == link_shape::join);
    REQUIRE_EQ(link.owner, link.sources.first());
}

TEST_CASE("canvas edit: a bus needs the shared bus capability")
{
    FakeStudioBackend backend;
    capabilities caps = backend.caps();
    caps.shared_bus = false;
    backend.set_capabilities(caps);
    REQUIRE(!item_available(caps, canvas_item::bus));
    REQUIRE(!build_bundle(backend, default_request(canvas_item::bus, {})));
    REQUIRE(backend.ports().empty());
}

TEST_CASE("canvas edit: duplicate copies the ports and the link between them")
{
    FakeStudioBackend backend;
    bundle_request request = default_request(canvas_item::pair, {});
    request.slots[0] = {slot_kind::new_fixture, QStringLiteral("rx-sim")};
    request.slots[1] = {slot_kind::new_fixture, QStringLiteral("fixture-1")};
    REQUIRE(build_bundle(backend, request));

    QVector<vss::endpoint_id> ids;
    for (const auto& port : backend.ports())
        ids.append(port.id);
    const auto copied = duplicate_ports(backend, ids);
    REQUIRE(copied.has_value());
    REQUIRE(copied->created.contains(QStringLiteral("rx-sim-copy")));
    REQUIRE(copied->created.contains(QStringLiteral("fixture-2")));
    REQUIRE_EQ(backend.ports().size(), std::size_t{4});
    REQUIRE_EQ(backend.links().size(), std::size_t{2});
    REQUIRE(vss_test::require_value(named(backend, QStringLiteral("fixture-2"))).kind ==
            vss::endpoint_kind::mock);
}

TEST_CASE("canvas edit: a removal is made again by name")
{
    FakeStudioBackend backend;
    REQUIRE(build_bundle(backend, default_request(canvas_item::pair, {})));
    const auto first = backend.ports().front().id;
    const QString name = backend.ports().front().name;

    const removal what = describe_removal(backend, {first}, {});
    REQUIRE_EQ(what.ports.size(), 1);
    REQUIRE_EQ(what.links.size(), 1);
    REQUIRE(apply_removal(backend, what));
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});
    REQUIRE(backend.links().empty());

    REQUIRE(restore_removal(backend, what));
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(named(backend, name).has_value());
}

TEST_CASE("canvas edit: new link options change the link in place")
{
    FakeStudioBackend backend;
    REQUIRE(build_bundle(backend, default_request(canvas_item::pair, {})));
    const auto before = backend.links().front();
    vss::link_options options = before.options;
    options.delay_ms = 20;
    options.jitter_ms = 5;
    REQUIRE(change_link_options(backend, before.id, options).has_value());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    // The same link, not one made again.
    REQUIRE_EQ(backend.links().front().id, before.id);
    REQUIRE_EQ(backend.links().front().options.delay_ms, 20u);
    REQUIRE_EQ(backend.links().front().options.jitter_ms, 5u);
    REQUIRE(backend.links().front().sources == before.sources);
    REQUIRE(backend.links().front().targets == before.targets);
}

TEST_CASE("canvas edit: refused link options change nothing")
{
    FakeStudioBackend backend;
    REQUIRE(build_bundle(backend, default_request(canvas_item::pair, {})));
    const auto before = backend.links().front();
    vss::link_options options = before.options;
    options.delay_ms = 5;
    options.jitter_ms = 6;
    REQUIRE(!change_link_options(backend, before.id, options).has_value());
    REQUIRE_EQ(backend.links().front().options.delay_ms, 0u);
    REQUIRE(!change_link_options(backend, before.id + 100, {}).has_value());
}

TEST_CASE("canvas edit: a wire makes the link it names")
{
    FakeStudioBackend backend;
    const auto a = backend.create_mock_port(QStringLiteral("a"), {},
                                            QStringLiteral("scripted"));
    const auto b = backend.create_mock_port(QStringLiteral("b"), {},
                                            QStringLiteral("scripted"));
    REQUIRE(a && b);
    REQUIRE(make_wire(backend, *a, *b, wire_kind::one_way));
    REQUIRE(backend.links().front().shape == link_shape::one_way);
}

TEST_CASE("canvas edit: port settings change on a fixture")
{
    FakeStudioBackend backend;
    const auto a = backend.create_mock_port(QStringLiteral("a"), {},
                                            QStringLiteral("scripted"));
    REQUIRE(a);
    vss::serial_settings settings;
    settings.baud_rate = 9600;
    settings.data_bits = 7;
    settings.parity = vss::parity_mode::even;
    REQUIRE(backend.set_port_settings(*a, settings));
    REQUIRE(vss_test::require_value(backend.port(*a)).serial == settings);
    settings.data_bits = 2;
    REQUIRE(!backend.set_port_settings(*a, settings));
}
