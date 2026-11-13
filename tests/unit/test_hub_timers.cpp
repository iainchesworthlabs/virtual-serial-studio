//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// hub_options::fine_timers.  What it does to the time a timer takes is a
// platform matter and is checked in unit/platform/windows; here is what holds
// everywhere.
//

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <chrono>
#include <memory>
#include <string>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

std::shared_ptr<vss::hub> make_hub(bool fine_timers)
{
    hub_options options;
    options.io_threads = 2;
    options.traffic_capacity = 256;
    options.fine_timers = fine_timers;
    return std::make_shared<vss::hub>(options);
}

/// Sends `text` over a link with a 5 ms delay and waits for it.
void require_delayed_delivery(vss::hub& hub, const std::string& text)
{
    mock_endpoint from = add_mock(hub, "from");
    mock_endpoint to = add_mock(hub, "to");
    link_options options;
    options.delay_ms = 5;
    REQUIRE(hub.connect(from.endpoint->id(), to.endpoint->id(), options));

    const auto sent = std::chrono::steady_clock::now();
    from.inject_read(text);
    REQUIRE(wait_until([&] { return to.written() == text; }, 5000ms));
    REQUIRE(std::chrono::steady_clock::now() - sent >= 5ms);
}

} // namespace

TEST_CASE("timers: a hub asks for fine timers unless told not to")
{
    REQUIRE(hub_options{}.fine_timers);
}

TEST_CASE("timers: a hub runs, stops and runs again with fine timers")
{
    const auto hub = make_hub(true);
    for (int round = 0; round < 3; ++round)
    {
        hub->start();
        REQUIRE(hub->running());
        hub->stop();
        hub->join();
        REQUIRE(!hub->running());
    }
}

TEST_CASE("timers: a hub that is never joined by hand gives its request back")
{
    // The destructor stops and joins; a sanitizer leg sees a request that is
    // not returned as a leak of the object that holds it.
    const auto hub = make_hub(true);
    hub->start();
    hub->start();
    REQUIRE(hub->running());
}

TEST_CASE("timers: a delayed link delivers with fine timers on")
{
    const auto hub = make_hub(true);
    hub->start();
    require_delayed_delivery(*hub, "fine");
}

TEST_CASE("timers: a delayed link delivers with fine timers off")
{
    const auto hub = make_hub(false);
    hub->start();
    require_delayed_delivery(*hub, "coarse");
}

TEST_CASE("timers: two hubs hold their requests at once")
{
    const auto first = make_hub(true);
    const auto second = make_hub(true);
    first->start();
    second->start();
    require_delayed_delivery(*first, "first");

    // The first hub's request goes; the second still has its own.
    first->stop();
    first->join();
    require_delayed_delivery(*second, "second");
}
