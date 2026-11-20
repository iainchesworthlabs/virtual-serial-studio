//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The traffic tap, with a real hub and mock ports.
//

#include "support/test_support.hpp"

#include <vss/plugin/host/traffic_tap.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;
using namespace vss::plugin::host;

namespace
{
using namespace std::chrono_literals;

std::string text_of(const tap_item& item)
{
    if (!item.bytes)
        return {};
    return std::string(item.bytes->begin(), item.bytes->end());
}

// Takes until `count` items and drops together are seen, or the time is up.
struct collected
{
    std::vector<tap_item> items;
    std::uint64_t dropped_items = 0;
    std::uint64_t dropped_bytes = 0;
};

collected collect(traffic_tap& tap, std::size_t count)
{
    collected out;
    wait_until(
        [&] {
            auto batch = tap.take(1000000, 1000000000);
            for (auto& item : batch.items)
                out.items.push_back(std::move(item));
            out.dropped_items += batch.dropped_items;
            out.dropped_bytes += batch.dropped_bytes;
            return out.items.size() + out.dropped_items >= count;
        },
        5000ms);
    return out;
}

void inject_now(const mock_endpoint& port, std::string data)
{
    boost::asio::post(port.endpoint->get_executor(),
                      [transport = port.transport, data = std::move(data)] {
                          transport->mock()->inject_read(data);
                      });
}

// A recorder subscribed after the tap's sink.  The hub runs sinks one at a
// time in the order they subscribed, so when the recorder has seen an event
// the tap has too.
struct follower
{
    event_recorder recorder;

    explicit follower(hub& source)
    {
        source.subscribe(event_filter{},
                         [recorder = recorder](const endpoint_event& event) {
                             recorder.record(event);
                         });
    }

    // Sends `text` from `port` and waits until the hub has delivered it, so
    // that the next send cannot be joined to it into one chunk.
    void send(const mock_endpoint& port, const std::string& text)
    {
        port.inject_read(text);
        ++sent_;
        REQUIRE(wait_until(
            [&] { return recorder.count(event_kind::data) >= sent_; },
            20000ms));
    }

    // Waits until the events seen so far carry `bytes` bytes of data.
    void wait_bytes(std::size_t bytes) const
    {
        REQUIRE(wait_until(
            [&] {
                std::size_t total = 0;
                for (const auto& event : recorder.snapshot())
                {
                    if (event.kind == event_kind::data && event.data)
                        total += event.data->size();
                }
                return total >= bytes;
            },
            20000ms));
    }

private:
    std::size_t sent_ = 0;
};

tap_place place_of(std::string name, const mock_endpoint& port)
{
    tap_place place;
    place.name = std::move(name);
    place.endpoints = {port.endpoint->id()};
    return place;
}

} // namespace

TEST_CASE("traffic tap: items arrive in order with place, direction, bytes")
{
    auto hub = start_hub();
    auto first = add_mock(*hub, "first");
    auto second = add_mock(*hub, "second");
    auto other = add_mock(*hub, "other");
    traffic_tap tap(*hub, {place_of("A", first), place_of("B", second)});
    follower after(*hub);

    after.send(first, "one");
    after.send(first, "two");
    after.send(second, "x");
    after.send(other, "not watched");

    const auto got = collect(tap, 3);
    REQUIRE_EQ(got.items.size(), std::size_t{3});
    REQUIRE_EQ(got.dropped_items, std::uint64_t{0});

    std::string at_a;
    std::string at_b;
    time_point last{};
    for (const auto& item : got.items)
    {
        REQUIRE(item.kind == tap_item_kind::data);
        REQUIRE(item.direction == data_direction::rx);
        REQUIRE(item.when != time_point{});
        REQUIRE(item.when >= last);
        last = item.when;
        (item.place == "A" ? at_a : at_b) += text_of(item);
        REQUIRE(item.place == "A" || item.place == "B");
    }
    REQUIRE_EQ(at_a, std::string("onetwo"));
    REQUIRE_EQ(at_b, std::string("x"));
}

TEST_CASE("traffic tap: bytes sent on a link are seen as tx at the target")
{
    auto hub = start_hub();
    auto source = add_mock(*hub, "source");
    auto target = add_mock(*hub, "target");
    REQUIRE(hub->connect(source.endpoint->id(), target.endpoint->id()));
    traffic_tap tap(*hub, {place_of("T", target)});

    source.inject_read("hello");

    const auto got = collect(tap, 1);
    REQUIRE_EQ(got.items.size(), std::size_t{1});
    REQUIRE(got.items[0].direction == data_direction::tx);
    REQUIRE_EQ(text_of(got.items[0]), std::string("hello"));
}

