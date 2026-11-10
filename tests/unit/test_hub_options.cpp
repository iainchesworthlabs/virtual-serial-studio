//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// hub::set_link_options: the rules for each option and each link mode.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <chrono>
#include <string>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

struct options_fixture
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

/// True once `port` has been written exactly `text`.
bool heard(const mock_endpoint& port, const std::string& text)
{
    return wait_until([&] { return port.written() == text; }, 2000ms);
}

/// True once `link` has been handed `messages` deliveries in all.  The hub
/// counts a delivery when it routes the chunk, under the same lock that a
/// change of options takes, so after this the chunk is routed by the table as
/// it stands now, and a change that follows meets it on its way.
bool routed(const options_fixture& f, link_id link, std::uint64_t messages)
{
    return wait_until([&] { return f.info(link).stats.messages == messages; },
                      2000ms);
}

void same_options(const link_options& actual, const link_options& expected)
{
    REQUIRE_EQ(actual.emulate_modem_lines, expected.emulate_modem_lines);
    REQUIRE(actual.wiring == expected.wiring);
    REQUIRE(actual.policy == expected.policy);
    REQUIRE_EQ(actual.delay_ms, expected.delay_ms);
    REQUIRE_EQ(actual.jitter_ms, expected.jitter_ms);
}

/// A wiring that is not the null-modem cable.
modem_wiring crossed()
{
    modem_wiring wiring;
    wiring.dtr_to_dsr = false;
    wiring.dtr_to_dcd = false;
    wiring.dtr_to_cts = true;
    return wiring;
}

const auto not_found = make_error_code(errc::not_found);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);
} // namespace

TEST_CASE("options: a link that does not exist and bad delays are refused")
{
    options_fixture f;
    const auto link = f.hub->connect(f.id_a(), f.id_b());
    REQUIRE(link);

    REQUIRE_EQ(f.hub->set_link_options(link_id{9999}, link_options{}),
               not_found);

    link_options options;
    options.delay_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(f.hub->set_link_options(*link, options), invalid_argument);

    options.delay_ms = 10;
    options.jitter_ms = 11;
    REQUIRE_EQ(f.hub->set_link_options(*link, options), invalid_argument);

    // The limits themselves are fine, and so is jitter equal to the delay.
    options.delay_ms = max_link_delay_ms;
    options.jitter_ms = max_link_delay_ms;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE_EQ(f.info(*link).options.delay_ms, max_link_delay_ms);

    // An argument error is reported even for a link that is not there.
    options.delay_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(f.hub->set_link_options(link_id{9999}, options),
               invalid_argument);
}

TEST_CASE("options: a refused call changes nothing")
{
    options_fixture f;
    link_options before;
    before.policy = merge_policy::interleave;
    before.delay_ms = 3;
    before.jitter_ms = 1;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c(), before);
    REQUIRE(link);

    // Everything else is valid, so only an all-or-nothing call leaves the
    // policy, the lines and the wiring as they were.
    link_options bad;
    bad.policy = merge_policy::owner;
    bad.emulate_modem_lines = false;
    bad.wiring = crossed();
    bad.delay_ms = 5;
    bad.jitter_ms = 6;
    REQUIRE_EQ(f.hub->set_link_options(*link, bad), invalid_argument);
    same_options(f.info(*link).options, before);

    // The link still interleaves: both sources reach the target, nothing is
    // dropped and nobody owns it.
    f.a.inject_read("A");
    f.b.inject_read("B");
    REQUIRE(wait_until([&] { return f.c.written().size() == 2; }, 2000ms));
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{0});
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
}

