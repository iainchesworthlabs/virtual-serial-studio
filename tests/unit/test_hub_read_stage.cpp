//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The read time and the read stage: an endpoint reads the hub's clock once
// for a chunk, and a stage put on the endpoint is shown the chunk with that
// time before it is routed.  The last case changes the stage under traffic,
// and the ThreadSanitizer leg is what gives it its teeth.
//

#include "support/test_support.hpp"

#include <vss/core/endpoint.hpp>
#include <vss/core/hub.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

/// The hub's clock, moved by the test.  Copies share the one time.
class hand_clock
{
public:
    time_point now() const
    {
        return time_point{} + 1h + std::chrono::nanoseconds(ticks_->load());
    }

    void advance(std::chrono::nanoseconds by) const
    {
        ticks_->fetch_add(by.count());
    }

private:
    std::shared_ptr<std::atomic<std::int64_t>> ticks_ =
        std::make_shared<std::atomic<std::int64_t>>(0);
};

std::shared_ptr<vss::hub> start_hub_on(const hand_clock& clock)
{
    vss::hub_options options;
    options.io_threads = 2;
    options.traffic_capacity = 256;
    options.clock = [clock] { return clock.now(); };
    auto hub = std::make_shared<vss::hub>(options);
    hub->start();
    return hub;
}

/// A stage that keeps what it was shown.
class recording_stage final : public read_stage
{
public:
    struct seen
    {
        endpoint_id from = 0;
        std::string data;
        time_point when{};
        /// What `watched` had been given when the stage was called.
        std::string watched_before;
    };

    /// `watched`, when given, is looked at on every call: the mock transport
    /// of an endpoint the chunk is routed to.
    explicit recording_stage(
        std::shared_ptr<vss::serial_transport> watched = nullptr)
        : watched_(std::move(watched))
    {
    }

    void on_read(endpoint_id from, bytes_view data,
                 time_point when) noexcept override
    {
        seen entry;
        entry.from = from;
        entry.data.assign(data.begin(), data.end());
        entry.when = when;
        if (watched_)
            entry.watched_before = watched_->mock()->written();
        std::lock_guard lock(mutex_);
        seen_.push_back(std::move(entry));
    }

    std::vector<seen> snapshot() const
    {
        std::lock_guard lock(mutex_);
        return seen_;
    }

    std::string text() const
    {
        std::lock_guard lock(mutex_);
        std::string all;
        for (const auto& entry : seen_)
            all += entry.data;
        return all;
    }

private:
    const std::shared_ptr<vss::serial_transport> watched_;
    mutable std::mutex mutex_;
    std::vector<seen> seen_;
};

/// Puts `stage` on `port` and returns once it is there.
void put_stage(const mock_endpoint& port, std::shared_ptr<read_stage> stage)
{
    std::promise<void> done;
    auto finished = done.get_future();
    port.endpoint->set_read_stage(std::move(stage),
                                  [&done] { done.set_value(); });
    REQUIRE(finished.wait_for(10s) == std::future_status::ready);
}

event_recorder record_data(vss::hub& hub, endpoint_id of)
{
    event_recorder recorder;
    event_filter filter;
    filter.endpoint = of;
    filter.kind = event_kind::data;
    hub.subscribe(filter, [recorder](const endpoint_event& event)
                  { recorder.record(event); });
    return recorder;
}

} // namespace

TEST_CASE("read stage: a stage is shown each chunk, in order, with its "
          "endpoint")
{
    const auto hub = start_hub();
    const mock_endpoint port = add_mock(*hub, "port");
    const auto stage = std::make_shared<recording_stage>();
    put_stage(port, stage);

    port.inject_read("one");
    port.inject_read("two");
    port.inject_read("three");
    REQUIRE(wait_until([&] { return stage->text() == "onetwothree"; },
                       5000ms));

    time_point last{};
    for (const auto& entry : stage->snapshot())
    {
        REQUIRE_EQ(entry.from, port.endpoint->id());
        REQUIRE(entry.when >= last);
        last = entry.when;
    }
}

TEST_CASE("read stage: the stage is called before the chunk is routed")
{
    const auto hub = start_hub();
    const mock_endpoint from = add_mock(*hub, "from");
    const mock_endpoint to = add_mock(*hub, "to");
    REQUIRE(hub->connect(from.endpoint->id(), to.endpoint->id()));

    // The stage looks at the target's transport from the source's strand.
    // Nothing else touches that transport until the chunk is routed, which
    // is after the stage returns.
    const auto stage = std::make_shared<recording_stage>(to.transport);
    put_stage(from, stage);

    from.inject_read("first");
    REQUIRE(wait_until([&] { return to.written() == "first"; }, 5000ms));

    const auto seen = stage->snapshot();
    REQUIRE_EQ(seen.size(), std::size_t{1});
    REQUIRE_EQ(seen.front().data, std::string("first"));
    REQUIRE_EQ(seen.front().watched_before, std::string());
}

TEST_CASE("read stage: the stage and the data event carry the one read time")
{
    const hand_clock clock;
    const auto hub = start_hub_on(clock);
    const mock_endpoint port = add_mock(*hub, "port");
    const auto stage = std::make_shared<recording_stage>();
    put_stage(port, stage);
    const event_recorder events = record_data(*hub, port.endpoint->id());

    clock.advance(250ms);
    const time_point first = clock.now();
    port.inject_read("a");
    REQUIRE(wait_until([&] { return stage->text() == "a"; }, 5000ms));

    clock.advance(1s);
    const time_point second = clock.now();
    port.inject_read("b");
    REQUIRE(wait_until([&] { return events.count(event_kind::data) == 2; },
                       5000ms));

    const auto seen = stage->snapshot();
    REQUIRE_EQ(seen.size(), std::size_t{2});
    REQUIRE(seen[0].when == first);
    REQUIRE(seen[1].when == second);

    const auto published = events.snapshot();
    REQUIRE(published[0].when == first);
    REQUIRE(published[1].when == second);
}