TEST_CASE("traffic tap: a full queue drops, counts and keeps the oldest")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");
    tap_limits limits;
    limits.max_items = 4;
    traffic_tap tap(*hub, {place_of("P", port)}, limits);
    follower after(*hub);

    for (int i = 0; i < 10; ++i)
        after.send(port, std::to_string(i) + "-");

    const auto got = collect(tap, 10);
    REQUIRE_EQ(got.items.size(), std::size_t{4});
    REQUIRE_EQ(got.dropped_items, std::uint64_t{6});
    REQUIRE_EQ(got.dropped_bytes, std::uint64_t{12});
    REQUIRE_EQ(text_of(got.items[0]), std::string("0-"));
    REQUIRE_EQ(text_of(got.items[3]), std::string("3-"));
    // The counters are since the last take.
    const auto again = tap.take(10, 100);
    REQUIRE_EQ(again.dropped_items, std::uint64_t{0});
    REQUIRE_EQ(again.dropped_bytes, std::uint64_t{0});
}

TEST_CASE("traffic tap: limits at their edge")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");

    {
        tap_limits limits;
        limits.max_bytes = 6;
        traffic_tap tap(*hub, {place_of("P", port)}, limits);
        follower after(*hub);
        after.send(port, "abc");
        after.send(port, "def");
        after.send(port, "g");
        const auto got = collect(tap, 3);
        REQUIRE_EQ(got.items.size(), std::size_t{2});
        REQUIRE_EQ(got.dropped_items, std::uint64_t{1});
        REQUIRE_EQ(got.dropped_bytes, std::uint64_t{1});
    }
    {
        tap_limits limits;
        limits.max_items = 0;
        traffic_tap tap(*hub, {place_of("P", port)}, limits);
        follower after(*hub);
        after.send(port, "abc");
        const auto got = collect(tap, 1);
        REQUIRE_EQ(got.items.size(), std::size_t{0});
        REQUIRE_EQ(got.dropped_items, std::uint64_t{1});
    }
    {
        traffic_tap tap(*hub, {place_of("P", port)});
        follower after(*hub);
        after.send(port, "abc");
        after.send(port, "def");
        after.send(port, "ghi");
        REQUIRE_EQ(tap.queued_items(), std::size_t{3});
        REQUIRE_EQ(tap.queued_bytes(), std::size_t{9});
        // Two items, and the byte limit stops the second at 4 bytes.
        auto first = tap.take(2, 4);
        REQUIRE_EQ(first.items.size(), std::size_t{1});
        // The first item is returned even when it alone is over the limit.
        auto second = tap.take(10, 1);
        REQUIRE_EQ(second.items.size(), std::size_t{1});
        REQUIRE_EQ(tap.queued_items(), std::size_t{1});
        REQUIRE_EQ(tap.queued_bytes(), std::size_t{3});
        REQUIRE_EQ(tap.take(0, 100).items.size(), std::size_t{0});
        REQUIRE_EQ(tap.queued_items(), std::size_t{1});
    }
}

TEST_CASE("traffic tap: a tap nobody reads never slows the ports")
{
    auto hub = start_hub();
    auto source = add_mock(*hub, "source");
    auto target = add_mock(*hub, "target");
    REQUIRE(hub->connect(source.endpoint->id(), target.endpoint->id()));
    tap_limits limits;
    limits.max_items = 8;
    limits.max_bytes = 80;
    traffic_tap tap(*hub, {place_of("S", source), place_of("T", target)},
                    limits);
    const follower after(*hub);

    constexpr int chunks = 3000;
    for (int i = 0; i < chunks; ++i)
        inject_now(source, "0123456789");

    REQUIRE(wait_until(
        [&] { return target.written().size() == std::size_t{chunks} * 10; },
        20000ms));
    after.wait_bytes(std::size_t{2} * chunks * 10);
    // The ports may join chunks, so the number of items is not fixed; the
    // limits and the totals are.
    REQUIRE(tap.queued_items() >= 1);
    REQUIRE(tap.queued_items() <= 8);
    REQUIRE(tap.queued_bytes() <= 80);
    const std::size_t held = tap.queued_bytes();
    const auto got = tap.take(100, 1000);
    // Each event is at one place; everything beyond the queue is counted.
    REQUIRE_EQ(got.dropped_items + got.items.size(),
               after.recorder.count(event_kind::data));
    REQUIRE_EQ(got.dropped_bytes + held, std::uint64_t{2} * chunks * 10);
}