namespace
{
/// Makes one kind of link; a chunk goes in at `a` and comes out at `b`.
using make_link = link_id (*)(options_fixture&);

void keeps_identity(make_link make, bool bus)
{
    options_fixture f;
    const link_id link = make(f);
    const bool lines_before = f.info(link).options.emulate_modem_lines;

    f.a.inject_read("one");
    REQUIRE(heard(f.b, "one"));
    const link_info before = f.info(link);

    // Every option changes at once, the bus's own limits aside.
    link_options options;
    options.emulate_modem_lines = !lines_before;
    options.wiring = crossed();
    options.policy = bus ? merge_policy::interleave : merge_policy::owner;
    options.delay_ms = 4;
    options.jitter_ms = 2;
    REQUIRE(f.hub->set_link_options(link, options));

    const link_info after = f.info(link);
    REQUIRE_EQ(after.id, before.id);
    REQUIRE(after.mode == before.mode);
    REQUIRE_EQ(after.sources, before.sources);
    REQUIRE_EQ(after.targets, before.targets);
    REQUIRE_EQ(after.stats.bytes, before.stats.bytes);
    REQUIRE_EQ(after.stats.messages, before.stats.messages);
    REQUIRE_EQ(f.hub->links().size(), std::size_t{1});

    link_options expected = options;
    if (bus)
    {
        // A bus has no lines to switch or wire.
        expected.emulate_modem_lines = false;
        expected.wiring = before.options.wiring;
    }
    same_options(after.options, expected);

    // And it still carries traffic, with its new delay.
    f.a.inject_read("two");
    REQUIRE(heard(f.b, "onetwo"));
    REQUIRE_EQ(f.info(link).stats.bytes,
               before.stats.bytes + 3 * before.stats.messages);
}
} // namespace

TEST_CASE("options: a connect keeps its id, ports and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->connect(f.id_a(), f.id_b()); }, false);
}

TEST_CASE("options: a split keeps its id, ports and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->split(f.id_a(), {f.id_b(), f.id_c()}); },
                   false);
}

TEST_CASE("options: a join keeps its id, ports and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->join({f.id_a(), f.id_d()}, f.id_b()); },
                   false);
}

TEST_CASE("options: a redirect keeps its id, ports and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->redirect(f.id_a(), f.id_b()); }, false);
}

TEST_CASE("options: a monitor keeps its id, ports and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->monitor(f.id_a(), f.id_b()); }, false);
}

TEST_CASE("options: a bus keeps its id, members and counters")
{
    keeps_identity([](options_fixture& f)
                   { return *f.hub->bus({f.id_a(), f.id_b(), f.id_c()}); },
                   true);
}

TEST_CASE("options: a bus stores no modem lines and takes interleave only")
{
    options_fixture f;
    const auto bus = f.hub->bus({f.id_a(), f.id_b(), f.id_c()});
    REQUIRE(bus);
    REQUIRE(!f.info(*bus).options.emulate_modem_lines);

    // Asking for lines is not an error, as on creation: they are not kept.
    link_options options;
    options.emulate_modem_lines = true;
    options.delay_ms = 50;
    REQUIRE(f.hub->set_link_options(*bus, options));
    REQUIRE(!f.info(*bus).options.emulate_modem_lines);
    REQUIRE_EQ(f.info(*bus).options.delay_ms, 50u);

    // The owner policy names one publisher, which a bus cannot have.  The
    // refusal leaves the delay as it was.
    link_options owner;
    owner.policy = merge_policy::owner;
    owner.delay_ms = 70;
    REQUIRE_EQ(f.hub->set_link_options(*bus, owner), unsupported);
    REQUIRE_EQ(f.info(*bus).options.delay_ms, 50u);
    REQUIRE(f.info(*bus).options.policy == merge_policy::interleave);

    // The arguments are checked first.
    owner.delay_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(f.hub->set_link_options(*bus, owner), invalid_argument);
}

