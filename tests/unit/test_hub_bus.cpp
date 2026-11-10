//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

struct bus_fixture
{
    std::shared_ptr<vss::hub> hub = start_hub();
    std::vector<mock_endpoint> ports;

    explicit bus_fixture(std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
            ports.push_back(add_mock(*hub, "p" + std::to_string(index)));
    }

    endpoint_id id(std::size_t index) const
    {
        return ports[index].endpoint->id();
    }

    std::vector<endpoint_id> ids(std::size_t first, std::size_t last) const
    {
        std::vector<endpoint_id> out;
        for (std::size_t index = first; index < last; ++index)
            out.push_back(id(index));
        return out;
    }

    /// Waits until the endpoint has written `count` bytes in all, to the
    /// device behind it.
    bool wait_written(std::size_t index, std::size_t count) const
    {
        return wait_until(
            [&] { return ports[index].written().size() == count; }, 5000ms);
    }
};

std::string sorted(std::string text)
{
    std::sort(text.begin(), text.end());
    return text;
}

} // namespace

TEST_CASE("bus: three members hear each other and never themselves")
{
    bus_fixture fixture(3);
    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);

    const auto info = fixture.hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE_EQ(info->mode, link_mode::bus);
    REQUIRE_EQ(info->sources.size(), std::size_t{3});
    REQUIRE_EQ(info->targets.size(), std::size_t{0});
    REQUIRE(!info->options.emulate_modem_lines);

    fixture.ports[0].inject_read("A");
    REQUIRE(
        wait_until([&] { return fixture.ports[1].written() == "A"; }, 5000ms));
    REQUIRE(
        wait_until([&] { return fixture.ports[2].written() == "A"; }, 5000ms));

    fixture.ports[1].inject_read("B");
    REQUIRE(
        wait_until([&] { return fixture.ports[0].written() == "B"; }, 5000ms));
    REQUIRE(
        wait_until([&] { return fixture.ports[2].written() == "AB"; }, 5000ms));

    fixture.ports[2].inject_read("C");
    REQUIRE(
        wait_until([&] { return fixture.ports[0].written() == "BC"; }, 5000ms));
    REQUIRE(
        wait_until([&] { return fixture.ports[1].written() == "AC"; }, 5000ms));

    // Nothing came back to a sender: each port holds only what the other two
    // said.
    REQUIRE_EQ(fixture.ports[0].written(), std::string("BC"));
    REQUIRE_EQ(fixture.ports[1].written(), std::string("AC"));
    REQUIRE_EQ(fixture.ports[2].written(), std::string("AB"));
}

TEST_CASE("bus: five members each hear the other four exactly once")
{
    bus_fixture fixture(5);
    REQUIRE(fixture.hub->bus(fixture.ids(0, 5)));

    for (std::size_t index = 0; index < 5; ++index)
        fixture.ports[index].inject_read(std::string(1, char('a' + index)));

    for (std::size_t index = 0; index < 5; ++index)
    {
        REQUIRE(fixture.wait_written(index, 4));
        std::string expected;
        for (std::size_t other = 0; other < 5; ++other)
        {
            if (other != index)
                expected.push_back(char('a' + other));
        }
        REQUIRE_EQ(sorted(fixture.ports[index].written()), expected);
    }
}

TEST_CASE("bus: one sender's bytes arrive in order")
{
    bus_fixture fixture(3);
    REQUIRE(fixture.hub->bus(fixture.ids(0, 3)));

    std::string sent;
    for (int index = 0; index < 100; ++index)
    {
        const std::string chunk = std::to_string(index) + ",";
        sent += chunk;
        fixture.ports[0].inject_read(chunk);
    }

    REQUIRE(fixture.wait_written(1, sent.size()));
    REQUIRE(fixture.wait_written(2, sent.size()));
    REQUIRE_EQ(fixture.ports[1].written(), sent);
    REQUIRE_EQ(fixture.ports[2].written(), sent);
    REQUIRE_EQ(fixture.ports[0].written(), std::string());
}

