//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/fake_studio_backend.hpp"

#include "test_framework.hpp"

#include <vss/core/error.hpp>
#include <vss/core/link.hpp>
#include <vss/core/types.hpp>
#include <vss/ports/virtual_port.hpp>

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <vector>

TEST_CASE("fake backend: creates ports and suffixes duplicate names")
{
    vss::gui::FakeStudioBackend backend;
    int changes = 0;
    QObject::connect(&backend, &vss::gui::StudioBackend::ports_changed,
                     [&changes] { ++changes; });

    const auto first = backend.create_virtual_port(
        QStringLiteral("port-1"), vss::virtual_port_kind::loopback, true);
    REQUIRE(first.has_value());

    const auto second = backend.create_virtual_port(
        QStringLiteral("port-1"), vss::virtual_port_kind::loopback, true);
    REQUIRE(second.has_value());

    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE_EQ(backend.ports()[0].name, QStringLiteral("port-1"));
    REQUIRE_EQ(backend.ports()[1].name, QStringLiteral("port-1-2"));
    REQUIRE(backend.ports()[0].studio_only);
    REQUIRE(backend.ports()[0].open);
    REQUIRE_EQ(backend.ports()[0].peer, vss::gui::peer_state::unknown);
    REQUIRE(changes >= 2);

    const auto removed = backend.remove_port(*first);
    REQUIRE(removed.has_value());
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});
}

TEST_CASE("fake backend: links, owner, lines and send")
{
    vss::gui::FakeStudioBackend backend;
    const auto a = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    const auto b = backend.create_virtual_port(
        QStringLiteral("b"), vss::virtual_port_kind::loopback, true);
    const auto c = backend.create_virtual_port(
        QStringLiteral("c"), vss::virtual_port_kind::loopback, true);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());

    const auto duplex = backend.connect_ports(*a, *b, vss::link_options{});
    REQUIRE(duplex.has_value());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links()[0].shape, vss::gui::link_shape::two_way);

    const auto split = backend.split(*a, {*b, *c}, vss::link_options{});
    REQUIRE(split.has_value());
    REQUIRE_EQ(backend.links()[1].shape, vss::gui::link_shape::split);

    const auto joined = backend.join({*b, *c}, *a, vss::link_options{});
    REQUIRE(joined.has_value());
    REQUIRE_EQ(backend.links()[2].shape, vss::gui::link_shape::join);

    const auto one_way = backend.redirect(*a, *b, vss::link_options{});
    REQUIRE(one_way.has_value());
    REQUIRE_EQ(backend.links()[3].shape, vss::gui::link_shape::one_way);

    const auto monitor = backend.monitor(*a, *c);
    REQUIRE(monitor.has_value());
    REQUIRE_EQ(backend.links()[4].shape, vss::gui::link_shape::monitor);

    const auto owned = backend.set_owner(*joined, *b);
    REQUIRE(owned.has_value());
    REQUIRE_EQ(backend.links()[2].owner, *b);
    REQUIRE(!backend.set_owner(*joined, *a).has_value());
    REQUIRE(!backend.set_owner(*joined, 999).has_value());
    REQUIRE(backend.release_owner(*joined).has_value());
    REQUIRE_EQ(backend.links()[2].owner, vss::endpoint_id{0});

    REQUIRE(backend.send(*a, QByteArray("hello")).has_value());
    REQUIRE_EQ(backend.sent().size(), std::size_t{1});
    REQUIRE_EQ(backend.sent()[0], QByteArray("hello"));
    REQUIRE(backend.send(999, QByteArrayLiteral("x")).error() ==
            vss::make_error_code(vss::errc::not_found));

    REQUIRE(backend.set_line(*a, vss::control_line::dtr, true).has_value());
    REQUIRE(vss_test::require_value(backend.port(*a)).lines.dtr());
}