TEST_CASE("options: a bus ignores the wiring, which set_link_wiring refuses")
{
    options_fixture f;
    const auto bus = f.hub->bus({f.id_a(), f.id_b()});
    REQUIRE(bus);
    const modem_wiring before = f.info(*bus).options.wiring;

    // The wiring of a link without lines has no meaning, so it is left as it
    // is, and the rest of the options are applied.
    link_options options;
    options.wiring = crossed();
    options.delay_ms = 9;
    REQUIRE(f.hub->set_link_options(*bus, options));
    REQUIRE(f.info(*bus).options.wiring == before);
    REQUIRE_EQ(f.info(*bus).options.delay_ms, 9u);

    // Asked for nothing else, the wiring call says so.
    REQUIRE_EQ(f.hub->set_link_wiring(*bus, crossed()), unsupported);
}

TEST_CASE("options: a bus delay can be changed and removed")
{
    options_fixture f;
    const auto bus = f.hub->bus({f.id_a(), f.id_b(), f.id_c()});
    REQUIRE(bus);

    link_options options;
    options.delay_ms = 200;
    REQUIRE(f.hub->set_link_options(*bus, options));

    const auto started = std::chrono::steady_clock::now();
    f.a.inject_read("slow");
    REQUIRE(heard(f.b, "slow"));
    REQUIRE(heard(f.c, "slow"));
    // A chunk is never early.
    REQUIRE(std::chrono::steady_clock::now() - started >= 190ms);

    // With no delay the bytes go through at once, and the old lines hold
    // nothing that could follow them.
    options.delay_ms = 0;
    REQUIRE(f.hub->set_link_options(*bus, options));
    f.b.inject_read("fast");
    REQUIRE(heard(f.a, "fast"));
    REQUIRE(heard(f.c, "slowfast"));
}

TEST_CASE("options: a monitor takes modem lines and gives them up again")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto link = hub->monitor(a->id(), b->id());
    REQUIRE(link);
    REQUIRE(!hub->find_link(*link)->options.emulate_modem_lines);

    // A loopback port starts with its inputs high, and the monitor leaves
    // them so.
    REQUIRE(b->lines().cts());
    REQUIRE(b->lines().dsr());

    link_options on;
    on.emulate_modem_lines = true;
    REQUIRE(hub->set_link_options(*link, on));
    REQUIRE(hub->find_link(*link)->options.emulate_modem_lines);

    // The tap is told what the source drives now: nothing.
    REQUIRE(!b->lines().cts());
    REQUIRE(!b->lines().dsr());
    REQUIRE(!a->set_line(control_line::rts, true));
    REQUIRE(b->lines().cts());

    // Switched off again, the tap's inputs drop and it follows nothing.
    link_options off;
    off.emulate_modem_lines = false;
    REQUIRE(hub->set_link_options(*link, off));
    REQUIRE(!b->lines().cts());
    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(!b->lines().dsr());

    // It is still the same one-way link, with no route back.
    REQUIRE(hub->find_link(*link)->mode == link_mode::one_way);
    REQUIRE_EQ(hub->find_link(*link)->targets,
               std::vector<endpoint_id>{b->id()});
}

TEST_CASE("options: a join takes the owner policy and gives it up")
{
    options_fixture f;
    const auto link = f.hub->join({f.id_a(), f.id_b()}, f.id_c());
    REQUIRE(link);

    f.a.inject_read("A");
    f.b.inject_read("B");
    REQUIRE(wait_until([&] { return f.c.written().size() == 2; }, 2000ms));

    // Into the owner policy: nobody owns the link, and the first to send
    // takes it.
    link_options options;
    options.policy = merge_policy::owner;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
    f.a.inject_read("1");
    REQUIRE(wait_until([&] { return f.c.written().size() == 3; }, 2000ms));
    REQUIRE_EQ(f.info(*link).owner, f.id_a());
    f.b.inject_read("2");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 1; }, 2000ms));
    REQUIRE_EQ(f.c.written().size(), std::size_t{3});

    // Out of it, the owner is cleared, so the link names nobody it no
    // longer has, and every route forwards again.
    options.policy = merge_policy::interleave;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
    f.b.inject_read("3");
    f.a.inject_read("4");
    REQUIRE(wait_until([&] { return f.c.written().size() == 5; }, 2000ms));
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{1});
    // Interleaving takes no owner, however many publishers send.
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
}

