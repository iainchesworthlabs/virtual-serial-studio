//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// hub::add_member and hub::remove_member: a bus whose membership changes.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

struct member_fixture
{
    std::shared_ptr<vss::hub> hub = start_hub();
    std::vector<mock_endpoint> ports;

    explicit member_fixture(std::size_t count)
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

    link_info info(link_id link) const
    {
        const auto found = hub->find_link(link);
        REQUIRE(found);
        return *found;
    }

    bool heard(std::size_t index, const std::string& text) const
    {
        return wait_until([&] { return ports[index].written() == text; },
                          5000ms);
    }

    /// True once the endpoint has read `count` bytes from its device, which
    /// is when the hub has routed them.
    bool read(std::size_t index, std::uint64_t count) const
    {
        return wait_until(
            [&] { return ports[index].endpoint->stats().bytes_rx == count; },
            5000ms);
    }
};

const auto not_found = make_error_code(errc::not_found);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);
const auto already_exists = make_error_code(errc::already_exists);
const auto cycle_detected = make_error_code(errc::cycle_detected);
} // namespace

TEST_CASE("members: a member added to a bus hears the others and is heard")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);

    f.ports[0].inject_read("A");
    REQUIRE(f.heard(1, "A"));

    REQUIRE(f.hub->add_member(*bus, f.id(2)));

    // The same bus, with one more member at the end of the list.
    REQUIRE_EQ(f.hub->links().size(), std::size_t{1});
    const auto info = f.info(*bus);
    REQUIRE_EQ(info.mode, link_mode::bus);
    REQUIRE_EQ(info.sources, f.ids(0, 3));
    REQUIRE_EQ(info.targets.size(), std::size_t{0});
    REQUIRE(!info.options.emulate_modem_lines);

    // The new member hears nothing from before, then everyone.
    REQUIRE_EQ(f.ports[2].written(), std::string());
    f.ports[0].inject_read("B");
    REQUIRE(f.heard(1, "AB"));
    REQUIRE(f.heard(2, "B"));
    f.ports[1].inject_read("C");
    REQUIRE(f.heard(0, "C"));
    REQUIRE(f.heard(2, "BC"));

    // And it is heard by both of the others, never by itself.
    f.ports[2].inject_read("D");
    REQUIRE(f.heard(0, "CD"));
    REQUIRE(f.heard(1, "ABD"));
    REQUIRE_EQ(f.ports[2].written(), std::string("BC"));

    // Each delivery was made once: A went to one member, B and C to two, D
    // to two.
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{7});
    REQUIRE_EQ(f.info(*bus).stats.messages, std::uint64_t{7});
}

TEST_CASE("members: the counters go on counting across a change")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 3));
    REQUIRE(bus);

    f.ports[0].inject_read("1234");
    REQUIRE(f.heard(1, "1234"));
    REQUIRE(f.heard(2, "1234"));
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{8});
    REQUIRE_EQ(f.info(*bus).stats.messages, std::uint64_t{2});

    // What the removed routes carried stays counted.
    REQUIRE(f.hub->remove_member(*bus, f.id(2)));
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{8});

    f.ports[0].inject_read("56");
    REQUIRE(f.heard(1, "123456"));
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{10});

    REQUIRE(f.hub->add_member(*bus, f.id(3)));
    f.ports[1].inject_read("7");
    REQUIRE(f.heard(3, "7"));
    REQUIRE(f.heard(0, "7"));
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{12});
    REQUIRE_EQ(f.info(*bus).stats.messages, std::uint64_t{5});
    REQUIRE_EQ(f.info(*bus).stats.dropped, std::uint64_t{0});
}

