//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/fake_studio_backend.hpp"

#include "test_framework.hpp"

#include <vss/core/error.hpp>
#include <vss/core/types.hpp>
#include <vss/ports/virtual_port.hpp>

#include <QByteArray>
#include <QString>

#include <algorithm>

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
