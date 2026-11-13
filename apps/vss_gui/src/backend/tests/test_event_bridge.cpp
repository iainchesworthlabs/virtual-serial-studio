//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/engine_studio_backend.hpp"
#include "backend/event_bridge.hpp"

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include <vss/core/hub.hpp>
#include <vss/ports/peer_open.hpp>
#include <vss/studio/studio.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

vss::endpoint_event make_event(int sequence)
{
    vss::endpoint_event event;
    event.kind = vss::event_kind::log_message;
    event.endpoint = 1;
    event.name = "test";
    event.message = "event " + std::to_string(sequence);
    event.when = vss::clock_type::now();
    return event;
}

using vss_test::spin;
using vss_test::wait_until;

vss::hub_options make_hub_options()
{
    vss::hub_options options;
    options.io_threads = 1;
    options.traffic_capacity = 64;
    return options;
}

} // namespace

TEST_CASE("event bridge: batches hub events on the GUI thread")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 60000;
    options.max_events = 128;

    {
        vss::gui::EventBridge bridge(hub, options);
        int batches = 0;
        std::size_t last_size = 0;
        QThread* delivery_thread = nullptr;
        QObject::connect(&bridge, &vss::gui::EventBridge::batch_ready, &bridge,
                         [&](const vss::gui::event_batch& events)
                         {
                             ++batches;
                             last_size =
                                 static_cast<std::size_t>(events.size());
                             delivery_thread = QThread::currentThread();
                         });

        bridge.start();
        for (int index = 0; index < 40; ++index)
            hub.publish(make_event(index));

        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 40; }));

        REQUIRE_EQ(bridge.delivered(), std::uint64_t{40});
        REQUIRE_EQ(batches, 0);

        bridge.flush();
        REQUIRE_EQ(batches, 1);
        REQUIRE_EQ(last_size, std::size_t{40});
        REQUIRE(delivery_thread == QCoreApplication::instance()->thread());
    }

    hub.stop();
    hub.join();
}

TEST_CASE("event bridge: the bytes of a batch share one block")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 60000;
    {
        vss::gui::EventBridge bridge(hub, options);
        vss::gui::event_batch seen;
        QObject::connect(&bridge, &vss::gui::EventBridge::batch_ready, &bridge,
                         [&seen](const vss::gui::event_batch& events)
                         { seen = events; });
        bridge.start();

        // Data of three sizes from two ports, and an event with no bytes
        // between them.
        const std::vector<std::string> payloads{"first", std::string(300, 'b'),
                                                "c"};
        for (std::size_t index = 0; index < payloads.size(); ++index)
        {
            vss::endpoint_event event;
            event.kind = vss::event_kind::data;
            event.endpoint = index == 1 ? 2 : 1;
            event.name = index == 1 ? "other" : "port";
            event.direction = vss::data_direction::tx;
            vss::byte_buffer bytes;
            for (const char character : payloads[index])
                bytes.push_back(static_cast<vss::byte>(character));
            event.data =
                std::make_shared<const vss::byte_buffer>(std::move(bytes));
            hub.publish(event);
            if (index == 0)
                hub.publish(make_event(7));
        }
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 4; }));
        bridge.flush();

        REQUIRE_EQ(seen.size(), 4);
        REQUIRE_EQ(seen[0].data.to_byte_array(), QByteArray("first"));
        REQUIRE(seen[1].data.empty());
        REQUIRE_EQ(seen[1].message, QStringLiteral("event 7"));
        REQUIRE_EQ(seen[2].data.to_byte_array(), QByteArray(300, 'b'));
        REQUIRE_EQ(seen[2].data.size(), 300);
        REQUIRE_EQ(seen[3].data.to_byte_array(), QByteArray("c"));
        REQUIRE_EQ(seen[3].data.first(256).size(), 1);
        REQUIRE_EQ(seen[0].name, QStringLiteral("port"));
        REQUIRE_EQ(seen[2].name, QStringLiteral("other"));
        REQUIRE_EQ(seen[3].name, QStringLiteral("port"));
        // The parts follow each other in one block.
        REQUIRE(seen[2].data.data() == seen[0].data.data() + 5);
        REQUIRE(seen[3].data.data() == seen[2].data.data() + 300);

        // A port that is renamed gets its new name.
        vss::endpoint_event renamed = make_event(8);
        renamed.name = "renamed";
        hub.publish(renamed);
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 5; }));
        bridge.flush();
        REQUIRE_EQ(seen.size(), 1);
        REQUIRE_EQ(seen[0].name, QStringLiteral("renamed"));
    }

    hub.stop();
    hub.join();
}