TEST_CASE("bus: a tap that joins from the members hears each byte once")
{
    bus_fixture fixture(4);
    // p0 to p2 are the bus; p3 listens only.
    REQUIRE(fixture.hub->bus(fixture.ids(0, 3)));
    REQUIRE(fixture.hub->join(fixture.ids(0, 3), fixture.id(3)));

    fixture.ports[0].inject_read("x");
    fixture.ports[1].inject_read("y");
    fixture.ports[2].inject_read("z");

    REQUIRE(fixture.wait_written(3, 3));
    REQUIRE_EQ(sorted(fixture.ports[3].written()), std::string("xyz"));

    // What the tap's own device says goes nowhere: the join is one way and the
    // tap is not on the bus.
    fixture.ports[3].inject_read("silent");
    REQUIRE(wait_until(
        [&] { return fixture.ports[3].endpoint->stats().bytes_rx == 6; },
        5000ms));
    for (std::size_t index = 0; index < 3; ++index)
    {
        REQUIRE(fixture.wait_written(index, 2));
        REQUIRE(fixture.ports[index].written().find("silent") ==
                std::string::npos);
    }
}

TEST_CASE("bus: no route may loop through a bus")
{
    bus_fixture fixture(6);
    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);

    // A route out of the bus is fine.
    REQUIRE(fixture.hub->redirect(fixture.id(0), fixture.id(3)));
    // A route back into it from what it feeds is a loop, wherever it lands.
    REQUIRE_EQ(fixture.hub->redirect(fixture.id(3), fixture.id(1)),
               make_error_code(errc::cycle_detected));
    REQUIRE_EQ(fixture.hub->connect(fixture.id(1), fixture.id(3)),
               make_error_code(errc::cycle_detected));

    // The path may be longer than one hop.
    REQUIRE(fixture.hub->redirect(fixture.id(3), fixture.id(4)));
    REQUIRE_EQ(fixture.hub->redirect(fixture.id(4), fixture.id(2)),
               make_error_code(errc::cycle_detected));

    // A second route between two members would deliver twice.
    REQUIRE_EQ(fixture.hub->redirect(fixture.id(0), fixture.id(1)),
               make_error_code(errc::cycle_detected));
    REQUIRE_EQ(fixture.hub->connect(fixture.id(1), fixture.id(2)),
               make_error_code(errc::cycle_detected));

    // A route into the bus from a port that nothing feeds is fine.
    REQUIRE(fixture.hub->redirect(fixture.id(5), fixture.id(2)));
}

TEST_CASE("bus: a bus cannot be made over a route that already loops")
{
    bus_fixture fixture(5);

    // Two members already joined by a route.
    REQUIRE(fixture.hub->redirect(fixture.id(0), fixture.id(1)));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(0), fixture.id(1)}),
               make_error_code(errc::cycle_detected));

    // Two members joined through a third port.
    REQUIRE(fixture.hub->redirect(fixture.id(2), fixture.id(3)));
    REQUIRE(fixture.hub->redirect(fixture.id(3), fixture.id(4)));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(2), fixture.id(4)}),
               make_error_code(errc::cycle_detected));

    // The ports that stay clear of those routes can still share a bus.
    REQUIRE(fixture.hub->bus({fixture.id(0), fixture.id(3)}));
}

TEST_CASE("bus: two buses chained one way do not close a ring")
{
    bus_fixture fixture(4);
    REQUIRE(fixture.hub->bus({fixture.id(0), fixture.id(1)}));
    REQUIRE(fixture.hub->bus({fixture.id(2), fixture.id(3)}));
    REQUIRE(fixture.hub->redirect(fixture.id(1), fixture.id(2)));
    // Back from the second bus to the first would be a ring of buses.
    REQUIRE_EQ(fixture.hub->redirect(fixture.id(3), fixture.id(0)),
               make_error_code(errc::cycle_detected));
}

TEST_CASE("bus: a member may also be in a split")
{
    bus_fixture fixture(5);
    REQUIRE(fixture.hub->bus(fixture.ids(0, 3)));
    REQUIRE(fixture.hub->split(fixture.id(0), {fixture.id(3), fixture.id(4)}));

    // The member's device speaks to the bus and to the split; the split's
    // listeners speak to the member only.
    fixture.ports[0].inject_read("m");
    REQUIRE(fixture.wait_written(1, 1));
    REQUIRE(fixture.wait_written(3, 1));
    REQUIRE(fixture.wait_written(4, 1));

    fixture.ports[3].inject_read("s");
    REQUIRE(fixture.wait_written(0, 1));
    REQUIRE_EQ(fixture.ports[0].written(), std::string("s"));
    // The bus never heard the listener.
    REQUIRE_EQ(fixture.ports[1].written(), std::string("m"));
    REQUIRE_EQ(fixture.ports[2].written(), std::string("m"));
}

