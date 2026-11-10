//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// hub::add_source and hub::remove_source: the rules for each link mode.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <chrono>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

struct source_fixture
{
    std::shared_ptr<vss::hub> hub = start_hub();
    mock_endpoint a = add_mock(*hub, "a");
    mock_endpoint b = add_mock(*hub, "b");
    mock_endpoint c = add_mock(*hub, "c");
    mock_endpoint d = add_mock(*hub, "d");
    mock_endpoint e = add_mock(*hub, "e");

    endpoint_id id_a() const { return a.endpoint->id(); }
    endpoint_id id_b() const { return b.endpoint->id(); }
    endpoint_id id_c() const { return c.endpoint->id(); }
    endpoint_id id_d() const { return d.endpoint->id(); }
    endpoint_id id_e() const { return e.endpoint->id(); }

    link_info info(link_id link) const
    {
        const auto found = hub->find_link(link);
        REQUIRE(found);
        return *found;
    }
};

bool heard(const mock_endpoint& port, const std::string& text)
{
    return wait_until([&] { return port.written() == text; }, 2000ms);
}

/// True once the endpoint has read `count` bytes from its device, which is
/// when the hub has routed them.
bool read(const mock_endpoint& port, std::uint64_t count)
{
    return wait_until([&] { return port.endpoint->stats().bytes_rx == count; },
                      2000ms);
}

link_options owner_options()
{
    link_options options;
    options.policy = merge_policy::owner;
    return options;
}

const auto not_found = make_error_code(errc::not_found);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);
const auto already_exists = make_error_code(errc::already_exists);
const auto cycle_detected = make_error_code(errc::cycle_detected);
} // namespace

TEST_CASE("sources: a source added to a join is heard by the target")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    f.a.inject_read("one");
    REQUIRE(heard(f.c, "one"));

    REQUIRE(f.hub->add_source(*link, f.id_d()));

    // The same link, with one more source at the end of the list.
    REQUIRE_EQ(f.hub->links().size(), std::size_t{1});
    const auto info = f.info(*link);
    REQUIRE_EQ(info.mode, link_mode::one_way);
    REQUIRE_EQ(info.sources,
               (std::vector<endpoint_id>{f.id_a(), f.id_b(), f.id_d()}));
    REQUIRE_EQ(info.targets, std::vector<endpoint_id>{f.id_c()});

    f.d.inject_read("two");
    REQUIRE(heard(f.c, "onetwo"));
    f.b.inject_read("three");
    REQUIRE(heard(f.c, "onetwothree"));

    // One way only: the target's bytes go nowhere, and the sources do not
    // hear each other.
    f.c.inject_read("back");
    REQUIRE(read(f.c, 4));
    REQUIRE_EQ(f.a.written(), std::string());
    REQUIRE_EQ(f.d.written(), std::string());

    // Each byte was delivered once.
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{11});
    REQUIRE_EQ(f.info(*link).stats.messages, std::uint64_t{3});
}

TEST_CASE("sources: a second source turns a redirect into a join")
{
    source_fixture f;
    const auto link = f.hub->redirect(f.id_a(), f.id_b());
    REQUIRE(link);
    REQUIRE(f.hub->add_source(*link, f.id_c()));

    const auto info = f.info(*link);
    REQUIRE_EQ(info.sources, (std::vector<endpoint_id>{f.id_a(), f.id_c()}));
    REQUIRE_EQ(info.targets, std::vector<endpoint_id>{f.id_b()});

    f.a.inject_read("x");
    f.c.inject_read("y");
    REQUIRE(wait_until([&] { return f.b.written().size() == 2; }, 2000ms));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{2});
}

TEST_CASE("sources: a monitor takes a second source")
{
    source_fixture f;
    const auto link = f.hub->monitor(f.id_a(), f.id_b());
    REQUIRE(link);
    REQUIRE(f.hub->add_source(*link, f.id_c()));

    // It is still a link without modem lines: a join saved as such.
    const auto info = f.info(*link);
    REQUIRE_EQ(info.sources.size(), std::size_t{2});
    REQUIRE(!info.options.emulate_modem_lines);

    f.c.inject_read("tap");
    REQUIRE(heard(f.b, "tap"));
    REQUIRE(f.hub->remove_source(*link, f.id_a()));
    REQUIRE_EQ(f.info(*link).sources, std::vector<endpoint_id>{f.id_c()});
}