TEST_CASE("members: a removed member hears nothing and the rest go on")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 4));
    REQUIRE(bus);

    REQUIRE(f.hub->remove_member(*bus, f.id(1)));
    REQUIRE_EQ(f.info(*bus).sources,
               (std::vector<endpoint_id>{f.id(0), f.id(2), f.id(3)}));
    // The endpoint is still in the hub; only its place on the bus went.
    REQUIRE(f.hub->find_endpoint(f.id(1)) != nullptr);

    f.ports[0].inject_read("go");
    REQUIRE(f.heard(2, "go"));
    REQUIRE(f.heard(3, "go"));
    f.ports[2].inject_read("on");
    REQUIRE(f.heard(0, "on"));
    REQUIRE(f.heard(3, "goon"));

    // What it says goes nowhere: the others hear a later chunk and nothing
    // of its.
    f.ports[1].inject_read("lost");
    REQUIRE(f.read(1, 4));
    f.ports[3].inject_read("kept");
    REQUIRE(f.heard(0, "onkept"));
    REQUIRE(f.heard(2, "gokept"));
    REQUIRE_EQ(f.ports[1].written(), std::string());

    // It can be added again, at the end.
    REQUIRE(f.hub->add_member(*bus, f.id(1)));
    REQUIRE_EQ(f.info(*bus).sources,
               (std::vector<endpoint_id>{f.id(0), f.id(2), f.id(3), f.id(1)}));
    f.ports[0].inject_read("!");
    REQUIRE(f.heard(1, "!"));
}

TEST_CASE("members: a removed member is free for another bus")
{
    member_fixture f(5);
    const auto bus = f.hub->bus(f.ids(0, 3));
    REQUIRE(bus);
    REQUIRE_EQ(f.hub->bus({f.id(2), f.id(3)}), already_exists);

    REQUIRE(f.hub->remove_member(*bus, f.id(2)));
    const auto other = f.hub->bus({f.id(2), f.id(3)});
    REQUIRE(other);
    REQUIRE_EQ(f.hub->add_member(*other, f.id(0)), already_exists);

    // Each bus carries its own members.
    f.ports[2].inject_read("x");
    REQUIRE(f.heard(3, "x"));
    REQUIRE_EQ(f.ports[0].written(), std::string());
    REQUIRE_EQ(f.ports[1].written(), std::string());
}

TEST_CASE("members: the arguments of add_member are checked")
{
    member_fixture f(5);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);
    const auto other = f.hub->bus({f.id(2), f.id(3)});
    REQUIRE(other);

    REQUIRE_EQ(f.hub->add_member(link_id{9999}, f.id(4)), not_found);
    REQUIRE_EQ(f.hub->add_member(*bus, endpoint_id{9999}), not_found);
    // A member is listed once, and an endpoint is on one bus at most.
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(0)), already_exists);
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(2)), already_exists);

    REQUIRE_EQ(f.info(*bus).sources, f.ids(0, 2));
    REQUIRE_EQ(f.info(*other).sources, f.ids(2, 4));
    f.ports[0].inject_read("fine");
    REQUIRE(f.heard(1, "fine"));
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{4});
}

TEST_CASE("members: a bus takes no more than the most members")
{
    member_fixture f(max_bus_members + 1);
    const auto bus = f.hub->bus(f.ids(0, max_bus_members - 1));
    REQUIRE(bus);

    // The last place is free, and then there is none.
    REQUIRE(f.hub->add_member(*bus, f.id(max_bus_members - 1)));
    REQUIRE_EQ(f.info(*bus).sources.size(), max_bus_members);
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(max_bus_members)),
               invalid_argument);
    REQUIRE_EQ(f.info(*bus).sources.size(), max_bus_members);

    // Taking one out makes room again.
    REQUIRE(f.hub->remove_member(*bus, f.id(0)));
    REQUIRE(f.hub->add_member(*bus, f.id(max_bus_members)));
    REQUIRE_EQ(f.info(*bus).sources.size(), max_bus_members);
}