TEST_CASE("bus: the arguments are checked")
{
    bus_fixture fixture(4);

    REQUIRE_EQ(fixture.hub->bus({}), make_error_code(errc::invalid_argument));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(0)}),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(0), fixture.id(0)}),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(0), endpoint_id{9999}}),
               make_error_code(errc::not_found));

    std::vector<endpoint_id> too_many;
    for (std::size_t index = 0; index <= max_bus_members; ++index)
        too_many.push_back(static_cast<endpoint_id>(index + 1));
    REQUIRE_EQ(fixture.hub->bus(too_many),
               make_error_code(errc::invalid_argument));

    // The owner policy has no meaning on a shared bus.
    link_options owner;
    owner.policy = merge_policy::owner;
    REQUIRE_EQ(fixture.hub->bus(fixture.ids(0, 2), owner),
               make_error_code(errc::unsupported));

    // An endpoint sits on one bus only.
    REQUIRE(fixture.hub->bus(fixture.ids(0, 2)));
    REQUIRE_EQ(fixture.hub->bus({fixture.id(1), fixture.id(2)}),
               make_error_code(errc::already_exists));

    REQUIRE_EQ(fixture.hub->links().size(), std::size_t{1});
}

TEST_CASE("bus: modem lines are never carried")
{
    auto hub = start_hub();
    auto first = add_loopback(*hub, "first");
    auto second = add_loopback(*hub, "second");
    auto third = add_loopback(*hub, "third");

    link_options asked;
    asked.emulate_modem_lines = true;
    const auto bus = hub->bus({first->id(), second->id(), third->id()}, asked);
    REQUIRE(bus);
    REQUIRE(!vss_test::require_value(hub->find_link(*bus)).options.emulate_modem_lines);

    // Raising a line on one member does not move an input on another.
    const control_lines before = second->lines();
    REQUIRE(!first->set_line(control_line::dtr, true));
    REQUIRE(!first->set_line(control_line::rts, true));
    REQUIRE(wait_until([&] { return first->lines().rts(); }, 2000ms));
    REQUIRE_EQ(second->lines().bits, before.bits);
    REQUIRE_EQ(third->lines().bits, before.bits);

    // And there is nothing to rewire or to own.
    REQUIRE_EQ(hub->set_link_wiring(*bus, modem_wiring{}),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(hub->set_owner(*bus, first->id()),
               make_error_code(errc::unsupported));
}

TEST_CASE("bus: removing a member leaves the others on the bus")
{
    bus_fixture fixture(3);
    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);

    REQUIRE(fixture.hub->remove_endpoint(fixture.id(2)));

    const auto info = fixture.hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE_EQ(info->sources.size(), std::size_t{2});
    REQUIRE(std::find(info->sources.begin(), info->sources.end(),
                      fixture.id(2)) == info->sources.end());

    fixture.ports[0].inject_read("still");
    REQUIRE(wait_until([&] { return fixture.ports[1].written() == "still"; },
                       5000ms));
    fixture.ports[1].inject_read("here");
    REQUIRE(wait_until([&] { return fixture.ports[0].written() == "here"; },
                       5000ms));

    // With one member left it is not a bus; the member is free for another.
    REQUIRE(fixture.hub->remove_endpoint(fixture.id(1)));
    REQUIRE_EQ(fixture.hub->links().size(), std::size_t{0});
    REQUIRE(!fixture.hub->find_link(*bus));
}

TEST_CASE("bus: removing the bus frees its members")
{
    bus_fixture fixture(3);
    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);
    REQUIRE(fixture.hub->disconnect(*bus));
    REQUIRE_EQ(fixture.hub->links().size(), std::size_t{0});

    fixture.ports[0].inject_read("gone");
    REQUIRE(wait_until(
        [&] { return fixture.ports[0].endpoint->stats().bytes_rx == 4; },
        5000ms));
    REQUIRE_EQ(fixture.ports[1].written(), std::string());

    // They can be wired some other way, or onto a new bus, and the old one
    // leaves no cycle behind.
    REQUIRE(fixture.hub->redirect(fixture.id(1), fixture.id(0)));
    REQUIRE(fixture.hub->disconnect(fixture.hub->links().front().id));
    REQUIRE(fixture.hub->bus(fixture.ids(0, 3)));
}