TEST_CASE("sources: the counters go on counting across a change")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    f.a.inject_read("1234");
    REQUIRE(heard(f.c, "1234"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{4});
    REQUIRE_EQ(f.info(*link).stats.messages, std::uint64_t{1});

    // What the removed route carried stays counted.
    REQUIRE(f.hub->remove_source(*link, f.id_a()));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{4});

    f.b.inject_read("56");
    REQUIRE(heard(f.c, "123456"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{6});

    REQUIRE(f.hub->add_source(*link, f.id_d()));
    f.d.inject_read("7");
    REQUIRE(heard(f.c, "1234567"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{7});
    REQUIRE_EQ(f.info(*link).stats.messages, std::uint64_t{3});
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{0});
}

TEST_CASE("sources: a removed source is not heard and the rest go on")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b(), f.id_c()}, f.id_d());
    REQUIRE(link);

    REQUIRE(f.hub->remove_source(*link, f.id_b()));
    REQUIRE_EQ(f.info(*link).sources,
               (std::vector<endpoint_id>{f.id_a(), f.id_c()}));
    // The endpoint is still in the hub; only its place on the link went.
    REQUIRE(f.hub->find_endpoint(f.id_b()) != nullptr);

    f.a.inject_read("go");
    REQUIRE(heard(f.d, "go"));

    // Its bytes have no route: it is read, and the target hears the next
    // source's chunk and nothing of b's.
    f.b.inject_read("lost");
    REQUIRE(read(f.b, 4));
    f.c.inject_read("kept");
    REQUIRE(heard(f.d, "gokept"));

    // It can be added again, at the end.
    REQUIRE(f.hub->add_source(*link, f.id_b()));
    REQUIRE_EQ(f.info(*link).sources,
               (std::vector<endpoint_id>{f.id_a(), f.id_c(), f.id_b()}));
    f.b.inject_read("!");
    REQUIRE(heard(f.d, "gokept!"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{7});
}

TEST_CASE("sources: the arguments of add_source are checked")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    REQUIRE_EQ(f.hub->add_source(link_id{9999}, f.id_d()), not_found);
    REQUIRE_EQ(f.hub->add_source(*link, endpoint_id{9999}), not_found);
    // The target cannot be its own source, and a source is listed once.
    REQUIRE_EQ(f.hub->add_source(*link, f.id_c()), invalid_argument);
    REQUIRE_EQ(f.hub->add_source(*link, f.id_a()), already_exists);

    REQUIRE_EQ(f.info(*link).sources,
               (std::vector<endpoint_id>{f.id_a(), f.id_b()}));
    f.a.inject_read("fine");
    REQUIRE(heard(f.c, "fine"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{4});
}

TEST_CASE("sources: the arguments of remove_source are checked")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    REQUIRE_EQ(f.hub->remove_source(link_id{9999}, f.id_a()), not_found);
    // Neither an unknown endpoint, one off the link nor the target is a
    // source of it.
    REQUIRE_EQ(f.hub->remove_source(*link, endpoint_id{9999}), not_found);
    REQUIRE_EQ(f.hub->remove_source(*link, f.id_d()), not_found);
    REQUIRE_EQ(f.hub->remove_source(*link, f.id_c()), not_found);

    REQUIRE(f.hub->remove_source(*link, f.id_a()));
    REQUIRE_EQ(f.hub->remove_source(*link, f.id_a()), not_found);

    // A link keeps at least one source.
    REQUIRE_EQ(f.hub->remove_source(*link, f.id_b()), invalid_argument);
    REQUIRE_EQ(f.info(*link).sources, std::vector<endpoint_id>{f.id_b()});
    f.b.inject_read("still");
    REQUIRE(heard(f.c, "still"));
}

TEST_CASE("sources: duplex links and buses take no source changes")
{
    source_fixture f;
    const auto connected = f.hub->connect(f.id_a(), f.id_b());
    REQUIRE(connected);
    REQUIRE_EQ(f.hub->add_source(*connected, f.id_c()), unsupported);
    REQUIRE_EQ(f.hub->remove_source(*connected, f.id_a()), unsupported);
    REQUIRE(f.hub->disconnect(*connected));

    const auto split = f.hub->split(f.id_a(), {f.id_b(), f.id_c()});
    REQUIRE(split);
    REQUIRE_EQ(f.hub->add_source(*split, f.id_d()), unsupported);
    REQUIRE_EQ(f.hub->remove_source(*split, f.id_a()), unsupported);
    REQUIRE_EQ(f.info(*split).sources, std::vector<endpoint_id>{f.id_a()});
    REQUIRE(f.hub->disconnect(*split));

    const auto bus = f.hub->bus({f.id_a(), f.id_b(), f.id_c()});
    REQUIRE(bus);
    REQUIRE_EQ(f.hub->add_source(*bus, f.id_d()), unsupported);
    REQUIRE_EQ(f.hub->remove_source(*bus, f.id_c()), unsupported);
    REQUIRE_EQ(f.info(*bus).sources.size(), std::size_t{3});
}

