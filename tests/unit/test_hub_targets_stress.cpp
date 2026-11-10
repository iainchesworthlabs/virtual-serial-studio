//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// The routing table changed under traffic: publishers send through a duplex
// split while another thread adds and removes targets, changes the owner and
// removes endpoints.  The ThreadSanitizer leg is what gives this its teeth.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

/// One message is four bytes: who sent it, then its number in three digits
/// of base 64, so a receiver can tell the order and the sender of each.
constexpr std::size_t record_size = 4;
constexpr int last_number = 64 * 64 * 64 - 1;

std::string record(char sender, int number)
{
    std::string out(record_size, sender);
    out[1] = static_cast<char>('0' + ((number >> 12) & 63));
    out[2] = static_cast<char>('0' + ((number >> 6) & 63));
    out[3] = static_cast<char>('0' + (number & 63));
    return out;
}

/// The numbers `heard` holds for each sender, in the order they arrived.
/// Fails the test if a message was torn.
std::map<char, std::vector<int>> by_sender(const std::string& heard)
{
    REQUIRE_EQ(heard.size() % record_size, std::size_t{0});
    std::map<char, std::vector<int>> out;
    for (std::size_t at = 0; at < heard.size(); at += record_size)
    {
        const int number = ((heard[at + 1] - '0') << 12) |
                           ((heard[at + 2] - '0') << 6) | (heard[at + 3] - '0');
        REQUIRE(number >= 0 && number <= last_number);
        out[heard[at]].push_back(number);
    }
    return out;
}

/// In order, nothing twice.
bool strictly_rising(const std::vector<int>& numbers)
{
    for (std::size_t at = 1; at < numbers.size(); ++at)
    {
        if (numbers[at] <= numbers[at - 1])
            return false;
    }
    return true;
}

/// Every number from zero to `count - 1`, in order, and then the last one.
bool complete(const std::vector<int>& numbers, int count)
{
    if (numbers.size() != static_cast<std::size_t>(count) + 1)
        return false;
    for (int at = 0; at < count; ++at)
    {
        if (numbers[static_cast<std::size_t>(at)] != at)
            return false;
    }
    return numbers.back() == last_number;
}

/// True once `port` has been written the last message of `sender`.
bool has_last(const mock_endpoint& port, char sender)
{
    const auto heard = by_sender(port.written());
    const auto found = heard.find(sender);
    return found != heard.end() && found->second.back() == last_number;
}