TEST_CASE("bus: stats and the traffic ring count every delivery")
{
    bus_fixture fixture(3);
    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);

    fixture.ports[0].inject_read("hello");
    REQUIRE(fixture.wait_written(1, 5));
    REQUIRE(fixture.wait_written(2, 5));

    // The link counts a delivery to each receiver, as a split does.
    const auto info = fixture.hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE_EQ(info->stats.bytes, std::uint64_t{10});
    REQUIRE_EQ(info->stats.messages, std::uint64_t{2});
    REQUIRE_EQ(info->stats.dropped, std::uint64_t{0});

    REQUIRE_EQ(fixture.ports[0].endpoint->stats().bytes_rx, std::uint64_t{5});
    REQUIRE_EQ(fixture.ports[0].endpoint->stats().bytes_tx, std::uint64_t{0});
    REQUIRE(wait_until(
        [&]
        {
            return fixture.ports[1].endpoint->stats().bytes_tx == 5 &&
                   fixture.ports[2].endpoint->stats().bytes_tx == 5;
        },
        5000ms));

    // The ring holds the publication once on the sender and once on each
    // receiver.
    REQUIRE(
        wait_until([&] { return fixture.hub->traffic().size() >= 3; }, 5000ms));
    int received = 0;
    int sent = 0;
    for (const auto& record : fixture.hub->traffic())
    {
        REQUIRE(record.data != nullptr);
        REQUIRE_EQ(to_string(bytes_view(*record.data)), std::string("hello"));
        if (record.direction == data_direction::rx)
        {
            ++received;
            REQUIRE_EQ(record.name, std::string("p0"));
        }
        else
        {
            ++sent;
            REQUIRE(record.name != std::string("p0"));
        }
    }
    REQUIRE_EQ(received, 1);
    REQUIRE_EQ(sent, 2);
}

TEST_CASE("bus: concurrent publishers each reach everyone else intact")
{
    constexpr std::size_t members = 4;
    constexpr int messages = 150;

    bus_fixture fixture(members);
    REQUIRE(fixture.hub->bus(fixture.ids(0, members)));

    std::vector<std::thread> threads;
    threads.reserve(members);
    for (std::size_t index = 0; index < members; ++index)
    {
        threads.emplace_back(
            [&fixture, index]
            {
                const std::string chunk(3, char('a' + index));
                for (int count = 0; count < messages; ++count)
                    fixture.ports[index].inject_read(chunk);
            });
    }
    for (auto& thread : threads)
        thread.join();

    const std::size_t expected = (members - 1) * messages * 3;
    for (std::size_t index = 0; index < members; ++index)
    {
        REQUIRE(fixture.wait_written(index, expected));
        const std::string heard = fixture.ports[index].written();
        for (std::size_t other = 0; other < members; ++other)
        {
            const auto count = static_cast<std::size_t>(
                std::count(heard.begin(), heard.end(), char('a' + other)));
            REQUIRE_EQ(count, other == index
                                  ? std::size_t{0}
                                  : static_cast<std::size_t>(messages) * 3);
        }
    }

    // A chunk is never split by another sender's bytes.
    for (std::size_t index = 0; index < members; ++index)
    {
        const std::string heard = fixture.ports[index].written();
        for (std::size_t at = 0; at < heard.size(); at += 3)
            REQUIRE(heard[at] == heard[at + 1] && heard[at] == heard[at + 2]);
    }
}

TEST_CASE("bus: a plain bus forwards two senders at once and reports nothing")
{
    bus_fixture fixture(3);
    event_recorder events;
    const subscription_guard guard(
        *fixture.hub, {},
        [events](const endpoint_event& event) { events.record(event); });

    const auto bus = fixture.hub->bus(fixture.ids(0, 3));
    REQUIRE(bus);
    const auto made = fixture.hub->find_link(*bus);
    REQUIRE(made);
    REQUIRE_EQ(made->options.delay_ms, std::uint32_t{0});
    REQUIRE_EQ(made->options.jitter_ms, std::uint32_t{0});
    REQUIRE(made->options.policy == merge_policy::interleave);

    // Two members speak with nothing between them.  Each chunk reaches the
    // other two members whole, at once and unchanged.
    fixture.ports[0].inject_read("first");
    fixture.ports[1].inject_read("second");
    REQUIRE(wait_until(
        [&] { return fixture.ports[2].written() == "firstsecond"; }, 5000ms));
    REQUIRE(wait_until([&] { return fixture.ports[1].written() == "first"; },
                       5000ms));
    REQUIRE(wait_until([&] { return fixture.ports[0].written() == "second"; },
                       5000ms));

    const auto info = fixture.hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE_EQ(info->stats.bytes, std::uint64_t{22});
    REQUIRE_EQ(info->stats.messages, std::uint64_t{4});
    REQUIRE_EQ(info->stats.dropped, std::uint64_t{0});

    // The hub reported the traffic itself, two chunks received and four
    // sent, and no error about any of it.
    REQUIRE(wait_until(
        [&] { return events.count(event_kind::data) == 6; }, 5000ms));
    REQUIRE_EQ(events.count(event_kind::error), std::size_t{0});
}
