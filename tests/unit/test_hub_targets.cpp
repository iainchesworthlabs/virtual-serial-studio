//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// hub::add_target and hub::remove_target: the rules for each link mode.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <chrono>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

struct target_fixture
{
    std::shared_ptr<vss::hub> hub = start_hub();
    mock_endpoint a = add_mock(*hub, "a");
    mock_endpoint b = add_mock(*hub, "b");
    mock_endpoint c = add_mock(*hub, "c");
    mock_endpoint d = add_mock(*hub, "d");

    endpoint_id id_a() const { return a.endpoint->id(); }
    endpoint_id id_b() const { return b.endpoint->id(); }
    endpoint_id id_c() const { return c.endpoint->id(); }
    endpoint_id id_d() const { return d.endpoint->id(); }

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

const auto not_found = make_error_code(errc::not_found);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);
const auto already_exists = make_error_code(errc::already_exists);
const auto cycle_detected = make_error_code(errc::cycle_detected);
} // namespace

TEST_CASE("targets: a target added to a split hears the source and answers it")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b()});
    REQUIRE(link);

    f.a.inject_read("one");
    REQUIRE(heard(f.b, "one"));

    REQUIRE(f.hub->add_target(*link, f.id_c()));

    // The same link, with one more target at the end of the list.
    REQUIRE_EQ(f.hub->links().size(), std::size_t{1});
    const auto info = f.info(*link);
    REQUIRE_EQ(info.mode, link_mode::duplex);
    REQUIRE_EQ(info.sources, std::vector<endpoint_id>{f.id_a()});
    REQUIRE_EQ(info.targets, (std::vector<endpoint_id>{f.id_b(), f.id_c()}));

    f.a.inject_read("two");
    REQUIRE(heard(f.b, "onetwo"));
    REQUIRE(heard(f.c, "two"));

    f.c.inject_read("reply");
    REQUIRE(heard(f.a, "reply"));
    // The targets of a split still do not hear each other.
    REQUIRE_EQ(f.b.written(), std::string("onetwo"));
}

TEST_CASE("targets: a second target turns a two-way link into a split")
{
    target_fixture f;
    const auto link = f.hub->connect(f.id_a(), f.id_b());
    REQUIRE(link);
    REQUIRE(f.hub->add_target(*link, f.id_c()));

    const auto info = f.info(*link);
    REQUIRE_EQ(info.sources.size(), std::size_t{1});
    REQUIRE_EQ(info.targets.size(), std::size_t{2});

    f.a.inject_read("x");
    REQUIRE(heard(f.b, "x"));
    REQUIRE(heard(f.c, "x"));
}

TEST_CASE("targets: the counters go on counting across a change")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()});
    REQUIRE(link);

    f.a.inject_read("1234");
    REQUIRE(heard(f.b, "1234"));
    REQUIRE(heard(f.c, "1234"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{8});
    REQUIRE_EQ(f.info(*link).stats.messages, std::uint64_t{2});

    // What the removed route carried stays counted.
    REQUIRE(f.hub->remove_target(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{8});

    f.a.inject_read("56");
    REQUIRE(heard(f.b, "123456"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{10});
    REQUIRE_EQ(f.info(*link).stats.messages, std::uint64_t{3});

    REQUIRE(f.hub->add_target(*link, f.id_d()));
    f.a.inject_read("7");
    REQUIRE(heard(f.b, "1234567"));
    REQUIRE(heard(f.d, "7"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{12});
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{0});
}

TEST_CASE("targets: a removed target hears nothing more and the rest go on")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c(), f.id_d()});
    REQUIRE(link);

    REQUIRE(f.hub->remove_target(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).targets,
               (std::vector<endpoint_id>{f.id_b(), f.id_d()}));
    // The endpoint is still in the hub; only its place on the link went.
    REQUIRE(f.hub->find_endpoint(f.id_c()) != nullptr);

    f.a.inject_read("go");
    REQUIRE(heard(f.b, "go"));
    REQUIRE(heard(f.d, "go"));

    // Its replies have no route either: the source hears b's reply, sent
    // after c's was read, and nothing of c's before it.
    f.c.inject_read("lost");
    REQUIRE(wait_until([&] { return f.c.endpoint->stats().bytes_rx == 4; },
                       2000ms));
    f.b.inject_read("kept");
    REQUIRE(heard(f.a, "kept"));
    REQUIRE_EQ(f.c.written(), std::string());

    // It can be added again, at the end.
    REQUIRE(f.hub->add_target(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).targets,
               (std::vector<endpoint_id>{f.id_b(), f.id_d(), f.id_c()}));
    f.a.inject_read("!");
    REQUIRE(heard(f.c, "!"));
}

