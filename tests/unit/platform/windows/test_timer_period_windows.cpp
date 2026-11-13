//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On Windows a timer fires on a 15.6 ms tick unless the process asks for
// more.  A running hub asks (hub_options::fine_timers), and this checks that
// the request has its effect on a timer of the hub.
//

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

using namespace vss;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

/// How long a 1 ms timer of the hub takes, as the middle one of `count`
/// waits made one after another.  The middle one, because a loaded machine
/// makes a few of them long whatever the timer period is.
std::chrono::microseconds median_one_millisecond_wait(vss::hub& hub,
                                                      std::size_t count)
{
    std::vector<std::chrono::microseconds> took;
    took.reserve(count);
    boost::asio::steady_timer timer(hub.io());
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto before = std::chrono::steady_clock::now();
        timer.expires_after(1ms);
        timer.async_wait(boost::asio::use_future).get();
        took.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - before));
    }
    std::sort(took.begin(), took.end());
    return took[took.size() / 2];
}

} // namespace

TEST_CASE("timers: a 1 ms timer of a running hub is not held to the 15.6 ms "
          "tick")
{
    const auto hub = start_hub();

    // On the tick every wait takes about 15 ms.  With the request it takes 1
    // to 2 ms; 8 ms is far from both.
    const auto middle = median_one_millisecond_wait(*hub, 31);
    REQUIRE(middle >= 1ms);
    REQUIRE(middle < 8ms);
}