TEST_CASE("fake backend: the owner of a duplex link is a target")
{
    vss::gui::FakeStudioBackend backend;
    const auto a = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    const auto b = backend.create_virtual_port(
        QStringLiteral("b"), vss::virtual_port_kind::loopback, true);
    const auto c = backend.create_virtual_port(
        QStringLiteral("c"), vss::virtual_port_kind::loopback, true);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());

    // Duplex: the routes that meet at one endpoint come from the targets.
    const auto split = backend.split(*a, {*b, *c}, vss::link_options{});
    REQUIRE(split.has_value());
    REQUIRE(backend.set_owner(*split, *b).has_value());
    REQUIRE_EQ(backend.links().front().owner, *b);
    REQUIRE(backend.set_owner(*split, *a).error() ==
            vss::make_error_code(vss::errc::invalid_argument));
    REQUIRE_EQ(backend.links().front().owner, *b);
    REQUIRE(backend.set_owner(*split, 999).error() ==
            vss::make_error_code(vss::errc::invalid_argument));

    // One-way: the owner is one of the sources.
    const auto joined = backend.join({*b, *c}, *a, vss::link_options{});
    REQUIRE(joined.has_value());
    REQUIRE(backend.set_owner(*joined, *c).has_value());
    REQUIRE(backend.set_owner(*joined, *a).error() ==
            vss::make_error_code(vss::errc::invalid_argument));

    // A bus has no owner.
    const auto d = backend.create_virtual_port(
        QStringLiteral("d"), vss::virtual_port_kind::loopback, true);
    const auto e = backend.create_virtual_port(
        QStringLiteral("e"), vss::virtual_port_kind::loopback, true);
    REQUIRE(d.has_value());
    REQUIRE(e.has_value());
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
    const auto bus = backend.bus({*d, *e});
    REQUIRE(bus.has_value());
    REQUIRE(backend.set_owner(*bus, *d).error() ==
            vss::make_error_code(vss::errc::unsupported));
}

TEST_CASE("fake backend: add_target and remove_target edit a duplex link")
{
    vss::gui::FakeStudioBackend backend;
    const auto a = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    const auto b = backend.create_virtual_port(
        QStringLiteral("b"), vss::virtual_port_kind::loopback, true);
    const auto c = backend.create_virtual_port(
        QStringLiteral("c"), vss::virtual_port_kind::loopback, true);
    const auto d = backend.create_virtual_port(
        QStringLiteral("d"), vss::virtual_port_kind::loopback, true);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(d.has_value());
    const auto not_found = vss::make_error_code(vss::errc::not_found);
    const auto unsupported = vss::make_error_code(vss::errc::unsupported);
    const auto invalid = vss::make_error_code(vss::errc::invalid_argument);
    const auto exists = vss::make_error_code(vss::errc::already_exists);

    const auto pair = backend.connect_ports(*a, *b, vss::link_options{});
    REQUIRE(pair.has_value());
    REQUIRE_EQ(backend.links().front().shape, vss::gui::link_shape::two_way);

    // Adding a second target makes the pair a split and keeps the link.
    REQUIRE(backend.add_target(*pair, *c).has_value());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, *pair);
    REQUIRE_EQ(backend.links().front().shape, vss::gui::link_shape::split);
    REQUIRE_EQ(backend.links().front().sources.size(), 1);
    REQUIRE_EQ(backend.links().front().targets.size(), 2);

    // Every error code, and nothing changed by any of them.
    REQUIRE(backend.add_target(999, *d).error() == not_found);
    REQUIRE(backend.add_target(*pair, 999).error() == not_found);
    REQUIRE(backend.add_target(*pair, *a).error() == invalid);
    REQUIRE(backend.add_target(*pair, *c).error() == exists);
    REQUIRE(backend.remove_target(999, *c).error() == not_found);
    REQUIRE(backend.remove_target(*pair, 999).error() == not_found);
    REQUIRE(backend.remove_target(*pair, *d).error() == not_found);
    REQUIRE(backend.remove_target(*pair, *a).error() == invalid);
    REQUIRE_EQ(backend.links().front().targets.size(), 2);

    // The injected error comes first.
    backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(backend.add_target(*pair, *d).error() ==
            vss::make_error_code(vss::errc::io_error));
    backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(backend.remove_target(*pair, *c).error() ==
            vss::make_error_code(vss::errc::io_error));
    REQUIRE_EQ(backend.links().front().targets.size(), 2);

    // Removing the owner releases it; removing another target keeps it.
    REQUIRE(backend.set_owner(*pair, *c).has_value());
    REQUIRE(backend.add_target(*pair, *d).has_value());
    REQUIRE(backend.remove_target(*pair, *d).has_value());
    REQUIRE_EQ(backend.links().front().owner, *c);
    REQUIRE(backend.remove_target(*pair, *c).has_value());
    REQUIRE_EQ(backend.links().front().owner, vss::endpoint_id{0});
    REQUIRE_EQ(backend.links().front().id, *pair);

    // The last target stays.
    REQUIRE_EQ(backend.links().front().targets.size(), 1);
    REQUIRE(backend.remove_target(*pair, *b).error() == invalid);
    REQUIRE_EQ(backend.links().front().targets.size(), 1);

    // One-way links and buses are not edited in place.
    const auto joined = backend.join({*b, *c}, *d, vss::link_options{});
    REQUIRE(joined.has_value());
    REQUIRE(backend.add_target(*joined, *a).error() == unsupported);
    REQUIRE(backend.remove_target(*joined, *d).error() == unsupported);
    const auto redirected = backend.redirect(*a, *d, vss::link_options{});
    REQUIRE(redirected.has_value());
    REQUIRE(backend.add_target(*redirected, *c).error() == unsupported);
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
    const auto bus = backend.bus({*b, *c});
    REQUIRE(bus.has_value());
    REQUIRE(backend.add_target(*bus, *a).error() == unsupported);
    REQUIRE(backend.remove_target(*bus, *b).error() == unsupported);
}

