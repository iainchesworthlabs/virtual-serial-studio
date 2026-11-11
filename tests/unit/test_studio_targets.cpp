//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// studio::add_target and studio::remove_target, and what a saved topology
// keeps of a link whose targets changed.

#include "support/test_support.hpp"

#include <vss/config/document.hpp>
#include <vss/studio/studio.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace vss;

namespace
{

struct ports_fixture
{
    studio app;

    ports_fixture()
    {
        for (const char* name : {"device", "one", "two", "three"})
            REQUIRE(app.create_mock_port(name));
    }

    config::link_entry only_link() const
    {
        const auto saved = app.snapshot();
        REQUIRE_EQ(saved.links.size(), std::size_t{1});
        return saved.links.front();
    }
};

using names = std::vector<std::string>;
} // namespace

TEST_CASE("studio targets: ports are added and removed by name")
{
    ports_fixture f;
    const auto link = f.app.split("device", {"one"});
    REQUIRE(link);

    REQUIRE(f.app.add_target(*link, "two"));
    REQUIRE(f.app.add_target(*link, "three"));
    REQUIRE_EQ(f.only_link().targets, (names{"one", "two", "three"}));

    REQUIRE(f.app.remove_target(*link, "two"));
    REQUIRE_EQ(f.only_link().targets, (names{"one", "three"}));
    // The port is still there, with no link.
    REQUIRE(f.app.find("two") != nullptr);

    REQUIRE_EQ(f.app.add_target(*link, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_target(*link, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_target(link_id{9999}, "two"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_target(*link, "one"),
               make_error_code(errc::already_exists));
    REQUIRE_EQ(f.app.remove_target(*link, "device"),
               make_error_code(errc::invalid_argument));
}

TEST_CASE("studio targets: the saved topology follows the target list")
{
    ports_fixture f;
    const auto link = f.app.connect("device", "one");
    REQUIRE(link);
    REQUIRE_EQ(f.only_link().mode, std::string("duplex"));

    // A second target makes the two-way link a split in the file, and
    // taking it away makes it a two-way link again.
    REQUIRE(f.app.add_target(*link, "two"));
    auto entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("split"));
    REQUIRE_EQ(entry.sources, names{"device"});
    REQUIRE_EQ(entry.targets, (names{"one", "two"}));

    REQUIRE(f.app.remove_target(*link, "one"));
    entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("duplex"));
    REQUIRE_EQ(entry.targets, names{"two"});
}

TEST_CASE("studio targets: a changed link survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.split("device", {"one", "two"});
    REQUIRE(link);
    REQUIRE(f.app.add_target(*link, "three"));
    REQUIRE(f.app.remove_target(*link, "one"));

    const auto saved = f.app.snapshot();
    REQUIRE(!config::validate(saved));
    const auto text = config::to_json(saved);
    REQUIRE(text);
    const auto parsed = config::from_json(*text);
    REQUIRE(parsed);

    studio again;
    REQUIRE(again.apply(*parsed));
    const auto links = again.link_infos();
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links.front().mode, link_mode::duplex);
    REQUIRE_EQ(links.front().sources.size(), std::size_t{1});
    REQUIRE_EQ(links.front().targets.size(), std::size_t{2});
    REQUIRE_EQ(again.snapshot().links.front().targets, (names{"two", "three"}));
}