TEST_CASE("members: the arguments of remove_member are checked")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 3));
    REQUIRE(bus);

    REQUIRE_EQ(f.hub->remove_member(link_id{9999}, f.id(0)), not_found);
    // Neither an unknown endpoint nor one off the bus is a member of it.
    REQUIRE_EQ(f.hub->remove_member(*bus, endpoint_id{9999}), not_found);
    REQUIRE_EQ(f.hub->remove_member(*bus, f.id(3)), not_found);

    REQUIRE(f.hub->remove_member(*bus, f.id(0)));
    REQUIRE_EQ(f.hub->remove_member(*bus, f.id(0)), not_found);

    // A bus keeps at least two members.
    REQUIRE_EQ(f.hub->remove_member(*bus, f.id(1)), invalid_argument);
    REQUIRE_EQ(f.hub->remove_member(*bus, f.id(2)), invalid_argument);
    REQUIRE_EQ(f.info(*bus).sources, f.ids(1, 3));
    f.ports[1].inject_read("still");
    REQUIRE(f.heard(2, "still"));
    f.ports[2].inject_read("here");
    REQUIRE(f.heard(1, "here"));
}

TEST_CASE("members: links that are not buses take no member changes")
{
    member_fixture f(5);
    const auto connected = f.hub->connect(f.id(0), f.id(1));
    REQUIRE(connected);
    REQUIRE_EQ(f.hub->add_member(*connected, f.id(4)), unsupported);
    REQUIRE_EQ(f.hub->remove_member(*connected, f.id(1)), unsupported);
    REQUIRE(f.hub->disconnect(*connected));

    const auto split = f.hub->split(f.id(0), {f.id(1), f.id(2)});
    REQUIRE(split);
    REQUIRE_EQ(f.hub->add_member(*split, f.id(4)), unsupported);
    REQUIRE_EQ(f.hub->remove_member(*split, f.id(1)), unsupported);
    REQUIRE(f.hub->disconnect(*split));

    const auto joined = f.hub->join({f.id(0), f.id(1)}, f.id(2));
    REQUIRE(joined);
    REQUIRE_EQ(f.hub->add_member(*joined, f.id(4)), unsupported);
    REQUIRE_EQ(f.hub->remove_member(*joined, f.id(1)), unsupported);
    REQUIRE(f.hub->disconnect(*joined));

    const auto redirected = f.hub->redirect(f.id(0), f.id(1));
    REQUIRE(redirected);
    REQUIRE_EQ(f.hub->add_member(*redirected, f.id(4)), unsupported);
    REQUIRE_EQ(f.hub->remove_member(*redirected, f.id(1)), unsupported);
    REQUIRE(f.hub->disconnect(*redirected));

    const auto watched = f.hub->monitor(f.id(0), f.id(1));
    REQUIRE(watched);
    REQUIRE_EQ(f.hub->add_member(*watched, f.id(4)), unsupported);
    REQUIRE_EQ(f.hub->remove_member(*watched, f.id(1)), unsupported);
    REQUIRE_EQ(f.info(*watched).sources, std::vector<endpoint_id>{f.id(0)});
}

TEST_CASE("members: an endpoint that already sends to the bus is refused")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);

    // p2 sends to a member: its bytes would arrive by the route and by the
    // bus.
    REQUIRE(f.hub->redirect(f.id(2), f.id(1)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(2)), cycle_detected);

    // The path may be longer than one hop.
    REQUIRE(f.hub->redirect(f.id(3), f.id(2)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(3)), cycle_detected);

    // Neither refusal left a route or a member behind.
    REQUIRE_EQ(f.info(*bus).sources, f.ids(0, 2));
    f.ports[0].inject_read("x");
    REQUIRE(f.heard(1, "x"));
    REQUIRE_EQ(f.ports[2].written(), std::string());
    REQUIRE_EQ(f.ports[3].written(), std::string());
    REQUIRE_EQ(f.info(*bus).stats.bytes, std::uint64_t{1});
}

TEST_CASE("members: an endpoint that the bus already sends to is refused")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);

    // A member sends to p2: p2 as a member would hear it twice.
    REQUIRE(f.hub->redirect(f.id(0), f.id(2)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(2)), cycle_detected);

    // A two-way link to a member is a route in each direction.
    REQUIRE(f.hub->connect(f.id(1), f.id(3)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(3)), cycle_detected);

    REQUIRE_EQ(f.info(*bus).sources, f.ids(0, 2));
    f.ports[1].inject_read("y");
    REQUIRE(f.heard(0, "y"));
    REQUIRE(f.heard(3, "y"));
    REQUIRE_EQ(f.ports[2].written(), std::string());
}