TEST_CASE("targets: the arguments of add_target are checked")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b()});
    REQUIRE(link);

    REQUIRE_EQ(f.hub->add_target(link_id{9999}, f.id_c()), not_found);
    REQUIRE_EQ(f.hub->add_target(*link, endpoint_id{9999}), not_found);
    // The source cannot be its own target, and a target is listed once.
    REQUIRE_EQ(f.hub->add_target(*link, f.id_a()), invalid_argument);
    REQUIRE_EQ(f.hub->add_target(*link, f.id_b()), already_exists);

    REQUIRE_EQ(f.info(*link).targets, std::vector<endpoint_id>{f.id_b()});
}

TEST_CASE("targets: the arguments of remove_target are checked")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()});
    REQUIRE(link);

    REQUIRE_EQ(f.hub->remove_target(link_id{9999}, f.id_b()), not_found);
    // Neither an unknown endpoint nor one off the link is a target of it.
    REQUIRE_EQ(f.hub->remove_target(*link, endpoint_id{9999}), not_found);
    REQUIRE_EQ(f.hub->remove_target(*link, f.id_d()), not_found);
    REQUIRE_EQ(f.hub->remove_target(*link, f.id_a()), invalid_argument);

    REQUIRE(f.hub->remove_target(*link, f.id_b()));
    REQUIRE_EQ(f.hub->remove_target(*link, f.id_b()), not_found);

    // A link keeps at least one target.
    REQUIRE_EQ(f.hub->remove_target(*link, f.id_c()), invalid_argument);
    REQUIRE_EQ(f.info(*link).targets, std::vector<endpoint_id>{f.id_c()});
    f.a.inject_read("still");
    REQUIRE(heard(f.c, "still"));
}

TEST_CASE("targets: one-way links and buses take no target changes")
{
    target_fixture f;
    const auto joined = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(joined);
    REQUIRE_EQ(f.hub->add_target(*joined, f.id_d()), unsupported);
    REQUIRE_EQ(f.hub->remove_target(*joined, f.id_c()), unsupported);
    REQUIRE(f.hub->disconnect(*joined));

    const auto redirected = f.hub->redirect(f.id_a(), f.id_b());
    REQUIRE(redirected);
    REQUIRE_EQ(f.hub->add_target(*redirected, f.id_c()), unsupported);
    REQUIRE_EQ(f.hub->remove_target(*redirected, f.id_b()), unsupported);
    REQUIRE(f.hub->disconnect(*redirected));

    const auto watched = f.hub->monitor(f.id_a(), f.id_b());
    REQUIRE(watched);
    REQUIRE_EQ(f.hub->add_target(*watched, f.id_c()), unsupported);
    REQUIRE_EQ(f.hub->remove_target(*watched, f.id_b()), unsupported);
    REQUIRE(f.hub->disconnect(*watched));

    const auto bus = f.hub->bus({f.id_a(), f.id_b(), f.id_c()});
    REQUIRE(bus);
    REQUIRE_EQ(f.hub->add_target(*bus, f.id_d()), unsupported);
    REQUIRE_EQ(f.hub->remove_target(*bus, f.id_c()), unsupported);
    REQUIRE_EQ(f.info(*bus).sources.size(), std::size_t{3});
}

TEST_CASE("targets: a target that would close a loop is refused whole")
{
    target_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b()});
    REQUIRE(link);

    // c already sends to a, so a -> c would loop.
    const auto inward = f.hub->redirect(f.id_c(), f.id_a());
    REQUIRE(inward);
    REQUIRE_EQ(f.hub->add_target(*link, f.id_c()), cycle_detected);

    // a already sends to d, so the reply route d -> a would loop, although
    // the forward route a -> d on its own would not.
    const auto outward = f.hub->redirect(f.id_a(), f.id_d());
    REQUIRE(outward);
    REQUIRE_EQ(f.hub->add_target(*link, f.id_d()), cycle_detected);

    // Neither refusal left a route behind: the split carries each byte once.
    REQUIRE_EQ(f.info(*link).targets, std::vector<endpoint_id>{f.id_b()});
    f.a.inject_read("x");
    REQUIRE(heard(f.b, "x"));
    REQUIRE(heard(f.d, "x"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{1});
    REQUIRE_EQ(f.info(*outward).stats.bytes, std::uint64_t{1});
    REQUIRE_EQ(f.c.written(), std::string());

    f.d.inject_read("y");
    REQUIRE(wait_until([&] { return f.d.endpoint->stats().bytes_rx == 1; },
                       2000ms));
    f.c.inject_read("z");
    REQUIRE(heard(f.a, "z"));
    REQUIRE_EQ(f.info(*link).stats.bytes, std::uint64_t{1});
}

TEST_CASE("targets: a loop through a bus is refused too")
{
    target_fixture f;
    auto e = add_mock(*f.hub, "e");
    const auto link = f.hub->split(f.id_a(), {f.id_b()});
    REQUIRE(link);

    // c and d share a bus and d sends to a: a -> c would come back by d.
    REQUIRE(f.hub->bus({f.id_c(), f.id_d()}));
    REQUIRE(f.hub->redirect(f.id_d(), f.id_a()));
    REQUIRE_EQ(f.hub->add_target(*link, f.id_c()), cycle_detected);
    REQUIRE_EQ(f.info(*link).targets, std::vector<endpoint_id>{f.id_b()});

    // An endpoint with no way back is still welcome.
    REQUIRE(f.hub->add_target(*link, e.endpoint->id()));
}