TEST_CASE("options: a split arbitrates the replies and never the source")
{
    options_fixture f;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()});
    REQUIRE(link);

    link_options options;
    options.policy = merge_policy::owner;
    REQUIRE(f.hub->set_link_options(*link, options));

    // The first target to answer takes the link; the other is dropped.
    f.b.inject_read("x");
    REQUIRE(heard(f.a, "x"));
    REQUIRE_EQ(f.info(*link).owner, f.id_b());
    f.c.inject_read("yz");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 2; }, 2000ms));

    // What the source sends goes to every target whoever owns the link, and
    // is never counted as dropped.
    f.a.inject_read("go");
    REQUIRE(heard(f.b, "go"));
    REQUIRE(heard(f.c, "go"));
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{2});
    REQUIRE_EQ(f.info(*link).owner, f.id_b());

    // Back to interleaving: the owner is cleared and both targets answer.
    options.policy = merge_policy::interleave;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
    f.c.inject_read("1");
    f.b.inject_read("2");
    REQUIRE(wait_until([&] { return f.a.written().size() == 3; }, 2000ms));
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{2});
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
}

TEST_CASE("options: a connect under the owner policy drops nothing it sends")
{
    options_fixture f;
    const auto link = f.hub->connect(f.id_a(), f.id_b());
    REQUIRE(link);

    link_options options;
    options.policy = merge_policy::owner;
    REQUIRE(f.hub->set_link_options(*link, options));

    // The source reaches its target whoever owns the link, and takes nothing.
    f.a.inject_read("ab");
    REQUIRE(heard(f.b, "ab"));
    REQUIRE_EQ(f.info(*link).owner, endpoint_id{0});
    // The target's reply is the arbitrated one; as the only one, it owns it.
    f.b.inject_read("cd");
    REQUIRE(heard(f.a, "cd"));
    REQUIRE_EQ(f.info(*link).owner, f.id_b());
    REQUIRE_EQ(f.info(*link).stats.dropped, std::uint64_t{0});
}

TEST_CASE("options: a redirect and a monitor are owned by their source")
{
    options_fixture f;
    const auto redirected = f.hub->redirect(f.id_a(), f.id_b());
    const auto watched = f.hub->monitor(f.id_c(), f.id_d());
    REQUIRE(redirected);
    REQUIRE(watched);

    link_options options;
    options.policy = merge_policy::owner;
    REQUIRE(f.hub->set_link_options(*redirected, options));
    options.emulate_modem_lines = false;
    REQUIRE(f.hub->set_link_options(*watched, options));

    f.a.inject_read("r");
    f.c.inject_read("m");
    REQUIRE(heard(f.b, "r"));
    REQUIRE(heard(f.d, "m"));
    REQUIRE_EQ(f.info(*redirected).owner, f.id_a());
    REQUIRE_EQ(f.info(*watched).owner, f.id_c());
    REQUIRE_EQ(f.info(*redirected).stats.dropped, std::uint64_t{0});
    REQUIRE_EQ(f.info(*watched).stats.dropped, std::uint64_t{0});
}

TEST_CASE("options: the owner is kept across a change that keeps the policy")
{
    options_fixture f;
    link_options options;
    options.policy = merge_policy::owner;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, options);
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_c()));

    options.delay_ms = 5;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE_EQ(f.info(*link).owner, f.id_c());

    // And the routes are still arbitrated: b is not the owner.
    f.b.inject_read("no");
    REQUIRE(
        wait_until([&] { return f.info(*link).stats.dropped == 2; }, 2000ms));
    f.c.inject_read("yes");
    REQUIRE(heard(f.a, "yes"));
}