void churn(const link_options& options, int messages)
{
    auto hub = start_hub(4);
    const auto source = add_mock(*hub, "source");
    // Two targets that stay, one that comes and goes and is on the link at
    // the end, and one that comes and goes and is off it at the end.
    const auto stay_1 = add_mock(*hub, "stay-1");
    const auto stay_2 = add_mock(*hub, "stay-2");
    const auto comes = add_mock(*hub, "comes");
    const auto goes = add_mock(*hub, "goes");
    // A second link, so that removing an endpoint has a link of its own to
    // take down next to the split.
    const auto tap = add_mock(*hub, "tap");

    const auto link =
        hub->split(source.endpoint->id(),
                   {stay_1.endpoint->id(), stay_2.endpoint->id()}, options);
    REQUIRE(link);

    std::atomic<bool> done{false};
    std::atomic<int> changes{0};

    std::thread mutator(
        [&]
        {
            int round = 0;
            while (!done.load())
            {
                ++round;
                const auto first = hub->add_target(*link, comes.endpoint->id());
                const auto second = hub->add_target(*link, goes.endpoint->id());
                REQUIRE(first);
                REQUIRE(second);

                // The owner moves between the targets, and to none.
                REQUIRE(hub->set_owner(*link, round % 2 == 0
                                                  ? stay_1.endpoint->id()
                                                  : comes.endpoint->id()));

                // An endpoint is made, linked twice and removed again.
                const std::string name = "short-" + std::to_string(round);
                const auto brief = add_mock(*hub, name);
                REQUIRE(hub->add_target(*link, brief.endpoint->id()));
                REQUIRE(
                    hub->redirect(brief.endpoint->id(), tap.endpoint->id()));
                REQUIRE_EQ(hub->find_link(*link)->targets.size(),
                           std::size_t{5});
                REQUIRE(hub->remove_target(*link, brief.endpoint->id()));
                REQUIRE(hub->remove_endpoint(brief.endpoint->id()));

                REQUIRE(hub->release_owner(*link));
                REQUIRE(hub->remove_target(*link, goes.endpoint->id()));
                REQUIRE(hub->remove_target(*link, comes.endpoint->id()));
                REQUIRE_EQ(hub->links().size(), std::size_t{1});
                changes.fetch_add(1);
                std::this_thread::yield();
            }
        });

    // Everyone sends at once: the source to its targets, and every target
    // (on the link or not) back to the source.
    const std::vector<std::pair<const mock_endpoint*, char>> senders{
        {&source, 'S'},
        {&stay_1, '1'},
        {&stay_2, '2'},
        {&comes, 'C'},
        {&goes, 'G'}};
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

    // The table must have changed while the bytes moved, or nothing was
    // tested.  Let the mutator catch up if the publishers were too quick.
    REQUIRE(wait_until([&] { return changes.load() >= 3; }, 60s));
    done.store(true);
    mutator.join();

    // Settle: one target is added for good, and a last message each way
    // tells the receivers that everything before it has arrived.
    REQUIRE(hub->add_target(*link, comes.endpoint->id()));
    REQUIRE(hub->set_owner(*link, stay_1.endpoint->id()));
    const bool owned = options.policy == merge_policy::owner;
    source.inject_read(record('S', last_number));
    stay_1.inject_read(record('1', last_number));
    REQUIRE(wait_until([&] { return has_last(stay_1, 'S'); }, 60s));
    REQUIRE(wait_until([&] { return has_last(stay_2, 'S'); }, 60s));
    REQUIRE(wait_until([&] { return has_last(comes, 'S'); }, 60s));
    REQUIRE(wait_until([&] { return has_last(source, '1'); }, 60s));
    if (!owned)
    {
        stay_2.inject_read(record('2', last_number));
        REQUIRE(wait_until([&] { return has_last(source, '2'); }, 60s));
    }

    // A target that was never removed missed nothing.
    for (const mock_endpoint* port : {&stay_1, &stay_2})
    {
        const auto heard = by_sender(port->written());
        REQUIRE_EQ(heard.size(), std::size_t{1});
        REQUIRE(complete(heard.at('S'), messages));
    }

    // A target that came and went heard a part of it, in order, once.
    for (const mock_endpoint* port : {&comes, &goes})
    {
        const auto heard = by_sender(port->written());
        for (const auto& [sender, numbers] : heard)
        {
            REQUIRE_EQ(sender, 'S');
            REQUIRE(strictly_rising(numbers));
        }
    }

    // The source heard each target in order.  Under the owner policy that
    // is whoever owned the link at the time; without it, a target that was
    // never removed got every reply through.
    const auto replies = by_sender(source.written());
    for (const auto& [sender, numbers] : replies)
    {
        REQUIRE(sender != 'S');
        REQUIRE(strictly_rising(numbers));
    }
    if (!owned)
    {
        REQUIRE(complete(replies.at('1'), messages));
        REQUIRE(complete(replies.at('2'), messages));
    }

    // The link is the one that was made, with what it should have now.
    const auto info = hub->find_link(*link);
    REQUIRE(info);
    REQUIRE_EQ(info->targets.size(), std::size_t{3});
    REQUIRE_EQ(info->owner, stay_1.endpoint->id());
    REQUIRE_EQ(tap.written(), std::string());
}
} // namespace

TEST_CASE("targets: a split changed under traffic loses and repeats nothing")
{
    link_options options;
    options.policy = merge_policy::owner;
    churn(options, 300);
}

TEST_CASE("targets: an interleaving split changed under traffic")
{
    churn(link_options{}, 300);
}

TEST_CASE("targets: a delayed split changed under traffic keeps its order")
{
    link_options options;
    options.delay_ms = 2;
    options.jitter_ms = 2;
    churn(options, 120);
}