TEST_CASE("sources: a source that would close a loop is refused whole")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    // c already sends to d, so d -> c would loop.
    const auto outward = f.hub->redirect(f.id_c(), f.id_d());
    REQUIRE(outward);
    REQUIRE_EQ(f.hub->add_source(*link, f.id_d()), cycle_detected);

    // c and e share a bus: e as a source would feed c from inside the node.
    REQUIRE(f.hub->bus({f.id_c(), f.id_e()}));
    REQUIRE_EQ(f.hub->add_source(*link, f.id_e()), cycle_detected);

    // Neither refusal left a route behind.
    REQUIRE_EQ(f.info(*link).sources,
               (std::vector<endpoint_id>{f.id_a(), f.id_b()}));
    f.d.inject_read("x");
    REQUIRE(read(f.d, 1));
    f.a.inject_read("z");
    REQUIRE(heard(f.c, "z"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{1});
}

TEST_CASE("sources: a loop through a bus is refused too")
{
    source_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    // c feeds d, d and e share a bus: e -> c would come back by the bus.
    REQUIRE(f.hub->redirect(f.id_c(), f.id_d()));
    REQUIRE(f.hub->bus({f.id_d(), f.id_e()}));
    REQUIRE_EQ(f.hub->add_source(*link, f.id_e()), cycle_detected);
    REQUIRE_EQ(f.info(*link).sources.size(), std::size_t{2});

    // An endpoint with no way round is still welcome.
    auto g = add_mock(*f.hub, "g");
    REQUIRE(f.hub->add_source(*link, g.endpoint->id()));
}

TEST_CASE("sources: a tap on a bus can take another member as a source")
{
    source_fixture f;
    // a and b share a bus; c listens to both and to e.
    REQUIRE(f.hub->bus({f.id_a(), f.id_b()}));
    const auto tap = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(tap);
    REQUIRE(f.hub->add_source(*tap, f.id_e()));

    f.a.inject_read("1");
    f.b.inject_read("2");
    f.e.inject_read("3");
    REQUIRE(wait_until([&] { return f.c.written().size() == 3; }, 2000ms));
    // a and b heard each other on the bus; e is not on it.
    REQUIRE(heard(f.a, "2"));
    REQUIRE(heard(f.b, "1"));
    REQUIRE_EQ(f.e.written(), std::string());
}

TEST_CASE("sources: removing the owner releases the link")
{
    source_fixture f;
    const auto link =
        f.hub->join({f.id_a(), f.id_b(), f.id_c()}, f.id_d(), owner_options());
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_b()));

    // Removing another source leaves the owner alone.
    REQUIRE(f.hub->remove_source(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).owner, f.id_b());

    REQUIRE(f.hub->remove_source(*link, f.id_b()));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});

    // The source that is left takes the link when it sends first.
    f.a.inject_read("mine");
    REQUIRE(heard(f.d, "mine"));
    REQUIRE_EQ(f.info(*link).owner, f.id_a());
}

TEST_CASE("sources: a new source's route is arbitrated like the others")
{
    source_fixture f;
    const auto link =
        f.hub->join({f.id_a(), f.id_b()}, f.id_c(), owner_options());
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_a()));

    REQUIRE(f.hub->add_source(*link, f.id_d()));
    // Adding it moved nothing.
    REQUIRE_EQ(f.info(*link).owner, f.id_a());

    // The owner is heard; the new source is dropped, and counted.
    f.d.inject_read("no");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 2; }, 2000ms));
    REQUIRE_EQ(f.c.written(), std::string());
    f.a.inject_read("yes");
    REQUIRE(heard(f.c, "yes"));

    // It can be given the link, which it can own like any source...
    REQUIRE(f.hub->set_owner(*link, f.id_d()));
    f.d.inject_read("now");
    REQUIRE(heard(f.c, "yesnow"));
    f.a.inject_read("late");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 6; }, 2000ms));

    // ...and once it is off the link it cannot be given it again.
    REQUIRE(f.hub->remove_source(*link, f.id_d()));
    REQUIRE_EQ(f.hub->set_owner(*link, f.id_d()), invalid_argument);
}

