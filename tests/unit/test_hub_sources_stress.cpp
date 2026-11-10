//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// The routing table changed under traffic: publishers send through a join and
// through a bus while another thread adds and removes sources and members,
// moves the owner and makes and removes endpoints.  The ThreadSanitizer leg is
// what gives this its teeth.

#include "support/records.hpp"
#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;
using namespace vss_test_support::records;

namespace
{
using namespace std::chrono_literals;

const auto not_found = make_error_code(errc::not_found);
const auto invalid_argument = make_error_code(errc::invalid_argument);
const auto already_exists = make_error_code(errc::already_exists);
const auto cycle_detected = make_error_code(errc::cycle_detected);

/// Sends `messages` numbered records from each port, one thread each.
void publish(const std::vector<std::pair<const mock_endpoint*, char>>& senders,
             int messages)
{
    std::vector<std::thread> publishers;
    publishers.reserve(senders.size());
    for (const auto& [port, tag] : senders)
    {
        publishers.emplace_back(
            [port, tag, messages]
            {
                for (int number = 0; number < messages; ++number)
                    port->inject_read(record(tag, number));
            });
    }
    for (auto& thread : publishers)
        thread.join();
}

void join_churn(const link_options& options, int messages)
{
    auto hub = start_hub(4);
    // Two sources that stay, one that comes and goes and is on the link at
    // the end, and one that comes and goes and is off it at the end.
    const auto stay_1 = add_mock(*hub, "stay-1");
    const auto stay_2 = add_mock(*hub, "stay-2");
    const auto comes = add_mock(*hub, "comes");
    const auto goes = add_mock(*hub, "goes");
    const auto target = add_mock(*hub, "target");
    // A receiver of a route that is never removed, on a link of its own.
    const auto witness = add_mock(*hub, "witness");
    // Receives from the short-lived endpoints, which never send.
    const auto tap = add_mock(*hub, "tap");
    // The target sends to it, so it cannot be a source of the link.
    const auto back = add_mock(*hub, "back");

    const auto link = hub->join({stay_1.endpoint->id(), stay_2.endpoint->id()},
                                target.endpoint->id(), options);
    REQUIRE(link);
    REQUIRE(hub->redirect(stay_1.endpoint->id(), witness.endpoint->id()));
    REQUIRE(hub->redirect(target.endpoint->id(), back.endpoint->id()));

    std::atomic<bool> done{false};
    std::atomic<int> changes{0};

    std::thread mutator(
        [&]
        {
            int round = 0;
            while (!done.load())
            {
                ++round;
                const auto first = hub->add_source(*link, comes.endpoint->id());
                const auto second = hub->add_source(*link, goes.endpoint->id());
                REQUIRE(first);
                REQUIRE(second);
                REQUIRE_EQ(hub->add_source(*link, comes.endpoint->id()),
                           already_exists);
                REQUIRE_EQ(hub->add_source(*link, target.endpoint->id()),
                           invalid_argument);
                REQUIRE_EQ(hub->add_source(*link, back.endpoint->id()),
                           cycle_detected);

                // The owner moves between the sources, and to none.
                REQUIRE(hub->set_owner(*link, round % 2 == 0
                                                  ? stay_1.endpoint->id()
                                                  : comes.endpoint->id()));

                // An endpoint is made, linked twice and removed again.
                const std::string name = "short-" + std::to_string(round);
                const auto brief = add_mock(*hub, name);
                REQUIRE(hub->add_source(*link, brief.endpoint->id()));
                REQUIRE(
                    hub->redirect(brief.endpoint->id(), tap.endpoint->id()));
                REQUIRE_EQ(hub->find_link(*link)->sources.size(),
                           std::size_t{5});
                REQUIRE(hub->remove_source(*link, brief.endpoint->id()));
                REQUIRE(hub->remove_endpoint(brief.endpoint->id()));

                REQUIRE(hub->release_owner(*link));
                REQUIRE(hub->remove_source(*link, goes.endpoint->id()));
                REQUIRE_EQ(hub->remove_source(*link, goes.endpoint->id()),
                           not_found);

                // A source that owns the link cannot own it once it is off
                // it: nothing it sends can take the link again.
                REQUIRE(hub->set_owner(*link, comes.endpoint->id()));
                REQUIRE(hub->remove_source(*link, comes.endpoint->id()));
                REQUIRE(hub->find_link(*link)->owner != comes.endpoint->id());

                REQUIRE_EQ(hub->links().size(), std::size_t{3});
                changes.fetch_add(1);
                std::this_thread::yield();
            }
        });

    // Everyone sends at once, on the link or not.
    publish({{&stay_1, 'A'}, {&stay_2, 'B'}, {&comes, 'C'}, {&goes, 'G'}},
            messages);

    // The table must have changed while the bytes moved, or nothing was
    // tested.  Let the mutator catch up if the publishers were too quick.
    REQUIRE(wait_until([&] { return changes.load() >= 3; }, 60s));
    done.store(true);
    mutator.join();

    // Settle: one source is added for good, and a last message from each
    // source that stayed tells the receivers that everything before it has
    // arrived.
    REQUIRE(hub->add_source(*link, comes.endpoint->id()));
    REQUIRE(hub->set_owner(*link, stay_1.endpoint->id()));
    const bool owned = options.policy == merge_policy::owner;
    stay_1.inject_read(record('A', last_number));
    REQUIRE(wait_until([&] { return has_last(target, 'A'); }, 60s));
    REQUIRE(wait_until([&] { return has_last(witness, 'A'); }, 60s));
    if (!owned)
    {
        stay_2.inject_read(record('B', last_number));
        REQUIRE(wait_until([&] { return has_last(target, 'B'); }, 60s));
    }

    // A receiver fed by a route that was never removed missed nothing.
    const auto seen = by_sender(witness.written());
    REQUIRE_EQ(seen.size(), std::size_t{1});
    REQUIRE(complete(seen.at('A'), messages));

    // The target heard every source in order, once.  Without the owner policy
    // a source that was never removed got everything through.
    const auto heard = by_sender(target.written());
    for (const auto& [sender, numbers] : heard)
    {
        REQUIRE(sender == 'A' || sender == 'B' || sender == 'C' ||
                sender == 'G');
        REQUIRE(strictly_rising(numbers));
    }
    if (!owned)
    {
        REQUIRE(complete(heard.at('A'), messages));
        REQUIRE(complete(heard.at('B'), messages));
    }

    // One-way: nothing went back, and the short-lived endpoints sent nothing.
    for (const mock_endpoint* port :
         {&stay_1, &stay_2, &comes, &goes, &tap, &back})
        REQUIRE_EQ(port->written(), std::string());

    // The link is the one that was made, with what it should have now.
    const auto info = hub->find_link(*link);
    REQUIRE(info);
    REQUIRE_EQ(info->sources, (std::vector<endpoint_id>{stay_1.endpoint->id(),
                                                        stay_2.endpoint->id(),
                                                        comes.endpoint->id()}));
    REQUIRE_EQ(info->targets, std::vector<endpoint_id>{target.endpoint->id()});
    REQUIRE_EQ(info->owner, stay_1.endpoint->id());
    if (!owned)
        REQUIRE_EQ(info->stats.dropped, std::uint64_t{0});
}

void bus_churn(const link_options& options, int messages)
{
    auto hub = start_hub(4);
    const auto stay_1 = add_mock(*hub, "stay-1");
    const auto stay_2 = add_mock(*hub, "stay-2");
    const auto stay_3 = add_mock(*hub, "stay-3");
    // One that comes and goes and is on the bus at the end, and one that
    // comes and goes and is off it at the end.
    const auto comes = add_mock(*hub, "comes");
    const auto goes = add_mock(*hub, "goes");
    // Hears each of the stayers once, through a join of its own.
    const auto tap = add_mock(*hub, "tap");
    // Sends to a member, so it cannot join the bus.
    const auto loopy = add_mock(*hub, "loopy");

    const std::vector<endpoint_id> stayers{
        stay_1.endpoint->id(), stay_2.endpoint->id(), stay_3.endpoint->id()};
    const auto bus = hub->bus(stayers, options);
    REQUIRE(bus);
    REQUIRE(hub->join(stayers, tap.endpoint->id()));
    REQUIRE(hub->redirect(loopy.endpoint->id(), stay_1.endpoint->id()));

    std::atomic<bool> done{false};
    std::atomic<int> changes{0};

    std::thread mutator(
        [&]
        {
            int round = 0;
            while (!done.load())
            {
                ++round;
                const auto first = hub->add_member(*bus, comes.endpoint->id());
                const auto second = hub->add_member(*bus, goes.endpoint->id());
                REQUIRE(first);
                REQUIRE(second);
                REQUIRE_EQ(hub->add_member(*bus, comes.endpoint->id()),
                           already_exists);
                REQUIRE_EQ(hub->add_member(*bus, loopy.endpoint->id()),
                           cycle_detected);

                // An endpoint is made and put on the bus, and goes either by
                // leaving it or with the hub's removal of the endpoint.
                const std::string name = "short-" + std::to_string(round);
                const auto brief = add_mock(*hub, name);
                REQUIRE(hub->add_member(*bus, brief.endpoint->id()));
                REQUIRE_EQ(hub->find_link(*bus)->sources.size(),
                           std::size_t{6});
                if (round % 2 == 0)
                    REQUIRE(hub->remove_member(*bus, brief.endpoint->id()));
                REQUIRE(hub->remove_endpoint(brief.endpoint->id()));

                REQUIRE(hub->remove_member(*bus, goes.endpoint->id()));
                REQUIRE_EQ(hub->remove_member(*bus, goes.endpoint->id()),
                           not_found);
                REQUIRE(hub->remove_member(*bus, comes.endpoint->id()));
                REQUIRE_EQ(hub->find_link(*bus)->sources, stayers);
                REQUIRE_EQ(hub->links().size(), std::size_t{3});
                changes.fetch_add(1);
                std::this_thread::yield();
            }
        });

    publish({{&stay_1, 'a'},
             {&stay_2, 'b'},
             {&stay_3, 'c'},
             {&comes, 'C'},
             {&goes, 'G'}},
            messages);

    REQUIRE(wait_until([&] { return changes.load() >= 3; }, 60s));
    done.store(true);
    mutator.join();

    // Settle: one member is added for good, and a last message from each
    // stayer tells the receivers that everything before it has arrived.
    REQUIRE(hub->add_member(*bus, comes.endpoint->id()));
    stay_1.inject_read(record('a', last_number));
    stay_2.inject_read(record('b', last_number));
    stay_3.inject_read(record('c', last_number));
    struct listener
    {
        const mock_endpoint* port;
        char own;
        std::string others;
    };
    const std::vector<listener> hears{{&stay_1, 'a', "bc"},
                                      {&stay_2, 'b', "ac"},
                                      {&stay_3, 'c', "ab"},
                                      {&comes, 'C', "abc"}};
    for (const auto& [port, own, tags] : hears)
    {
        for (const char tag : tags)
            REQUIRE(wait_until([&] { return has_last(*port, tag); }, 60s));
    }
    for (const char tag : {'a', 'b', 'c'})
        REQUIRE(wait_until([&] { return has_last(tap, tag); }, 60s));

    // A member that was never removed missed nothing from the others that
    // never were, and heard nothing twice and nothing of its own.
    for (const auto& [port, own, tags] : hears)
    {
        const auto heard = by_sender(port->written());
        for (const auto& [sender, numbers] : heard)
        {
            REQUIRE(strictly_rising(numbers));
            REQUIRE(sender != own);
        }
        if (port != &comes)
        {
            for (const char tag : tags)
                REQUIRE(complete(heard.at(tag), messages));
        }
    }

    // A receiver beside the bus heard each stayer once.
    const auto beside = by_sender(tap.written());
    REQUIRE_EQ(beside.size(), std::size_t{3});
    for (const char tag : {'a', 'b', 'c'})
        REQUIRE(complete(beside.at(tag), messages));

    // The goer is off the bus and heard only while it was on.
    for (const auto& [sender, numbers] : by_sender(goes.written()))
    {
        REQUIRE(sender != 'G');
        REQUIRE(strictly_rising(numbers));
    }
    REQUIRE_EQ(loopy.written(), std::string());

    // The bus is the one that was made, with what it should have now.
    const auto info = hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE_EQ(info->mode, link_mode::bus);
    REQUIRE_EQ(info->sources.size(), std::size_t{4});
    REQUIRE_EQ(info->stats.dropped, std::uint64_t{0});
}
} // namespace

TEST_CASE("sources: a join changed under traffic loses and repeats nothing")
{
    link_options options;
    options.policy = merge_policy::owner;
    join_churn(options, 300);
}

TEST_CASE("sources: an interleaving join changed under traffic")
{
    join_churn(link_options{}, 300);
}

TEST_CASE("sources: a delayed join changed under traffic keeps its order")
{
    link_options options;
    options.delay_ms = 2;
    options.jitter_ms = 2;
    join_churn(options, 120);
}

TEST_CASE("members: a bus changed under traffic loses and repeats nothing")
{
    bus_churn(link_options{}, 300);
}

TEST_CASE("members: a delayed bus changed under traffic keeps its order")
{
    link_options options;
    options.delay_ms = 2;
    options.jitter_ms = 2;
    bus_churn(options, 120);
}
