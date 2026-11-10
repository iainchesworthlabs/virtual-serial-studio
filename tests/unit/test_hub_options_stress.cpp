//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// The options of links changed under traffic: publishers send numbered records
// through a split, a join and a bus while another thread flips the merge
// policy, the delay, the jitter and the modem lines of each, moves the owner
// and adds and removes a target.  Bytes may be lost at a change, but never
// reordered or repeated.  The ThreadSanitizer leg is what gives this its teeth.

#include "support/records.hpp"
#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
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

const auto unsupported = make_error_code(errc::unsupported);

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

/// What a receiver may have heard: the tags it can hear, each in order and
/// once.
void ordered(const mock_endpoint& port, const std::string& tags)
{
    for (const auto& [sender, numbers] : by_sender(port.written()))
    {
        REQUIRE(tags.find(sender) != std::string::npos);
        REQUIRE(strictly_rising(numbers));
    }
}

/// The options of round `round`: the delay, the jitter, the policy and the
/// lines all move, each at its own pace.
link_options options_of(int round, int phase, bool owner_allowed)
{
    static constexpr std::array<std::uint32_t, 4> delays{0, 2, 0, 3};
    link_options options;
    options.delay_ms = delays[static_cast<std::size_t>(round + phase) % 4];
    options.jitter_ms = options.delay_ms / 2;
    options.policy = owner_allowed && ((round + phase) / 2) % 2 == 1
                         ? merge_policy::owner
                         : merge_policy::interleave;
    options.emulate_modem_lines = ((round + phase) / 3) % 2 == 0;
    if ((round + phase) % 5 == 0)
    {
        options.wiring.dtr_to_cts = true;
        options.wiring.rts_to_cts = false;
    }
    return options;
}