TEST_CASE("event bridge: caps the queue and counts dropped events")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 60000;
    options.max_events = 10;

    {
        vss::gui::EventBridge bridge(hub, options);
        int batches = 0;
        std::size_t last_size = 0;
        QObject::connect(&bridge, &vss::gui::EventBridge::batch_ready, &bridge,
                         [&](const vss::gui::event_batch& events)
                         {
                             ++batches;
                             last_size =
                                 static_cast<std::size_t>(events.size());
                         });

        bridge.start();
        for (int index = 0; index < 50; ++index)
            hub.publish(make_event(index));
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 50; }));

        REQUIRE_EQ(bridge.delivered(), std::uint64_t{50});
        REQUIRE_EQ(bridge.dropped(), std::uint64_t{40});
        bridge.flush();
        REQUIRE_EQ(batches, 1);
        REQUIRE_EQ(last_size, std::size_t{10});
        REQUIRE_EQ(bridge.dropped(), std::uint64_t{40});
    }

    hub.stop();
    hub.join();
}

TEST_CASE("event bridge: stop unsubscribes but keeps the shared queue alive")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 60000;

    {
        vss::gui::EventBridge bridge(hub, options);
        // A second subscriber sees every event the hub dispatches. Sinks run
        // in publish order on one strand, so once it has seen the last event
        // the hub has finished dispatching all the earlier ones.
        auto seen = std::make_shared<std::atomic<int>>(0);
        const auto probe =
            hub.subscribe({}, [seen](const vss::endpoint_event&)
                          { seen->fetch_add(1, std::memory_order_relaxed); });

        bridge.start();
        for (int index = 0; index < 5; ++index)
            hub.publish(make_event(index));
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 5; }));
        REQUIRE_EQ(bridge.delivered(), std::uint64_t{5});

        bridge.stop();
        for (int index = 0; index < 5; ++index)
            hub.publish(make_event(index));
        // Plus one more, so the dispatch of the fifth is also finished.
        hub.publish(make_event(5));
        REQUIRE(wait_until([&seen] { return seen->load() >= 11; }));
        REQUIRE_EQ(bridge.delivered(), std::uint64_t{5});
        hub.unsubscribe(probe);
    }

    hub.stop();
    hub.join();
}

TEST_CASE("event bridge: restart drops events queued before stop")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 60000;

    {
        vss::gui::EventBridge bridge(hub, options);
        int batches = 0;
        QObject::connect(&bridge, &vss::gui::EventBridge::batch_ready, &bridge,
                         [&batches](const vss::gui::event_batch&)
                         { ++batches; });

        bridge.start();
        for (int index = 0; index < 3; ++index)
            hub.publish(make_event(index));
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 3; }));
        REQUIRE_EQ(bridge.delivered(), std::uint64_t{3});

        bridge.stop();
        bridge.start();
        bridge.flush();
        REQUIRE_EQ(batches, 0);

        hub.publish(make_event(99));
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 4; }));
        bridge.flush();
        REQUIRE_EQ(batches, 1);
    }

    hub.stop();
    hub.join();
}