TEST_CASE("traffic tap: notify runs once per empty to non-empty change, "
          "on the given executor")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");
    boost::asio::thread_pool pool(1);
    std::atomic<std::thread::id> pool_thread{};
    boost::asio::post(pool, [&] { pool_thread = std::this_thread::get_id(); });
    REQUIRE(wait_until([&] { return pool_thread.load() != std::thread::id{}; },
                       5000ms));

    traffic_tap tap(*hub, {place_of("P", port)});
    std::atomic<int> calls{0};
    std::atomic<bool> on_pool{true};
    tap.set_notify(
        [&] {
            if (std::this_thread::get_id() != pool_thread.load())
                on_pool = false;
            ++calls;
        },
        pool.get_executor());

    port.inject_read("a");
    REQUIRE(wait_until([&] { return calls == 1; }, 5000ms));
    port.inject_read("b");
    REQUIRE(wait_until([&] { return tap.queued_items() == 2; }, 5000ms));
    port.inject_read("c");
    REQUIRE(wait_until([&] { return tap.queued_items() == 3; }, 5000ms));
    std::this_thread::sleep_for(100ms);
    REQUIRE_EQ(calls.load(), 1);

    REQUIRE_EQ(tap.take(10, 100).items.size(), std::size_t{3});
    port.inject_read("d");
    REQUIRE(wait_until([&] { return calls == 2; }, 5000ms));
    REQUIRE(on_pool.load());

    // Items already queued when the callback is set are told at once.
    std::atomic<int> late{0};
    tap.set_notify([&] { ++late; }, pool.get_executor());
    REQUIRE(wait_until([&] { return late == 1; }, 5000ms));

    tap.close();
    pool.join();
}

TEST_CASE("traffic tap: closed taps hand out nothing and notify nobody")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");
    boost::asio::thread_pool pool(1);
    traffic_tap tap(*hub, {place_of("P", port)});
    port.inject_read("a");
    REQUIRE(wait_until([&] { return tap.queued_items() == 1; }, 5000ms));
    tap.close();
    tap.close();
    REQUIRE_EQ(tap.queued_items(), std::size_t{0});
    REQUIRE_EQ(tap.queued_bytes(), std::size_t{0});

    std::atomic<int> calls{0};
    tap.set_notify([&] { ++calls; }, pool.get_executor());
    tap.set_places({place_of("P", port)});
    port.inject_read("b");
    std::this_thread::sleep_for(100ms);
    REQUIRE_EQ(tap.take(10, 100).items.size(), std::size_t{0});
    REQUIRE_EQ(calls.load(), 0);
    pool.join();
}

TEST_CASE("traffic tap: closing and destroying while traffic flows")
{
    auto hub = start_hub(4);
    std::vector<mock_endpoint> ports;
    ports.reserve(4);
    for (int i = 0; i < 4; ++i)
        ports.push_back(add_mock(*hub, "port" + std::to_string(i)));

    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    senders.reserve(ports.size());
    for (const auto& port : ports)
    {
        senders.emplace_back([&stop, target = port] {
            while (!stop)
            {
                inject_now(target, "payload");
                std::this_thread::yield();
            }
        });
    }
    boost::asio::thread_pool pool(2);

    for (int round = 0; round < 200; ++round)
    {
        std::vector<tap_place> places;
        places.reserve(ports.size());
        for (std::size_t i = 0; i < ports.size(); ++i)
            places.push_back(place_of("p" + std::to_string(i), ports[i]));
        tap_limits limits;
        limits.max_items = 16;
        auto tap = std::make_unique<traffic_tap>(*hub, std::move(places),
                                                 limits);
        tap->set_notify([] {}, pool.get_executor());
        std::thread reader([&tap] {
            for (int i = 0; i < 20; ++i)
            {
                static_cast<void>(tap->take(8, 1024));
                std::this_thread::yield();
            }
        });
        reader.join();
        if (round % 2 == 0)
            tap->close();
        tap.reset();
    }

    stop = true;
    for (auto& sender : senders)
        sender.join();
    pool.join();
}