TEST_CASE("members: a loop through other links or another bus is refused")
{
    member_fixture f(14);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);

    // Out of the new endpoint and into the bus by way of p3.
    REQUIRE(f.hub->redirect(f.id(2), f.id(3)));
    REQUIRE(f.hub->redirect(f.id(3), f.id(0)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(2)), cycle_detected);

    // Out of the bus and into the new endpoint by way of p4.
    REQUIRE(f.hub->redirect(f.id(1), f.id(4)));
    REQUIRE(f.hub->redirect(f.id(4), f.id(5)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(5)), cycle_detected);

    // Through a second bus (p6 and p7), into the first: p8 feeds the second
    // bus and the second feeds the first.
    REQUIRE(f.hub->bus({f.id(6), f.id(7)}));
    REQUIRE(f.hub->redirect(f.id(8), f.id(6)));
    REQUIRE(f.hub->redirect(f.id(7), f.id(0)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(8)), cycle_detected);

    // Through a third bus (p10 and p11), out of the first: the first feeds
    // the third and the third feeds p12.
    REQUIRE(f.hub->bus({f.id(10), f.id(11)}));
    REQUIRE(f.hub->redirect(f.id(1), f.id(10)));
    REQUIRE(f.hub->redirect(f.id(11), f.id(12)));
    REQUIRE_EQ(f.hub->add_member(*bus, f.id(12)), cycle_detected);

    // Nothing was left behind, and an endpoint with no way round is welcome.
    REQUIRE_EQ(f.info(*bus).sources, f.ids(0, 2));
    f.ports[0].inject_read("x");
    REQUIRE(f.heard(1, "x"));
    for (const std::size_t index : {2u, 5u, 8u, 12u})
        REQUIRE_EQ(f.ports[index].written(), std::string());
    REQUIRE(f.hub->add_member(*bus, f.id(13)));
}

TEST_CASE("members: a member's route to a tap beside the bus is not a loop")
{
    member_fixture f(5);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);

    // p3 listens to both members, and the new member as well: p2 and the bus
    // reach p3 by routes of their own, which is not a path between them.
    REQUIRE(f.hub->join(f.ids(0, 2), f.id(3)));
    const auto tap = f.hub->redirect(f.id(2), f.id(3));
    REQUIRE(tap);
    REQUIRE(f.hub->add_member(*bus, f.id(2)));

    f.ports[0].inject_read("a");
    REQUIRE(f.heard(1, "a"));
    REQUIRE(f.heard(2, "a"));
    f.ports[2].inject_read("c");
    REQUIRE(f.heard(1, "ac"));
    REQUIRE(
        wait_until([&] { return f.ports[3].written().size() == 2; }, 5000ms));
    REQUIRE_EQ(f.info(*tap).stats.bytes, std::uint64_t{1});

    // A route from a member to the new one is a loop, as it was.
    REQUIRE_EQ(f.hub->redirect(f.id(1), f.id(2)), cycle_detected);
}

TEST_CASE("members: a tap that joins from the members can take the new one")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);
    const auto tap = f.hub->join(f.ids(0, 2), f.id(3));
    REQUIRE(tap);

    REQUIRE(f.hub->add_member(*bus, f.id(2)));
    REQUIRE(f.hub->add_source(*tap, f.id(2)));

    f.ports[0].inject_read("x");
    f.ports[1].inject_read("y");
    f.ports[2].inject_read("z");
    REQUIRE(
        wait_until([&] { return f.ports[3].written().size() == 3; }, 5000ms));
    // Each byte reached the tap once, and each member heard the other two.
    REQUIRE_EQ(f.info(*tap).stats.bytes, std::uint64_t{3});
    REQUIRE(
        wait_until([&] { return f.ports[0].written().size() == 2; }, 5000ms));
    REQUIRE(
        wait_until([&] { return f.ports[1].written().size() == 2; }, 5000ms));
    REQUIRE(
        wait_until([&] { return f.ports[2].written().size() == 2; }, 5000ms));
    REQUIRE(f.ports[0].written().find('x') == std::string::npos);
    REQUIRE(f.ports[1].written().find('y') == std::string::npos);
    REQUIRE(f.ports[2].written().find('z') == std::string::npos);
}