TEST_CASE("fake backend: add_source and remove_source edit a one-way link")
{
    vss::gui::FakeStudioBackend backend;
    std::vector<vss::endpoint_id> ids;
    for (const char* name : {"a", "b", "c", "d", "e"})
    {
        const auto port = backend.create_virtual_port(
            QString::fromLatin1(name), vss::virtual_port_kind::loopback, true);
        REQUIRE(port.has_value());
        ids.push_back(*port);
    }
    const vss::endpoint_id a = ids[0];
    const vss::endpoint_id b = ids[1];
    const vss::endpoint_id c = ids[2];
    const vss::endpoint_id d = ids[3];
    const vss::endpoint_id e = ids[4];
    const auto not_found = vss::make_error_code(vss::errc::not_found);
    const auto unsupported = vss::make_error_code(vss::errc::unsupported);
    const auto invalid = vss::make_error_code(vss::errc::invalid_argument);
    const auto exists = vss::make_error_code(vss::errc::already_exists);

    const auto joined = backend.join({a, b}, c, vss::link_options{});
    REQUIRE(joined.has_value());

    // A source is added at the end, and the link is the same link.
    REQUIRE(backend.add_source(*joined, d).has_value());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, *joined);
    REQUIRE_EQ(backend.links().front().shape, vss::gui::link_shape::join);
    REQUIRE_EQ(backend.links().front().sources.size(), 3);
    REQUIRE_EQ(backend.links().front().sources.back(), d);
    REQUIRE_EQ(backend.links().front().targets.size(), 1);

    // Every error code, and nothing changed by any of them.
    REQUIRE(backend.add_source(999, e).error() == not_found);
    REQUIRE(backend.add_source(*joined, 999).error() == not_found);
    REQUIRE(backend.add_source(*joined, c).error() == invalid);
    REQUIRE(backend.add_source(*joined, a).error() == exists);
    REQUIRE(backend.remove_source(999, a).error() == not_found);
    REQUIRE(backend.remove_source(*joined, 999).error() == not_found);
    REQUIRE(backend.remove_source(*joined, e).error() == not_found);
    // The target is not a source of the link.
    REQUIRE(backend.remove_source(*joined, c).error() == not_found);
    REQUIRE_EQ(backend.links().front().sources.size(), 3);

    // The injected error comes first.
    backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(backend.add_source(*joined, e).error() ==
            vss::make_error_code(vss::errc::io_error));
    backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(backend.remove_source(*joined, d).error() ==
            vss::make_error_code(vss::errc::io_error));
    REQUIRE_EQ(backend.links().front().sources.size(), 3);

    // Removing the owner releases it; removing another source keeps it.
    REQUIRE(backend.set_owner(*joined, d).has_value());
    REQUIRE(backend.remove_source(*joined, a).has_value());
    REQUIRE_EQ(backend.links().front().owner, d);
    REQUIRE(backend.remove_source(*joined, d).has_value());
    REQUIRE_EQ(backend.links().front().owner, vss::endpoint_id{0});
    REQUIRE_EQ(backend.links().front().id, *joined);

    // The last source stays, and the shape follows the count, as the
    // engine's does.
    REQUIRE_EQ(backend.links().front().sources.size(), 1);
    REQUIRE_EQ(backend.links().front().shape, vss::gui::link_shape::one_way);
    REQUIRE(backend.remove_source(*joined, b).error() == invalid);
    REQUIRE_EQ(backend.links().front().sources.size(), 1);

    // A monitor takes a source, and is a monitor again with one.
    const auto watched = backend.monitor(a, d);
    REQUIRE(watched.has_value());
    REQUIRE(backend.add_source(*watched, e).has_value());
    REQUIRE_EQ(backend.links().back().shape, vss::gui::link_shape::join);
    REQUIRE(!backend.links().back().options.emulate_modem_lines);
    REQUIRE(backend.remove_source(*watched, a).has_value());
    REQUIRE_EQ(backend.links().back().shape, vss::gui::link_shape::monitor);

    // Duplex links and buses are not edited this way.
    const auto split = backend.split(a, {b, c}, vss::link_options{});
    REQUIRE(split.has_value());
    REQUIRE(backend.add_source(*split, d).error() == unsupported);
    REQUIRE(backend.remove_source(*split, a).error() == unsupported);
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
    const auto bus = backend.bus({d, e});
    REQUIRE(bus.has_value());
    REQUIRE(backend.add_source(*bus, a).error() == unsupported);
    REQUIRE(backend.remove_source(*bus, d).error() == unsupported);
}

