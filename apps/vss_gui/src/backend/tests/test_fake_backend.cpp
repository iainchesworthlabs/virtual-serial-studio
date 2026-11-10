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
#include <string>
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

namespace
{

/// A backend with a shared bus and the given half-duplex capability, and
/// three loopback ports.
struct bus_fixture
{
    explicit bus_fixture(bool half_duplex)
    {
        vss::gui::capabilities caps = backend.caps();
        caps.shared_bus = true;
        caps.half_duplex_bus = half_duplex;
        backend.set_capabilities(caps);
        for (const char* name : {"a", "b", "c"})
        {
            const auto made = backend.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::loopback,
                true);
            REQUIRE(made.has_value());
            members.push_back(*made);
        }
    }

    vss::gui::FakeStudioBackend backend;
    std::vector<vss::endpoint_id> members;
};

vss::link_options line_options()
{
    vss::link_options options;
    options.delay_ms = 20;
    options.jitter_ms = 5;
    options.half_duplex.enabled = true;
    options.half_duplex.bit_rate = 19200;
    options.half_duplex.turnaround_ms = 3;
    options.half_duplex.on_collision = vss::collision_action::garble;
    options.half_duplex.rts_gating = true;
    return options;
}

} // namespace

TEST_CASE("fake backend: a half-duplex bus keeps its line, delay and jitter")
{
    bus_fixture fixture(true);
    const auto made = fixture.backend.bus(fixture.members, line_options());
    REQUIRE(made.has_value());

    const auto links = fixture.backend.links();
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links[0].shape, vss::gui::link_shape::bus);
    REQUIRE(links[0].options.half_duplex == line_options().half_duplex);
    REQUIRE_EQ(links[0].options.delay_ms, 20u);
    REQUIRE_EQ(links[0].options.jitter_ms, 5u);
    REQUIRE(!links[0].options.emulate_modem_lines);

    // A plain bus has no half-duplex line.
    bus_fixture plain(true);
    REQUIRE(plain.backend.bus(plain.members, vss::link_options{}).has_value());
    REQUIRE(!plain.backend.links()[0].options.half_duplex.enabled);
}

TEST_CASE("fake backend: a half-duplex bus needs the capability")
{
    bus_fixture fixture(false);
    const auto refused = fixture.backend.bus(fixture.members, line_options());
    REQUIRE(!refused.has_value());
    REQUIRE_EQ(refused.error(), vss::make_error_code(vss::errc::unsupported));
    REQUIRE(fixture.backend.links().empty());

    // A bus without the line is still made.
    REQUIRE(
        fixture.backend.bus(fixture.members, vss::link_options{}).has_value());
}

TEST_CASE("fake backend: a bus refuses values outside the limits")
{
    bus_fixture fixture(true);
    const auto refused = [&fixture](const vss::link_options& options)
    {
        const auto made = fixture.backend.bus(fixture.members, options);
        return !made.has_value() &&
               made.error() ==
                   vss::make_error_code(vss::errc::invalid_argument);
    };

    vss::link_options slow = line_options();
    slow.half_duplex.bit_rate = vss::min_bus_bit_rate - 1;
    REQUIRE(refused(slow));
    vss::link_options fast = line_options();
    fast.half_duplex.bit_rate = vss::max_bus_bit_rate + 1;
    REQUIRE(refused(fast));
    vss::link_options turn = line_options();
    turn.half_duplex.turnaround_ms = vss::max_link_delay_ms + 1;
    REQUIRE(refused(turn));
    vss::link_options jitter = line_options();
    jitter.jitter_ms = jitter.delay_ms + 1;
    REQUIRE(refused(jitter));
    REQUIRE(fixture.backend.links().empty());
}

TEST_CASE("fake backend: snapshot and apply keep a bus line and its delay")
{
    bus_fixture fixture(true);
    REQUIRE(fixture.backend.bus(fixture.members, line_options()).has_value());

    const vss::config::document document = fixture.backend.snapshot();
    REQUIRE_EQ(document.links.size(), std::size_t{1});
    REQUIRE_EQ(document.links[0].mode, std::string("bus"));
    REQUIRE(document.links[0].half_duplex == line_options().half_duplex);
    REQUIRE_EQ(document.links[0].delay_ms, 20u);
    REQUIRE_EQ(document.links[0].jitter_ms, 5u);

    vss::gui::FakeStudioBackend copy;
    vss::gui::capabilities caps = copy.caps();
    caps.shared_bus = true;
    caps.half_duplex_bus = true;
    copy.set_capabilities(caps);
    REQUIRE(copy.apply(document, true).has_value());
    REQUIRE_EQ(copy.links().size(), std::size_t{1});
    REQUIRE(copy.links()[0].options.half_duplex == line_options().half_duplex);
    REQUIRE_EQ(copy.links()[0].options.delay_ms, 20u);
    REQUIRE_EQ(copy.links()[0].options.jitter_ms, 5u);

    // Without the capability the document is refused and nothing changes.
    vss::gui::FakeStudioBackend limited;
    caps.half_duplex_bus = false;
    limited.set_capabilities(caps);
    const auto refused = limited.apply(document, true);
    REQUIRE(!refused.has_value());
    REQUIRE_EQ(refused.error(), vss::make_error_code(vss::errc::unsupported));
    REQUIRE(limited.links().empty());
}