TEST_CASE("sources: a new source takes a link that has no owner")
{
    source_fixture f;
    const auto link = f.hub->redirect(f.id_a(), f.id_b(), owner_options());
    REQUIRE(link);
    REQUIRE(f.hub->add_source(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});

    f.c.inject_read("first");
    REQUIRE(heard(f.b, "first"));
    REQUIRE_EQ(f.info(*link).owner, f.id_c());

    f.a.inject_read("no");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 2; }, 2000ms));
    REQUIRE_EQ(f.b.written(), std::string("first"));
}

TEST_CASE("sources: bytes still delayed for a removed source are discarded")
{
    source_fixture f;
    link_options options;
    options.delay_ms = 400;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c(), options);
    REQUIRE(link);

    f.a.inject_read("held");
    REQUIRE(read(f.a, 4));
    f.b.inject_read("T");
    REQUIRE(read(f.b, 1));
    REQUIRE(f.hub->remove_source(*link, f.id_a()));

    // b is the clock: its chunk was routed after a's, so it falls due after
    // a's, and the one after it a whole delay later.
    REQUIRE(heard(f.c, "T"));
    f.b.inject_read("U");
    REQUIRE(heard(f.c, "TU"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{6});

    // A source added again starts with an empty line.
    REQUIRE(f.hub->add_source(*link, f.id_a()));
    f.a.inject_read("new");
    REQUIRE(heard(f.c, "TUnew"));
}

TEST_CASE("sources: a new source's route carries the link's delay")
{
    source_fixture f;
    link_options options;
    options.delay_ms = 300;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c(), options);
    REQUIRE(link);
    REQUIRE(f.hub->add_source(*link, f.id_d()));

    // Nothing is delivered before the delay has passed, whatever the load.
    const auto start = std::chrono::steady_clock::now();
    f.d.inject_read("slow");
    REQUIRE(heard(f.c, "slow"));
    REQUIRE(std::chrono::steady_clock::now() - start >= 300ms);
}

TEST_CASE("sources: a new source's lines reach the target from its next change")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    const auto d = add_loopback(*hub, "d");
    const auto link = hub->join({a->id(), b->id()}, c->id());
    REQUIRE(link);

    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(c->lines().dsr());

    // Adding a source changes no input: the target still shows a.
    REQUIRE(!d->set_line(control_line::rts, true));
    REQUIRE(hub->add_source(*link, d->id()));
    REQUIRE(c->lines().dsr());
    REQUIRE(!c->lines().cts());

    // From its next change the target follows it, as it follows any source
    // of a join.
    REQUIRE(!d->set_line(control_line::dtr, true));
    REQUIRE(c->lines().cts());
    REQUIRE(c->lines().dsr());
    REQUIRE(!d->set_line(control_line::dtr, false));
    REQUIRE(c->lines().cts());
    REQUIRE(!c->lines().dsr());
}

TEST_CASE("sources: a removed source's lines no longer show on the target")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto d = add_loopback(*hub, "d");
    const auto c = add_loopback(*hub, "c");
    const auto link = hub->join({a->id(), b->id(), d->id()}, c->id());
    REQUIRE(link);

    // a raises DTR, then d raises RTS: the target shows the last to change.
    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(!d->set_line(control_line::rts, true));
    REQUIRE(c->lines().cts());
    REQUIRE(!c->lines().dsr());

    // b drives nothing; with it gone the sources that remain are applied in
    // list order, so the last of them shows.
    REQUIRE(hub->remove_source(*link, b->id()));
    REQUIRE(c->lines().cts());
    REQUIRE(!c->lines().dsr());

    // d was showing; with d gone the target shows a, not a port that has left.
    REQUIRE(hub->remove_source(*link, d->id()));
    REQUIRE(c->lines().dsr());
    REQUIRE(!c->lines().cts());

    // d's outputs are its own: nothing of the link touched them.
    REQUIRE(d->lines().rts());
}

TEST_CASE("sources: a link without modem lines moves none")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    const auto d = add_loopback(*hub, "d");
    const auto link = hub->monitor(a->id(), b->id());
    REQUIRE(link);
    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(!c->set_line(control_line::rts, true));

    const control_lines before = b->lines();
    REQUIRE(hub->add_source(*link, c->id()));
    REQUIRE_EQ(b->lines().bits, before.bits);
    REQUIRE(!c->set_line(control_line::dtr, true));
    REQUIRE_EQ(b->lines().bits, before.bits);
    REQUIRE(hub->add_source(*link, d->id()));
    REQUIRE(hub->remove_source(*link, c->id()));
    REQUIRE_EQ(b->lines().bits, before.bits);
}