TEST_CASE("fake backend: add_member and remove_member edit a bus")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
    std::vector<vss::endpoint_id> ids;
    for (const char* name : {"a", "b", "c", "d", "e"})
    {
        const auto port = backend.create_virtual_port(
            QString::fromLatin1(name), vss::virtual_port_kind::loopback, true);
        REQUIRE(port.has_value());
        ids.push_back(*port);
    }
    const vss::endpoint_id a = ids[0];
    const vss::endpoint_id b = ids[1];
    const vss::endpoint_id c = ids[2];
    const vss::endpoint_id d = ids[3];
    const vss::endpoint_id e = ids[4];
    const auto not_found = vss::make_error_code(vss::errc::not_found);
    const auto unsupported = vss::make_error_code(vss::errc::unsupported);
    const auto invalid = vss::make_error_code(vss::errc::invalid_argument);
    const auto exists = vss::make_error_code(vss::errc::already_exists);

    const auto bus = backend.bus({a, b});
    REQUIRE(bus.has_value());
    const auto other = backend.bus({d, e});
    REQUIRE(other.has_value());

    // A member is added at the end, and the bus is the same bus.
    REQUIRE(backend.add_member(*bus, c).has_value());
    REQUIRE_EQ(backend.links().size(), std::size_t{2});
    REQUIRE_EQ(backend.links().front().id, *bus);
    REQUIRE_EQ(backend.links().front().shape, vss::gui::link_shape::bus);
    REQUIRE_EQ(backend.links().front().sources.size(), 3);
    REQUIRE_EQ(backend.links().front().sources.back(), c);
    REQUIRE_EQ(backend.links().front().targets.size(), 0);

    // Every error code, and nothing changed by any of them.
    REQUIRE(backend.add_member(999, c).error() == not_found);
    REQUIRE(backend.add_member(*bus, 999).error() == not_found);
    REQUIRE(backend.add_member(*bus, a).error() == exists);
    REQUIRE(backend.add_member(*bus, d).error() == exists);
    REQUIRE(backend.remove_member(999, a).error() == not_found);
    REQUIRE(backend.remove_member(*bus, 999).error() == not_found);
    REQUIRE(backend.remove_member(*bus, d).error() == not_found);
    REQUIRE_EQ(backend.links().front().sources.size(), 3);

    // The injected error comes first.
    backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(backend.remove_member(*bus, c).error() ==
            vss::make_error_code(vss::errc::io_error));
    REQUIRE_EQ(backend.links().front().sources.size(), 3);

    // A removed member is free for another bus, and the bus keeps two.
    REQUIRE(backend.remove_member(*bus, a).has_value());
    REQUIRE(backend.add_member(*other, a).has_value());
    REQUIRE_EQ(backend.links().front().sources.size(), 2);
    REQUIRE(backend.remove_member(*bus, b).error() == invalid);
    REQUIRE(backend.remove_member(*bus, c).error() == invalid);
    REQUIRE_EQ(backend.links().front().sources.size(), 2);
    REQUIRE_EQ(backend.links().front().id, *bus);

    // Other links are not edited this way.
    const auto joined = backend.join({a, b}, c, vss::link_options{});
    REQUIRE(joined.has_value());
    REQUIRE(backend.add_member(*joined, d).error() == unsupported);
    REQUIRE(backend.remove_member(*joined, a).error() == unsupported);
}