void flip_options(int messages)
{
    auto hub = start_hub(4);
    // The split's source and targets; the third target comes and goes.
    const auto s = add_mock(*hub, "s");
    const auto t1 = add_mock(*hub, "t1");
    const auto t2 = add_mock(*hub, "t2");
    const auto t3 = add_mock(*hub, "t3");
    // The join's sources and target.
    const auto j1 = add_mock(*hub, "j1");
    const auto j2 = add_mock(*hub, "j2");
    const auto jt = add_mock(*hub, "jt");
    // The bus.
    const auto m1 = add_mock(*hub, "m1");
    const auto m2 = add_mock(*hub, "m2");
    const auto m3 = add_mock(*hub, "m3");

    const auto split =
        hub->split(s.endpoint->id(), {t1.endpoint->id(), t2.endpoint->id()});
    const auto join =
        hub->join({j1.endpoint->id(), j2.endpoint->id()}, jt.endpoint->id());
    const auto bus =
        hub->bus({m1.endpoint->id(), m2.endpoint->id(), m3.endpoint->id()});
    REQUIRE(split);
    REQUIRE(join);
    REQUIRE(bus);

    std::atomic<bool> done{false};
    std::atomic<int> changes{0};
    // A failed requirement in the mutator ends it and fails the test, rather
    // than escaping its thread.
    std::mutex failure_mutex;
    std::string failure;

    std::thread mutator(
        [&]
        {
            try
            {
                int round = 0;
                while (!done.load())
                {
                    ++round;
                    REQUIRE(hub->set_link_options(*split,
                                                  options_of(round, 0, true)));
                    REQUIRE(hub->set_link_options(*join,
                                                  options_of(round, 1, true)));
                    REQUIRE(hub->set_link_options(*bus,
                                                  options_of(round, 2, false)));

                    // The bus takes interleave only, and a refusal changes
                    // nothing.
                    link_options owner = options_of(round, 3, false);
                    owner.policy = merge_policy::owner;
                    REQUIRE_EQ(hub->set_link_options(*bus, owner), unsupported);
                    REQUIRE(hub->find_link(*bus)->options.policy ==
                            merge_policy::interleave);

                    // The owner moves between the candidates, and to none.
                    REQUIRE(hub->set_owner(*split, round % 2 == 0
                                                       ? t1.endpoint->id()
                                                       : t2.endpoint->id()));
                    REQUIRE(hub->set_owner(*join, round % 2 == 0
                                                      ? j1.endpoint->id()
                                                      : j2.endpoint->id()));
                    if (round % 3 == 0)
                    {
                        REQUIRE(hub->release_owner(*split));
                        REQUIRE(hub->release_owner(*join));
                    }

                    // A target comes and goes while the options change.
                    REQUIRE(hub->add_target(*split, t3.endpoint->id()));
                    REQUIRE(hub->set_link_options(*split,
                                                  options_of(round, 2, true)));
                    REQUIRE(hub->remove_target(*split, t3.endpoint->id()));

                    // Leaving the owner policy leaves no owner behind.
                    link_options owned;
                    owned.policy = merge_policy::owner;
                    REQUIRE(hub->set_link_options(*join, owned));
                    REQUIRE(hub->set_owner(*join, j2.endpoint->id()));
                    REQUIRE(hub->set_link_options(*join, link_options{}));
                    REQUIRE_EQ(hub->find_link(*join)->owner, endpoint_id{0});

                    REQUIRE_EQ(hub->links().size(), std::size_t{3});
                    changes.fetch_add(1);
                    std::this_thread::yield();
                }
            }
            catch (const std::exception& error)
            {
                std::lock_guard lock(failure_mutex);
                failure = error.what();
            }
        });

    // Everyone sends at once, on a link or not.
    publish({{&s, 'S'},
             {&t1, '1'},
             {&t2, '2'},
             {&t3, '3'},
             {&j1, 'J'},
             {&j2, 'K'},
             {&m1, 'a'},
             {&m2, 'b'},
             {&m3, 'c'}},
            messages);

    // The options must have changed while the bytes moved, or nothing was
    // tested.  Let the mutator catch up if the publishers were too quick.
    const auto stopped = [&]
    {
        std::lock_guard lock(failure_mutex);
        return !failure.empty();
    };
    const bool ran =
        wait_until([&] { return changes.load() >= 3 || stopped(); }, 60s);
    done.store(true);
    mutator.join();
    if (!failure.empty())
        std::fprintf(stderr, "mutator: %s\n", failure.c_str());
    REQUIRE(failure.empty());
    REQUIRE(ran);

    // Settle: every link is undelayed and interleaves, and a last message
    // from each sender tells its receivers that everything before it has
    // arrived.  It travels on a link that has been changed many times.
    const link_options calm;
    REQUIRE(hub->set_link_options(*split, calm));
    REQUIRE(hub->set_link_options(*join, calm));
    REQUIRE(hub->set_link_options(*bus, calm));
    REQUIRE_EQ(hub->find_link(*split)->targets.size(), std::size_t{2});

    s.inject_read(record('S', last_number));
    t1.inject_read(record('1', last_number));
    t2.inject_read(record('2', last_number));
    j1.inject_read(record('J', last_number));
    j2.inject_read(record('K', last_number));
    m1.inject_read(record('a', last_number));
    m2.inject_read(record('b', last_number));
    m3.inject_read(record('c', last_number));

    for (const mock_endpoint* port : {&t1, &t2})
        REQUIRE(wait_until([&] { return has_last(*port, 'S'); }, 60s));
    for (const char tag : {'1', '2'})
        REQUIRE(wait_until([&] { return has_last(s, tag); }, 60s));
    for (const char tag : {'J', 'K'})
        REQUIRE(wait_until([&] { return has_last(jt, tag); }, 60s));
    struct member
    {
        const mock_endpoint* port;
        std::string others;
    };
    for (const auto& [port, others] :
         {member{&m1, "bc"}, member{&m2, "ac"}, member{&m3, "ab"}})
    {
        for (const char tag : others)
            REQUIRE(wait_until([&] { return has_last(*port, tag); }, 60s));
    }

    // Every receiver heard each sender it can hear in order, once, and
    // nobody heard a sender it is not linked to.
    ordered(t1, "S");
    ordered(t2, "S");
    ordered(t3, "S");
    ordered(s, "123");
    ordered(jt, "JK");
    ordered(m1, "bc");
    ordered(m2, "ac");
    ordered(m3, "ab");

    // One-way: nothing went back to the sources of the join.
    REQUIRE_EQ(j1.written(), std::string());
    REQUIRE_EQ(j2.written(), std::string());

    // The links are the ones that were made, with what they should have now.
    const auto after = hub->find_link(*split);
    REQUIRE(after);
    REQUIRE_EQ(after->id, *split);
    REQUIRE_EQ(after->sources, std::vector<endpoint_id>{s.endpoint->id()});
    REQUIRE_EQ(hub->find_link(*join)->sources.size(), std::size_t{2});
    REQUIRE_EQ(hub->find_link(*bus)->sources.size(), std::size_t{3});
}
} // namespace

TEST_CASE("options: links changed under traffic lose, reorder and repeat "
          "nothing")
{
    flip_options(200);
}
