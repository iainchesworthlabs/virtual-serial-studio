//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Which routes of a link `merge_policy::owner` arbitrates, for each link mode.

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <chrono>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace {
using namespace std::chrono_literals;

struct owner_fixture
{
    std::shared_ptr<vss::hub> hub = start_hub();
    mock_endpoint a = add_mock(*hub, "a");
    mock_endpoint b = add_mock(*hub, "b");
    mock_endpoint c = add_mock(*hub, "c");

    endpoint_id id_a() const { return a.endpoint->id(); }
    endpoint_id id_b() const { return b.endpoint->id(); }
    endpoint_id id_c() const { return c.endpoint->id(); }

    std::uint64_t dropped(link_id link) const
    {
        const auto info = hub->find_link(link);
        return info ? info->stats.dropped : 0;
    }

    endpoint_id owner(link_id link) const
    {
        const auto info = hub->find_link(link);
        return info ? info->owner : 0;
    }
};

link_options owner_options()
{
    link_options options;
    options.policy = merge_policy::owner;
    return options;
}
} // namespace

TEST_CASE("owner: on a split the rule drops the targets' replies")
{
    owner_fixture f;
    const auto link =
        f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, owner_options());
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_a()));

    f.a.inject_read("out");
    REQUIRE(wait_until([&] { return f.b.written() == "out"; }, 2000ms));
    REQUIRE(wait_until([&] { return f.c.written() == "out"; }, 2000ms));

    // The reply's publisher is a target, which is not the owner.
    f.b.inject_read("reply");
    REQUIRE(wait_until([&] { return f.dropped(*link) == 5; }, 2000ms));
    REQUIRE_EQ(f.a.written(), std::string());
}

TEST_CASE("owner: a reply takes a split that has no owner")
{
    owner_fixture f;
    const auto link =
        f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, owner_options());
    REQUIRE(link);

    f.b.inject_read("first");
    REQUIRE(wait_until([&] { return f.a.written() == "first"; }, 2000ms));
    REQUIRE_EQ(f.owner(*link), f.id_b());

    // The source is now shut out of its own link, on both of its routes.
    f.a.inject_read("data");
    REQUIRE(wait_until([&] { return f.dropped(*link) == 8; }, 2000ms));
    REQUIRE_EQ(f.b.written(), std::string());
    REQUIRE_EQ(f.c.written(), std::string());
}

TEST_CASE("owner: only the source of a split can be made its owner")
{
    owner_fixture f;
    const auto link =
        f.hub->split(f.id_a(), {f.id_b(), f.id_c()}, owner_options());
    REQUIRE(link);
    REQUIRE(f.hub->set_owner(*link, f.id_a()));
    REQUIRE_EQ(f.hub->set_owner(*link, f.id_b()),
               make_error_code(errc::invalid_argument));
}

TEST_CASE("owner: on a two-way link the first to send shuts the other out")
{
    owner_fixture f;
    const auto link = f.hub->connect(f.id_a(), f.id_b(), owner_options());
    REQUIRE(link);

    f.b.inject_read("first");
    REQUIRE(wait_until([&] { return f.a.written() == "first"; }, 2000ms));
    REQUIRE_EQ(f.owner(*link), f.id_b());

    f.a.inject_read("data");
    REQUIRE(wait_until([&] { return f.dropped(*link) == 4; }, 2000ms));
    REQUIRE_EQ(f.b.written(), std::string());
}

TEST_CASE("owner: on a join the first source to send takes the link")
{
    owner_fixture f;
    const auto link =
        f.hub->join({f.id_a(), f.id_b()}, f.id_c(), owner_options());
    REQUIRE(link);

    f.b.inject_read("mine");
    REQUIRE(wait_until([&] { return f.c.written() == "mine"; }, 2000ms));
    REQUIRE_EQ(f.owner(*link), f.id_b());

    f.a.inject_read("late");
    REQUIRE(wait_until([&] { return f.dropped(*link) == 4; }, 2000ms));
    REQUIRE_EQ(f.c.written(), std::string("mine"));

    // The target of a join sends nothing through it, so it cannot own it.
    REQUIRE_EQ(f.hub->set_owner(*link, f.id_c()),
               make_error_code(errc::invalid_argument));
}

TEST_CASE("owner: a one-way link with one source forwards everything")
{
    owner_fixture f;
    const auto link = f.hub->redirect(f.id_a(), f.id_b(), owner_options());
    REQUIRE(link);

    f.a.inject_read("one");
    REQUIRE(wait_until([&] { return f.b.written() == "one"; }, 2000ms));
    REQUIRE_EQ(f.owner(*link), f.id_a());
    REQUIRE_EQ(f.dropped(*link), std::uint64_t{0});
}

TEST_CASE("owner: a bus has no owner")
{
    owner_fixture f;
    REQUIRE_EQ(f.hub->bus({f.id_a(), f.id_b(), f.id_c()}, owner_options()),
               make_error_code(errc::unsupported));
    const auto bus = f.hub->bus({f.id_a(), f.id_b(), f.id_c()});
    REQUIRE(bus);
    REQUIRE_EQ(f.hub->set_owner(*bus, f.id_a()),
               make_error_code(errc::unsupported));
}