TEST_CASE("options: a changed delay discards the bytes the old lines hold")
{
    options_fixture f;
    link_options slow;
    slow.delay_ms = 300;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, slow);
    // Another link with the same delay is the clock, and its bytes are not
    // touched by a change to the first.
    const auto clock = f.hub->redirect(f.id_a(), f.id_d(), slow);
    REQUIRE(link);
    REQUIRE(clock);

    f.a.inject_read("held");
    REQUIRE(routed(f, *link, 2));

    link_options fast;
    REQUIRE(f.hub->set_link_options(*link, fast));

    // The link takes chunks again at once, and they arrive ahead of what the
    // old lines would have written, which is gone.
    f.a.inject_read("next");
    REQUIRE(heard(f.b, "next"));
    REQUIRE(heard(f.c, "next"));
    // d was due "held" at the same moment b would have been, and "next" a
    // whole delay after it.
    REQUIRE(heard(f.d, "heldnext"));
    REQUIRE_EQ(f.b.written(), std::string("next"));
    REQUIRE_EQ(f.c.written(), std::string("next"));
}

TEST_CASE("options: a delay that changes to another replaces the old lines")
{
    options_fixture f;
    link_options slow;
    slow.delay_ms = 300;
    const auto link = f.hub->connect(f.id_a(), f.id_b(), slow);
    const auto clock = f.hub->redirect(f.id_a(), f.id_c(), slow);
    REQUIRE(link);
    REQUIRE(clock);

    f.a.inject_read("old");
    REQUIRE(routed(f, *link, 1));
    slow.delay_ms = 100;
    REQUIRE(f.hub->set_link_options(*link, slow));
    f.a.inject_read("new");
    REQUIRE(heard(f.b, "new"));
    // The clock hears both of them, a whole delay after each.
    REQUIRE(heard(f.c, "oldnew"));
    REQUIRE_EQ(f.b.written(), std::string("new"));
}

TEST_CASE("options: a delay that is unchanged keeps the bytes it holds")
{
    options_fixture f;
    link_options slow;
    slow.delay_ms = 300;
    const auto link = f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, slow);
    REQUIRE(link);

    f.a.inject_read("held");
    REQUIRE(routed(f, *link, 2));

    // The policy and the wiring change and the lines do not, so nothing the
    // lines hold is lost.
    link_options options = slow;
    options.policy = merge_policy::owner;
    options.wiring = crossed();
    REQUIRE(f.hub->set_link_options(*link, options));

    REQUIRE(heard(f.b, "held"));
    REQUIRE(heard(f.c, "held"));

    // And the modem lines being switched makes no difference either.
    f.a.inject_read("more");
    REQUIRE(routed(f, *link, 4));
    options.emulate_modem_lines = false;
    REQUIRE(f.hub->set_link_options(*link, options));
    REQUIRE(heard(f.b, "heldmore"));
    REQUIRE(heard(f.c, "heldmore"));
}

TEST_CASE("options: a delay added to a link is applied to what follows")
{
    options_fixture f;
    const auto link = f.hub->connect(f.id_a(), f.id_b());
    REQUIRE(link);

    f.a.inject_read("now");
    REQUIRE(heard(f.b, "now"));

    link_options slow;
    slow.delay_ms = 200;
    REQUIRE(f.hub->set_link_options(*link, slow));
    const auto started = std::chrono::steady_clock::now();
    f.a.inject_read("later");
    REQUIRE(heard(f.b, "nowlater"));
    REQUIRE(std::chrono::steady_clock::now() - started >= 190ms);

    // And the reply route has it too.
    const auto replied = std::chrono::steady_clock::now();
    f.b.inject_read("back");
    REQUIRE(heard(f.a, "back"));
    REQUIRE(std::chrono::steady_clock::now() - replied >= 190ms);
}