TEST_CASE("fake backend: a bus takes no more than the most members")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
    std::vector<vss::endpoint_id> ids;
    for (std::size_t index = 0; index <= vss::max_bus_members; ++index)
    {
        const auto port =
            backend.create_virtual_port(QStringLiteral("p%1").arg(index),
                                        vss::virtual_port_kind::loopback, true);
        REQUIRE(port.has_value());
        ids.push_back(*port);
    }
    const std::vector<vss::endpoint_id> first(
        ids.begin(), ids.begin() + (vss::max_bus_members - 1));
    const auto bus = backend.bus(first);
    REQUIRE(bus.has_value());
    REQUIRE(
        backend.add_member(*bus, ids[vss::max_bus_members - 1]).has_value());
    REQUIRE(backend.add_member(*bus, ids[vss::max_bus_members]).error() ==
            vss::make_error_code(vss::errc::invalid_argument));
}

TEST_CASE("fake backend: snapshot and apply round trip")
{
    vss::gui::FakeStudioBackend backend;
    const auto a = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    const auto b = backend.create_mock_port(QStringLiteral("fixture"),
                                            vss::serial_settings{},
                                            QStringLiteral("scripted"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(backend.connect_ports(*a, *b, vss::link_options{}).has_value());

    const vss::config::document document = backend.snapshot();
    REQUIRE_EQ(document.ports.size(), std::size_t{2});
    REQUIRE_EQ(document.links.size(), std::size_t{1});

    vss::gui::FakeStudioBackend copy;
    const auto applied = copy.apply(document, true);
    REQUIRE(applied.has_value());
    REQUIRE_EQ(copy.ports().size(), std::size_t{2});
    REQUIRE_EQ(copy.links().size(), std::size_t{1});
    REQUIRE_EQ(copy.ports()[0].name, QStringLiteral("a"));
    REQUIRE_EQ(copy.links()[0].shape, vss::gui::link_shape::two_way);
}

TEST_CASE("fake backend: errors are returned and capabilities are visible")
{
    vss::gui::FakeStudioBackend backend;
    backend.fail_next(vss::make_error_code(vss::errc::permission_denied));

    const auto failed = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    REQUIRE(!failed.has_value());
    REQUIRE_EQ(failed.error(),
               vss::make_error_code(vss::errc::permission_denied));
    REQUIRE(backend.ports().empty());

    const auto unknown = backend.remove_port(42);
    REQUIRE(!unknown.has_value());
    REQUIRE_EQ(unknown.error(), vss::make_error_code(vss::errc::not_found));

    vss::gui::capabilities caps;
    caps.faults = true;
    caps.peer_open = true;
    backend.set_capabilities(caps);
    REQUIRE(backend.caps().faults);
    REQUIRE(backend.caps().peer_open);

    const auto port = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    backend.set_peer_state(*port, vss::gui::peer_state::not_open);
    REQUIRE_EQ(vss_test::require_value(backend.port(*port)).peer,
               vss::gui::peer_state::not_open);
}
