//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <boost/asio/error.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

std::shared_ptr<serial_transport> make_fresh(vss::hub& hub,
                                             const std::string& name)
{
    boost::system::error_code ec;
    auto transport = vss::serial_transport::make_mock(
        hub.executor(), vss::serial_settings{}, name, ec);
    REQUIRE(!ec);
    REQUIRE(transport != nullptr);
    return transport;
}

/// An endpoint whose transport fails at once, so it ends up `failed`, wired to
/// a peer by a duplex link.
struct dead_endpoint
{
    std::shared_ptr<vss::hub> hub = start_hub();
    mock_endpoint peer = add_mock(*hub, "peer");
    std::shared_ptr<endpoint> device;
    link_id link = 0;

    dead_endpoint()
    {
        auto failing = std::make_shared<failing_transport>(
            hub->executor(),
            boost::system::error_code(boost::asio::error::network_down));
        const auto added =
            hub->add_endpoint("device", endpoint_kind::tcp, failing);
        REQUIRE(added);
        device = *added;
        const auto linked = hub->connect(device->id(), peer.endpoint->id());
        REQUIRE(linked);
        link = *linked;
        REQUIRE(wait_until(
            [&] { return device->state() == endpoint_state::failed; }, 5000ms));
    }
};

} // namespace

TEST_CASE("reopen: a failed endpoint comes back and carries bytes again")
{
    dead_endpoint dead;
    REQUIRE(!dead.device->is_open());

    auto fresh = make_fresh(*dead.hub, "device");
    REQUIRE(dead.device->reopen(fresh));
    REQUIRE(wait_until([&] { return dead.device->is_open(); }, 5000ms));
    REQUIRE(dead.device->state() == endpoint_state::open);

    // Both ways across the link that was there before.
    const mock_endpoint device{dead.device, fresh};
    device.inject_read("up");
    REQUIRE(wait_until([&] { return dead.peer.written() == "up"; }, 5000ms));
    dead.peer.inject_read("down");
    REQUIRE(wait_until([&] { return device.written() == "down"; }, 5000ms));
}

TEST_CASE("reopen: the id, the name and the links stay")
{
    dead_endpoint dead;
    const endpoint_id id = dead.device->id();

    REQUIRE(dead.device->reopen(make_fresh(*dead.hub, "device")));

    REQUIRE_EQ(dead.device->id(), id);
    REQUIRE_EQ(dead.device->name(), std::string("device"));
    REQUIRE(dead.hub->find_endpoint("device") == dead.device);
    const auto link = dead.hub->find_link(dead.link);
    REQUIRE(link);
    REQUIRE_EQ(link->sources.size(), std::size_t{1});
    REQUIRE_EQ(link->sources[0], id);
}

TEST_CASE("reopen: a started event follows, and the state goes through opening")
{
    dead_endpoint dead;

    // A delivery that is already running cannot be recalled by unsubscribe, so
    // what the sink writes to must outlive the test body.
    struct seen_events
    {
        std::mutex mutex;
        std::vector<std::string> events;
    };
    const auto seen = std::make_shared<seen_events>();
    const auto subscription = dead.hub->subscribe(
        event_filter{dead.device->id(), std::nullopt},
        [seen](const endpoint_event& event) {
            std::lock_guard lock(seen->mutex);
            seen->events.push_back(std::string(to_string(event.kind)) + ":" +
                                   event.message);
        });

    REQUIRE(dead.device->reopen(make_fresh(*dead.hub, "device")));
    REQUIRE(wait_until(
        [&] {
            std::lock_guard lock(seen->mutex);
            for (const auto& event : seen->events)
            {
                if (event.find("started") == 0)
                    return true;
            }
            return false;
        },
        5000ms));
    dead.hub->unsubscribe(subscription);
}