TEST_CASE("options: the other links keep their bytes and their options")
{
    options_fixture f;
    link_options slow;
    slow.delay_ms = 200;
    const auto changed = f.hub->redirect(f.id_a(), f.id_b(), slow);
    const auto other = f.hub->redirect(f.id_a(), f.id_c(), slow);
    REQUIRE(changed);
    REQUIRE(other);

    f.a.inject_read("held");
    REQUIRE(routed(f, *changed, 1));
    link_options fast;
    fast.policy = merge_policy::owner;
    REQUIRE(f.hub->set_link_options(*changed, fast));

    REQUIRE(heard(f.c, "held"));
    same_options(f.info(*other).options, slow);
    REQUIRE(f.info(*other).owner == endpoint_id{0});
}

TEST_CASE("options: modem lines turned on are pushed along the link")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    link_options off;
    off.emulate_modem_lines = false;
    const auto link = hub->connect(a->id(), b->id(), off);
    REQUIRE(link);

    REQUIRE(!a->set_line(control_line::dtr, true));
    // Nothing crossed: the inputs are as the ports started.
    REQUIRE(b->lines().cts());
    REQUIRE(a->lines().dsr());

    // Turned on, each end is told what the other drives now, with no line
    // having changed since: b shows a's DTR and not its RTS, and a shows b,
    // which drives nothing.
    link_options on;
    REQUIRE(hub->set_link_options(*link, on));
    REQUIRE(b->lines().dsr());
    REQUIRE(!b->lines().cts());
    REQUIRE(!a->lines().dsr());

    // And from then on they follow.
    REQUIRE(!a->set_line(control_line::rts, true));
    REQUIRE(b->lines().cts());
}

TEST_CASE("options: a link that is rewired is pushed the new wires")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto link = hub->connect(a->id(), b->id());
    REQUIRE(link);

    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(b->lines().dsr());
    REQUIRE(!b->lines().cts());

    link_options options;
    options.wiring = crossed();
    REQUIRE(hub->set_link_options(*link, options));
    REQUIRE(hub->find_link(*link)->options.wiring == crossed());

    // DTR now drives CTS and neither DSR nor DCD, as set_link_wiring does.
    REQUIRE(b->lines().cts());
    REQUIRE(!b->lines().dsr());
    REQUIRE(!b->lines().dcd());
}

TEST_CASE("options: modem lines turned off drop what the link drove")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto link = hub->connect(a->id(), b->id());
    REQUIRE(link);

    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(b->lines().dsr());
    REQUIRE(b->lines().dcd());

    link_options off;
    off.emulate_modem_lines = false;
    REQUIRE(hub->set_link_options(*link, off));

    // The cable is out: both ends lose the inputs the link drove.
    REQUIRE(!b->lines().dsr());
    REQUIRE(!b->lines().dcd());
    REQUIRE(!a->lines().dsr());
    // The outputs are the ports' own and stay.
    REQUIRE(a->lines().dtr());

    // And nothing crosses any more.
    REQUIRE(!a->set_line(control_line::rts, true));
    REQUIRE(!b->lines().cts());
}

TEST_CASE("options: modem lines turned off keep what another link drives")
{
    auto hub = start_hub();
    const auto a = add_loopback(*hub, "a");
    const auto b = add_loopback(*hub, "b");
    const auto d = add_loopback(*hub, "d");
    const auto link = hub->connect(a->id(), b->id());
    REQUIRE(link);
    REQUIRE(hub->redirect(d->id(), b->id()));

    REQUIRE(!a->set_line(control_line::dtr, true));
    REQUIRE(!d->set_line(control_line::rts, true));
    REQUIRE(b->lines().cts());

    link_options off;
    off.emulate_modem_lines = false;
    REQUIRE(hub->set_link_options(*link, off));

    // d still holds b's CTS up; a's DTR no longer reaches it.
    REQUIRE(b->lines().cts());
    REQUIRE(!b->lines().dsr());
}