TEST_CASE("targets: removing the owner releases the link")
{
    target_fixture f;
    link_options options;
    options.policy = merge_policy::owner;
    const auto link =
        f.hub->split(f.id_a(), {f.id_b(), f.id_c(), f.id_d()}, options);
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_c()));

    // Removing another target leaves the owner alone.
    REQUIRE(f.hub->remove_target(*link, f.id_d()));
    REQUIRE_EQ(f.info(*link).owner, f.id_c());

    REQUIRE(f.hub->remove_target(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});

    // A target that is added owns nothing until it is given the link or
    // sends first.
    REQUIRE(f.hub->add_target(*link, f.id_c()));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
    REQUIRE(f.hub->set_owner(*link, f.id_c()));
    f.b.inject_read("no");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 2; }, 2000ms));
    f.c.inject_read("yes");
    REQUIRE(heard(f.a, "yes"));
}

TEST_CASE("targets: bytes still delayed for a removed target are discarded")
{
    target_fixture f;
    link_options options;
    options.delay_ms = 150;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, options);
    REQUIRE(link);

    f.a.inject_read("held");
    REQUIRE(wait_until([&] { return f.a.endpoint->stats().bytes_rx == 4; },
                       2000ms));
    REQUIRE(f.hub->remove_target(*link, f.id_c()));

    // b is the clock: its copy of the same chunk was due at the same moment,
    // and its copy of the next one a whole delay later.
    REQUIRE(heard(f.b, "held"));
    f.a.inject_read("next");
    REQUIRE(heard(f.b, "heldnext"));
    REQUIRE_EQ(f.c.written(), std::string());

    // A target added again starts with an empty line.
    REQUIRE(f.hub->add_target(*link, f.id_c()));
    f.a.inject_read("new");
    REQUIRE(heard(f.c, "new"));
}

TEST_CASE("targets: a new target sees the source's lines at once")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    const auto link = hub->split(a->id(), {b->id()});
    REQUIRE(link);

    // A loopback port starts with its inputs high.  The source raises RTS
    // only, so a port that is wired to it shows CTS and nothing else.
    REQUIRE(!a->set_line(control_line::rts, true));
    REQUIRE(b->lines().cts());
    REQUIRE(!b->lines().dsr());
    REQUIRE(c->lines().dsr());

    // No line changes after this, yet c is told.
    REQUIRE(hub->add_target(*link, c->id()));
    const control_lines seen = c->lines();
    REQUIRE(seen.cts());
    REQUIRE(!seen.dsr());
    REQUIRE(!seen.dcd());

    // The source keeps what it showed: the new target has changed no line.
    REQUIRE(a->lines().dsr());
}

TEST_CASE("targets: a removed target loses its inputs, as a pulled cable")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    const auto link = hub->split(a->id(), {b->id(), c->id()});
    REQUIRE(link);

    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(wait_until([&] { return b->lines().dsr() && c->lines().dsr(); },
                       2000ms));

    // b raises DTR, then c raises RTS: the source shows the last to change.
    REQUIRE(!b->set_line(control_line::dtr, true));
    REQUIRE(!c->set_line(control_line::rts, true));
    REQUIRE(a->lines().cts());
    REQUIRE(!a->lines().dsr());

    REQUIRE(hub->remove_target(*link, c->id()));

    // c's cable is out; b's is not; and the source shows b again.
    REQUIRE(!c->lines().dsr());
    REQUIRE(!c->lines().dcd());
    REQUIRE(c->lines().rts());
    REQUIRE(b->lines().dsr());
    REQUIRE(a->lines().dsr());
    REQUIRE(!a->lines().cts());
}

TEST_CASE("targets: a removed target keeps the lines another link drives")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    const auto d = add_loopback(*hub, "d");
    const auto link = hub->split(a->id(), {b->id(), c->id()});
    REQUIRE(link);
    REQUIRE(hub->redirect(d->id(), c->id()));

    REQUIRE(!d->set_line(control_line::rts, true));
    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(c->lines().dsr());

    REQUIRE(hub->remove_target(*link, c->id()));
    // d still holds c's CTS up; a's DTR no longer reaches it.
    REQUIRE(c->lines().cts());
    REQUIRE(!c->lines().dsr());
}

TEST_CASE("targets: a link without modem lines moves none")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto c = add_loopback(*hub, "c");
    link_options options;
    options.emulate_modem_lines = false;
    const auto link = hub->split(a->id(), {b->id()}, options);
    REQUIRE(link);
    REQUIRE(!a->set_line(control_line::dtr, true));

    const control_lines before = c->lines();
    REQUIRE(hub->add_target(*link, c->id()));
    REQUIRE_EQ(c->lines().bits, before.bits);
    REQUIRE(hub->remove_target(*link, c->id()));
    REQUIRE_EQ(c->lines().bits, before.bits);
}