TEST_CASE("members: bytes still delayed for a removed member are discarded")
{
    member_fixture f(3);
    link_options options;
    options.delay_ms = 400;
    const auto bus = f.hub->bus(f.ids(0, 3), options);
    REQUIRE(bus);

    // p0 and p2 each speak, and p2 leaves before either chunk is due.
    f.ports[0].inject_read("held");
    REQUIRE(f.read(0, 4));
    f.ports[2].inject_read("out");
    REQUIRE(f.read(2, 3));
    REQUIRE(f.hub->remove_member(*bus, f.id(2)));

    // p1 is the clock: it hears p0's chunk, which was due before p2's, and p2's
    // would have reached it too.  The next one is a whole delay later.
    REQUIRE(f.heard(1, "held"));
    f.ports[0].inject_read("next");
    REQUIRE(f.heard(1, "heldnext"));
    // Nothing p2 said reached p0, and nothing p0 said reached p2.
    REQUIRE_EQ(f.ports[0].written(), std::string());
    REQUIRE_EQ(f.ports[2].written(), std::string());

    // A member added again starts with empty lines.
    REQUIRE(f.hub->add_member(*bus, f.id(2)));
    f.ports[0].inject_read("new");
    REQUIRE(f.heard(2, "new"));
}

TEST_CASE("members: a new member's routes carry the bus's delay")
{
    member_fixture f(3);
    link_options options;
    options.delay_ms = 300;
    const auto bus = f.hub->bus(f.ids(0, 2), options);
    REQUIRE(bus);
    REQUIRE(f.hub->add_member(*bus, f.id(2)));

    // Nothing is delivered before the delay has passed, in either direction.
    auto start = std::chrono::steady_clock::now();
    f.ports[0].inject_read("to");
    REQUIRE(f.heard(2, "to"));
    REQUIRE(std::chrono::steady_clock::now() - start >= 300ms);

    start = std::chrono::steady_clock::now();
    f.ports[2].inject_read("fro");
    REQUIRE(f.heard(1, "tofro"));
    REQUIRE(std::chrono::steady_clock::now() - start >= 300ms);
}

TEST_CASE("members: a bus carries no modem lines whoever is on it")
{
    auto hub = start_hub();
    const auto first = add_loopback(*hub, "first");
    const auto second = add_loopback(*hub, "second");
    const auto third = add_loopback(*hub, "third");
    const auto bus = hub->bus({first->id(), second->id()});
    REQUIRE(bus);
    const control_lines second_before = second->lines();
    const control_lines third_before = third->lines();
    REQUIRE(!first->set_line(control_line::dtr, true));

    REQUIRE(hub->add_member(*bus, third->id()));
    REQUIRE_EQ(third->lines().bits, third_before.bits);
    REQUIRE(!first->set_line(control_line::rts, true));
    REQUIRE(!third->set_line(control_line::dtr, true));
    REQUIRE_EQ(second->lines().bits, second_before.bits);
    REQUIRE(hub->remove_member(*bus, third->id()));
    REQUIRE_EQ(second->lines().bits, second_before.bits);
    REQUIRE(!vss_test::require_value(hub->find_link(*bus))
                 .options.emulate_modem_lines);
}

TEST_CASE("members: removing an endpoint still takes it off a changed bus")
{
    member_fixture f(4);
    const auto bus = f.hub->bus(f.ids(0, 2));
    REQUIRE(bus);
    REQUIRE(f.hub->add_member(*bus, f.id(2)));
    REQUIRE(f.hub->add_member(*bus, f.id(3)));

    REQUIRE(f.hub->remove_endpoint(f.id(2)));
    REQUIRE_EQ(f.info(*bus).sources,
               (std::vector<endpoint_id>{f.id(0), f.id(1), f.id(3)}));
    f.ports[3].inject_read("q");
    REQUIRE(f.heard(0, "q"));
    REQUIRE(f.heard(1, "q"));
}