TEST_CASE("read stage: a written chunk's event is timed by the hub's clock")
{
    const hand_clock clock;
    const auto hub = start_hub_on(clock);
    const mock_endpoint port = add_mock(*hub, "port");

    const event_recorder events = record_data(*hub, port.endpoint->id());

    clock.advance(3s);
    const time_point at = clock.now();
    port.endpoint->write(as_bytes("out"));
    REQUIRE(wait_until([&] { return events.count(event_kind::data) == 1; },
                       5000ms));
    for (const auto& event : events.snapshot())
        REQUIRE(event.when == at);
}

TEST_CASE("read stage: a stage that replaces another sees what follows, and "
          "the old one nothing more")
{
    const auto hub = start_hub();
    const mock_endpoint port = add_mock(*hub, "port");
    const auto old_stage = std::make_shared<recording_stage>();
    const auto new_stage = std::make_shared<recording_stage>();

    put_stage(port, old_stage);
    port.inject_read("before");
    REQUIRE(wait_until([&] { return old_stage->text() == "before"; }, 5000ms));

    put_stage(port, new_stage);
    port.inject_read("after");
    REQUIRE(wait_until([&] { return new_stage->text() == "after"; }, 5000ms));
    REQUIRE_EQ(old_stage->text(), std::string("before"));

    // The endpoint has let go of the old stage.
    REQUIRE_EQ(old_stage.use_count(), 1L);
}

TEST_CASE("read stage: a null stage removes the one there, and bytes still "
          "route")
{
    const auto hub = start_hub();
    const mock_endpoint from = add_mock(*hub, "from");
    const mock_endpoint to = add_mock(*hub, "to");
    REQUIRE(hub->connect(from.endpoint->id(), to.endpoint->id()));
    const auto stage = std::make_shared<recording_stage>();

    put_stage(from, stage);
    from.inject_read("seen");
    REQUIRE(wait_until([&] { return to.written() == "seen"; }, 5000ms));

    put_stage(from, nullptr);
    from.inject_read("unseen");
    REQUIRE(wait_until([&] { return to.written() == "seenunseen"; }, 5000ms));
    REQUIRE_EQ(stage->text(), std::string("seen"));
}

TEST_CASE("read stage: a delayed link waits for the hub's clock, not the "
          "wall clock")
{
    const hand_clock clock;
    const auto hub = start_hub_on(clock);
    const mock_endpoint from = add_mock(*hub, "from");
    const mock_endpoint to = add_mock(*hub, "to");
    link_options options;
    options.delay_ms = 100;
    REQUIRE(hub->connect(from.endpoint->id(), to.endpoint->id(), options));

    from.inject_read("held");
    REQUIRE(wait_until([&] { return from.endpoint->stats().bytes_rx == 4; },
                       5000ms));

    // The clock has not moved, so real time passing delivers nothing; nor
    // does a move short of the delay.
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(to.written(), std::string());
    clock.advance(99ms);
    std::this_thread::sleep_for(20ms);
    REQUIRE_EQ(to.written(), std::string());

    clock.advance(1ms);
    REQUIRE(wait_until([&] { return to.written() == "held"; }, 5000ms));
}

TEST_CASE("read stage: stages changed under traffic lose no chunk and repeat "
          "none")
{
    const auto hub = start_hub(4);
    const mock_endpoint port = add_mock(*hub, "port");

    constexpr int chunks = 2000;
    constexpr std::size_t stages = 8;
    std::vector<std::shared_ptr<recording_stage>> all;
    all.reserve(stages);
    for (std::size_t index = 0; index < stages; ++index)
        all.push_back(std::make_shared<recording_stage>());
    put_stage(port, all.front());

    std::atomic<bool> sending{true};
    std::thread changer(
        [&]
        {
            std::size_t next = 1;
            while (sending.load())
            {
                port.endpoint->set_read_stage(all[next % stages]);
                ++next;
                std::this_thread::yield();
            }
        });

    std::string sent;
    for (int number = 0; number < chunks; ++number)
    {
        const std::string chunk = std::to_string(number) + ';';
        sent += chunk;
        port.inject_read(chunk);
    }
    REQUIRE(wait_until(
        [&] { return port.endpoint->stats().bytes_rx == sent.size(); },
        10000ms));
    sending.store(false);
    changer.join();
    put_stage(port, nullptr);

    // Each chunk went to exactly one stage: together the stages saw every
    // number once.
    std::vector<recording_stage::seen> merged;
    for (const auto& stage : all)
    {
        const auto part = stage->snapshot();
        merged.insert(merged.end(), part.begin(), part.end());
    }
    // A read may carry more than one of the chunks sent.
    std::vector<int> numbers;
    numbers.reserve(static_cast<std::size_t>(chunks));
    for (const auto& entry : merged)
    {
        std::size_t from = 0;
        for (std::size_t end = entry.data.find(';', from);
             end != std::string::npos; end = entry.data.find(';', from))
        {
            numbers.push_back(std::stoi(entry.data.substr(from, end - from)));
            from = end + 1;
        }
    }
    REQUIRE_EQ(numbers.size(), static_cast<std::size_t>(chunks));
    std::sort(numbers.begin(), numbers.end());
    for (int number = 0; number < chunks; ++number)
        REQUIRE_EQ(numbers[static_cast<std::size_t>(number)], number);
}