TEST_CASE("traffic tap: changing the watched set loses and repeats nothing")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");
    auto idle = add_mock(*hub, "idle");
    tap_limits limits;
    limits.max_items = 1000000;
    limits.max_bytes = std::size_t{1} << 30;
    traffic_tap tap(*hub, {place_of("one", port)}, limits);

    constexpr int chunks = 4000;
    std::string sent;
    std::thread sender([&] {
        for (int i = 0; i < chunks; ++i)
        {
            char text[16];
            std::snprintf(text, sizeof text, "%07d,", i);
            sent += text;
            inject_now(port, text);
        }
    });

    // Rename the place, widen it, narrow it, all while bytes flow.
    for (int i = 0; i < 300; ++i)
    {
        std::vector<tap_place> places;
        switch (i % 3)
        {
        case 0:
            places = {place_of("one", port)};
            break;
        case 1:
            places = {place_of("two", port)};
            break;
        default:
            places = {place_of("three", port), place_of("idle", idle)};
            break;
        }
        tap.set_places(std::move(places));
        std::this_thread::yield();
    }
    sender.join();
    tap.set_places({place_of("last", port)});

    std::string seen;
    wait_until(
        [&] {
            for (auto& item : tap.take(1000000, 1000000000).items)
                seen += text_of(item);
            return seen.size() >= sent.size();
        },
        10000ms);
    REQUIRE_EQ(seen.size(), sent.size());
    REQUIRE(seen == sent);
}

TEST_CASE("traffic tap: losses are marked in the queue where they happened")
{
    auto hub = start_hub();
    auto first = add_mock(*hub, "first");
    auto second = add_mock(*hub, "second");
    tap_limits limits;
    limits.max_items = 2;
    limits.mark_losses = true;
    traffic_tap tap(*hub, {place_of("A", first), place_of("B", second)},
                    limits);
    follower after(*hub);

    after.send(first, "a1");
    after.send(first, "a2");
    // The queue is full: these are dropped, and the loss is one item that
    // names both places.
    after.send(first, "a3");
    after.send(second, "bb4");
    after.send(first, "a5");

    auto got = tap.take(100, 1000);
    REQUIRE_EQ(got.items.size(), std::size_t{3});
    REQUIRE(got.items[0].kind == tap_item_kind::data);
    REQUIRE(got.items[1].kind == tap_item_kind::data);
    REQUIRE(got.items[2].kind == tap_item_kind::loss);
    const auto& losses = got.items[2].losses;
    REQUIRE_EQ(losses.size(), std::size_t{2});
    REQUIRE_EQ(losses[0].place, std::string("A"));
    REQUIRE(losses[0].direction == data_direction::rx);
    REQUIRE_EQ(losses[0].items, std::uint64_t{2});
    REQUIRE_EQ(losses[0].bytes, std::uint64_t{4});
    REQUIRE(losses[0].last >= losses[0].first);
    REQUIRE_EQ(losses[1].place, std::string("B"));
    REQUIRE_EQ(losses[1].items, std::uint64_t{1});
    REQUIRE_EQ(losses[1].bytes, std::uint64_t{3});
    // The totals are still kept as they were.
    REQUIRE_EQ(got.dropped_items, std::uint64_t{3});
    REQUIRE_EQ(got.dropped_bytes, std::uint64_t{7});

    // Data that comes when there is room again follows the loss.
    after.send(first, "a6");
    got = tap.take(100, 1000);
    REQUIRE_EQ(got.items.size(), std::size_t{1});
    REQUIRE_EQ(text_of(got.items[0]), std::string("a6"));
}

TEST_CASE("traffic tap: without mark_losses a drop leaves no item")
{
    auto hub = start_hub();
    auto port = add_mock(*hub, "port");
    tap_limits limits;
    limits.max_items = 1;
    traffic_tap tap(*hub, {place_of("P", port)}, limits);
    follower after(*hub);
    after.send(port, "one");
    after.send(port, "two");
    const auto got = tap.take(100, 1000);
    REQUIRE_EQ(got.items.size(), std::size_t{1});
    REQUIRE(got.items[0].kind == tap_item_kind::data);
    REQUIRE_EQ(got.dropped_items, std::uint64_t{1});
}