TEST_CASE("event bridge: survives a publisher during shutdown")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    std::atomic<bool> running{true};
    std::atomic<int> published{0};
    std::thread publisher(
        [&hub, &running, &published]
        {
            int sequence = 0;
            while (running.load(std::memory_order_relaxed))
            {
                hub.publish(make_event(sequence++));
                published.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

    REQUIRE(wait_until([&published] { return published.load() > 0; }));

    vss::gui::EventBridge::options options;
    options.interval_ms = 1;
    {
        vss::gui::EventBridge bridge(hub, options);
        bridge.start();
        for (int index = 0; index < 10; ++index)
            spin(2);
    }

    running.store(false);
    publisher.join();
    hub.stop();
    hub.join();
    REQUIRE(published.load() > 0);
}

TEST_CASE("engine backend: ports, links and sends go through the adapter")
{
    vss::studio studio;
    {
        vss::gui::EngineStudioBackend backend(studio);
        const auto first = backend.create_virtual_port(
            QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
        const auto second = backend.create_virtual_port(
            QStringLiteral("b"), vss::virtual_port_kind::loopback, true);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE_EQ(backend.ports().size(), std::size_t{2});
        // Only a pseudo terminal on a platform that can inspect other
        // processes has a definite answer; a loopback port never does.
        REQUIRE_EQ(backend.caps().peer_open, vss::peer_open_supported());
        REQUIRE(backend.caps().line_format);
        REQUIRE_EQ(vss_test::require_value(backend.port(*first)).peer,
                   vss::gui::peer_state::unknown);

        const auto link =
            backend.connect_ports(*first, *second, vss::link_options{});
        REQUIRE(link.has_value());
        REQUIRE_EQ(backend.links().size(), std::size_t{1});
        REQUIRE_EQ(backend.links()[0].shape, vss::gui::link_shape::two_way);

        REQUIRE(backend.send(*first, QByteArrayLiteral("hello")).has_value());
        // The send is written on the endpoint's own strand, so the counter
        // moves some time later. Poll the view until it does.
        REQUIRE(wait_until(
            [&backend, &first]
            {
                backend.refresh();
                return backend.port(*first)->stats.bytes_tx >= 5;
            }));
    }
}

TEST_CASE("engine backend: the line wiring of a link can be read and changed")
{
    vss::studio studio;
    vss::gui::EngineStudioBackend backend(studio);
    REQUIRE(backend.caps().line_matrix);

    const auto first = backend.create_virtual_port(
        QStringLiteral("a"), vss::virtual_port_kind::loopback, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("b"), vss::virtual_port_kind::loopback, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto link =
        backend.connect_ports(*first, *second, vss::link_options{});
    REQUIRE(link.has_value());

    // A new link has the null-modem cable.
    const auto standard = backend.link_wiring(*link);
    REQUIRE(standard.has_value());
    REQUIRE(*standard == vss::gui::line_wiring{});

    vss::gui::line_wiring wiring;
    wiring.dtr_to_dsr = false;
    wiring.dtr_to_ri = true;
    wiring.rts_to_cts = false;
    wiring.rts_to_dcd = true;
    REQUIRE(backend.set_link_wiring(*link, wiring).has_value());
    const auto changed = backend.link_wiring(*link);
    REQUIRE(changed.has_value());
    REQUIRE(*changed == wiring);

    REQUIRE(!backend.set_link_wiring(vss::link_id{9999}, wiring).has_value());
    REQUIRE(!backend.link_wiring(vss::link_id{9999}).has_value());
}

TEST_CASE(
    "engine backend: a send from a fixture reaches the far end of its link")
{
    vss::studio studio;
    vss::gui::EngineStudioBackend backend(studio);
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    const auto peer = backend.create_virtual_port(
        QStringLiteral("peer"), vss::virtual_port_kind::loopback, true);
    REQUIRE(fixture.has_value());
    REQUIRE(peer.has_value());
    REQUIRE(backend.connect_ports(*fixture, *peer, vss::link_options{})
                .has_value());

    // The fixture is the device: its bytes arrive on it and the hub writes
    // them to the peer.
    REQUIRE(backend.send(*fixture, QByteArrayLiteral("hello")).has_value());
    REQUIRE(wait_until(
        [&]
        {
            backend.refresh();
            return backend.port(*fixture)->stats.bytes_rx >= 5 &&
                   backend.port(*peer)->stats.bytes_tx >= 5;
        }));
}

TEST_CASE("engine backend: a fixture can echo, and a plain port cannot")
{
    vss::studio studio;
    vss::gui::EngineStudioBackend backend(studio);
    REQUIRE(backend.caps().fixture_echo);
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    const auto plain = backend.create_virtual_port(
        QStringLiteral("plain"), vss::virtual_port_kind::loopback, true);
    REQUIRE(fixture.has_value());
    REQUIRE(plain.has_value());

    REQUIRE(!backend.set_echo(*plain, true).has_value());
    REQUIRE(!backend.echo(*plain));
    REQUIRE(backend.set_echo(*fixture, true).has_value());
    REQUIRE(backend.echo(*fixture));

    // A byte written to the fixture comes back from it.
    studio.engine().find_endpoint(*fixture)->write(vss::as_bytes("ab"));
    REQUIRE(wait_until(
        [&]
        {
            backend.refresh();
            return backend.port(*fixture)->stats.bytes_rx >= 2;
        }));

    REQUIRE(backend.set_echo(*fixture, false).has_value());
    REQUIRE(!backend.echo(*fixture));
}

TEST_CASE("event bridge: a flood is delivered in slices, none of them larger "
          "than the limit")
{
    vss::hub hub{make_hub_options()};
    hub.start();

    vss::gui::EventBridge::options options;
    options.interval_ms = 20;
    options.max_events = 1000;
    options.max_batch = 25;

    {
        vss::gui::EventBridge bridge(hub, options);
        int batches = 0;
        std::size_t largest = 0;
        std::size_t total = 0;
        QObject::connect(
            &bridge, &vss::gui::EventBridge::batch_ready, &bridge,
            [&](const vss::gui::event_batch& events)
            {
                ++batches;
                largest =
                    std::max(largest, static_cast<std::size_t>(events.size()));
                total += static_cast<std::size_t>(events.size());
            });

        bridge.start();
        for (int index = 0; index < 200; ++index)
            hub.publish(make_event(index));
        REQUIRE(wait_until([&bridge] { return bridge.delivered() >= 200; }));

        // Every event arrives, but the timer path never hands the window more
        // than a slice at a time.
        REQUIRE(wait_until([&] { return total == 200; }));
        REQUIRE(largest <= std::size_t{25});
        REQUIRE(batches >= 8);
        REQUIRE_EQ(bridge.pending(), std::size_t{0});
    }

    hub.stop();
    hub.join();
}

TEST_CASE("engine backend: traffic raises stats_changed and leaves "
          "ports_changed alone")
{
    vss::studio studio;
    vss::gui::EngineStudioBackend backend(studio);
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    const auto peer = backend.create_virtual_port(
        QStringLiteral("peer"), vss::virtual_port_kind::loopback, true);
    REQUIRE(fixture.has_value());
    REQUIRE(peer.has_value());
    REQUIRE(backend.connect_ports(*fixture, *peer, vss::link_options{})
                .has_value());
    // Let the setup settle, so the opening of the ports is not counted.
    spin(100);
    backend.refresh();

    int ports_changed = 0;
    int links_changed = 0;
    int stats_changed = 0;
    QObject::connect(&backend, &vss::gui::StudioBackend::ports_changed,
                     [&ports_changed] { ++ports_changed; });
    QObject::connect(&backend, &vss::gui::StudioBackend::links_changed,
                     [&links_changed] { ++links_changed; });
    QObject::connect(&backend, &vss::gui::StudioBackend::stats_changed,
                     [&stats_changed] { ++stats_changed; });

    REQUIRE(backend.send(*fixture, QByteArrayLiteral("hello")).has_value());
    REQUIRE(wait_until(
        [&]
        {
            backend.refresh();
            return backend.port(*peer)->stats.bytes_tx >= 5;
        }));

    // The numbers moved, and only the numbers: nothing is told that a port or
    // a link changed, so no pane rebuilds itself for a byte count.
    REQUIRE(stats_changed > 0);
    REQUIRE_EQ(ports_changed, 0);
    REQUIRE_EQ(links_changed, 0);
}