TEST_CASE("reopen: an open endpoint can be restarted on a new transport")
{
    auto hub = start_hub();
    auto peer = add_mock(*hub, "peer");
    auto first = add_mock(*hub, "device");
    REQUIRE(hub->connect(first.endpoint->id(), peer.endpoint->id()));

    auto second = make_fresh(*hub, "device");
    REQUIRE(first.endpoint->reopen(second));

    // The transport it had is closed; the new one carries the traffic.
    // Asked on the endpoint's strand, where the transport lives: the mock port
    // is a single-threaded object and the close itself happens there.
    REQUIRE(wait_until(
        [&] {
            bool open = true;
            first.on_strand([&] { open = first.transport->is_open(); });
            return !open;
        },
        5000ms));
    const mock_endpoint now{first.endpoint, second};
    peer.inject_read("to-new");
    REQUIRE(wait_until([&] { return now.written() == "to-new"; }, 5000ms));
    REQUIRE_EQ(first.written(), std::string());

    // Bytes the old transport sends after the swap are not heard.
    first.inject_read("stale");
    now.inject_read("fresh");
    REQUIRE(wait_until([&] { return peer.written() == "fresh"; }, 5000ms));
    REQUIRE_EQ(peer.written(), std::string("fresh"));
}

TEST_CASE("reopen: it can be done more than once")
{
    dead_endpoint dead;
    for (int round = 0; round < 3; ++round)
    {
        auto fresh = make_fresh(*dead.hub, "device");
        REQUIRE(dead.device->reopen(fresh));
        const mock_endpoint device{dead.device, fresh};
        const std::string text = "round" + std::to_string(round);
        device.inject_read(text);
        REQUIRE(wait_until(
            [&] {
                return dead.peer.written().find(text) != std::string::npos;
            },
            5000ms));
    }
    REQUIRE(dead.peer.written() == "round0round1round2");
}

TEST_CASE("reopen: the arguments are checked")
{
    dead_endpoint dead;

    const auto none = dead.device->reopen(nullptr);
    REQUIRE(!none);
    REQUIRE_EQ(none.error(), make_error_code(errc::invalid_argument));

    auto closed = make_fresh(*dead.hub, "device");
    closed->close();
    const auto not_open = dead.device->reopen(closed);
    REQUIRE(!not_open);
    REQUIRE_EQ(not_open.error(), make_error_code(errc::not_open));
    // A refused reopen leaves the endpoint as it was.
    REQUIRE(dead.device->state() == endpoint_state::failed);

    dead.hub->stop();
    const auto stopped = dead.device->reopen(make_fresh(*dead.hub, "device"));
    REQUIRE(!stopped);
    REQUIRE_EQ(stopped.error(), make_error_code(errc::not_open));
}

TEST_CASE("reopen: bytes queued for the old transport are not sent to the new one")
{
    auto hub = start_hub();
    auto stalled =
        std::make_shared<stalled_transport>(hub->executor());
    const auto added = hub->add_endpoint("slow", endpoint_kind::tcp, stalled);
    REQUIRE(added);

    // Writes the stalled transport never completes pile up behind it.
    (*added)->write(as_bytes("old"));
    (*added)->write(as_bytes("older"));

    auto fresh = make_fresh(*hub, "slow");
    REQUIRE((*added)->reopen(fresh));
    const mock_endpoint slow{*added, fresh};
    (*added)->write(as_bytes("new"));
    REQUIRE(wait_until([&] { return slow.written() == "new"; }, 5000ms));
    REQUIRE_EQ(slow.written(), std::string("new"));
}

TEST_CASE("reopen: a write racing the reopen never reaches the new transport")
{
    // The write that was scheduled for the old transport reads the head of
    // the queue and then the transport.  A reopen landing between the two
    // used to hand the old bytes to the new transport, so the new transport
    // saw "old" before "new" and the wait below timed out.  Repeat it so the
    // two threads meet.
    for (int round = 0; round < 300; ++round)
    {
        auto hub = start_hub();
        auto stalled = std::make_shared<stalled_transport>(hub->executor());
        const auto added =
            hub->add_endpoint("slow", endpoint_kind::tcp, stalled);
        REQUIRE(added);

        (*added)->write(as_bytes("old"));
        auto fresh = make_fresh(*hub, "slow");
        REQUIRE((*added)->reopen(fresh));
        const mock_endpoint slow{*added, fresh};
        (*added)->write(as_bytes("new"));
        REQUIRE(wait_until([&] { return slow.written() == "new"; }, 5000ms));
        REQUIRE_EQ(slow.written(), std::string("new"));
    }
}
