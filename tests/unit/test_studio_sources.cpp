//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// studio::add_source, remove_source, add_member and remove_member, and what a
// saved topology keeps of a link whose sources or members changed.

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
        for (const char* name : {"device", "one", "two", "three", "four"})
            REQUIRE(app.create_mock_port(name));
    }

    config::link_entry only_link() const
    {
        const auto saved = app.snapshot();
        REQUIRE_EQ(saved.links.size(), std::size_t{1});
        return saved.links.front();
    }

    /// Saves the topology, reads it back into a new studio and returns the
    /// links that studio has.
    std::vector<link_info> round_trip() const
    {
        const auto saved = app.snapshot();
        REQUIRE(!config::validate(saved));
        const auto text = config::to_json(saved);
        REQUIRE(text);
        const auto parsed = config::from_json(*text);
        REQUIRE(parsed);

        studio again;
        REQUIRE(again.apply(*parsed));
        const auto links = again.link_infos();
        const auto snapshot = again.snapshot();
        REQUIRE_EQ(snapshot.links.size(), saved.links.size());
        for (std::size_t index = 0; index < saved.links.size(); ++index)
        {
            REQUIRE_EQ(snapshot.links[index].mode, saved.links[index].mode);
            REQUIRE_EQ(snapshot.links[index].sources,
                       saved.links[index].sources);
            REQUIRE_EQ(snapshot.links[index].targets,
                       saved.links[index].targets);
            REQUIRE_EQ(snapshot.links[index].members,
                       saved.links[index].members);
            REQUIRE_EQ(snapshot.links[index].emulate_modem_lines,
                       saved.links[index].emulate_modem_lines);
        }
        return links;
    }
};

using names = std::vector<std::string>;
} // namespace

TEST_CASE("studio sources: ports are added and removed by name")
{
    ports_fixture f;
    const auto link = f.app.join({"one", "two"}, "device");
    REQUIRE(link);

    REQUIRE(f.app.add_source(*link, "three"));
    REQUIRE(f.app.add_source(*link, "four"));
    REQUIRE_EQ(f.only_link().sources, (names{"one", "two", "three", "four"}));

    REQUIRE(f.app.remove_source(*link, "two"));
    REQUIRE_EQ(f.only_link().sources, (names{"one", "three", "four"}));
    // The port is still there, with no link.
    REQUIRE(f.app.find("two") != nullptr);

    REQUIRE_EQ(f.app.add_source(*link, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_source(*link, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_source(link_id{9999}, "two"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_source(link_id{9999}, "one"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_source(*link, "one"),
               make_error_code(errc::already_exists));
    REQUIRE_EQ(f.app.add_source(*link, "device"),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(f.app.remove_source(*link, "two"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_source(*link, "device"),
               make_error_code(errc::not_found));
}

TEST_CASE("studio sources: a link keeps at least one source")
{
    ports_fixture f;
    const auto link = f.app.join({"one", "two"}, "device");
    REQUIRE(link);
    REQUIRE(f.app.remove_source(*link, "one"));
    REQUIRE_EQ(f.app.remove_source(*link, "two"),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(f.only_link().sources, names{"two"});
}

TEST_CASE("studio sources: duplex links take no sources")
{
    ports_fixture f;
    const auto link = f.app.split("device", {"one", "two"});
    REQUIRE(link);
    REQUIRE_EQ(f.app.add_source(*link, "three"),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(f.app.remove_source(*link, "device"),
               make_error_code(errc::unsupported));
}

TEST_CASE("studio sources: the saved topology follows the source list")
{
    ports_fixture f;
    const auto link = f.app.redirect("one", "device");
    REQUIRE(link);
    REQUIRE_EQ(f.only_link().mode, std::string("redirect"));

    // A second source makes the redirect a join in the file, and taking it
    // away makes it a redirect again.
    REQUIRE(f.app.add_source(*link, "two"));
    auto entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("join"));
    REQUIRE_EQ(entry.sources, (names{"one", "two"}));
    REQUIRE_EQ(entry.targets, names{"device"});
    REQUIRE(entry.emulate_modem_lines);

    REQUIRE(f.app.remove_source(*link, "one"));
    entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("redirect"));
    REQUIRE_EQ(entry.sources, names{"two"});
}

TEST_CASE("studio sources: a monitor with two sources is a join without lines")
{
    ports_fixture f;
    const auto link = f.app.monitor("one", "device");
    REQUIRE(link);
    REQUIRE_EQ(f.only_link().mode, std::string("monitor"));

    REQUIRE(f.app.add_source(*link, "two"));
    auto entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("join"));
    REQUIRE(!entry.emulate_modem_lines);
    REQUIRE_EQ(entry.sources, (names{"one", "two"}));

    // The file reads back as the same link.
    const auto links = f.round_trip();
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links.front().sources.size(), std::size_t{2});
    REQUIRE(!links.front().options.emulate_modem_lines);

    // And with one source again it is a monitor, which is what it was.
    REQUIRE(f.app.remove_source(*link, "one"));
    entry = f.only_link();
    REQUIRE_EQ(entry.mode, std::string("monitor"));
    REQUIRE_EQ(entry.sources, names{"two"});
    f.round_trip();
}

TEST_CASE("studio sources: a changed join survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.join({"one", "two"}, "device");
    REQUIRE(link);

    // Every state the link passes through is a valid file.
    REQUIRE(f.app.add_source(*link, "three"));
    f.round_trip();
    REQUIRE(f.app.remove_source(*link, "one"));
    f.round_trip();
    REQUIRE(f.app.add_source(*link, "four"));
    f.round_trip();
    REQUIRE(f.app.remove_source(*link, "two"));
    f.round_trip();
    REQUIRE(f.app.remove_source(*link, "three"));
    const auto links = f.round_trip();

    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links.front().sources.size(), std::size_t{1});
    REQUIRE_EQ(links.front().targets.size(), std::size_t{1});
    REQUIRE_EQ(f.only_link().mode, std::string("redirect"));
    REQUIRE_EQ(f.only_link().sources, names{"four"});
}

TEST_CASE("studio sources: the owner and the delay are kept in the file")
{
    // A join with the owner policy and a delay, made through a document.
    config::document document;
    document.name = "sources";
    for (const char* name : {"one", "two", "three", "device"})
    {
        config::port_entry port;
        port.name = name;
        port.kind = "mock";
        document.ports.push_back(port);
    }
    config::link_entry entry;
    entry.mode = "join";
    entry.sources = {"one", "two"};
    entry.targets = {"device"};
    entry.policy = merge_policy::owner;
    entry.delay_ms = 7;
    entry.jitter_ms = 3;
    document.links.push_back(entry);

    studio app;
    REQUIRE(app.apply(document));
    const auto id = app.link_infos().front().id;
    REQUIRE(app.add_source(id, "three"));

    const auto saved = app.snapshot();
    REQUIRE(!config::validate(saved));
    REQUIRE_EQ(saved.links.size(), std::size_t{1});
    REQUIRE_EQ(saved.links.front().sources, (names{"one", "two", "three"}));
    REQUIRE(saved.links.front().policy == merge_policy::owner);
    REQUIRE_EQ(saved.links.front().delay_ms, 7u);
    REQUIRE_EQ(saved.links.front().jitter_ms, 3u);
}

TEST_CASE("studio members: ports are added and removed by name")
{
    ports_fixture f;
    const auto bus = f.app.bus({"one", "two"});
    REQUIRE(bus);
    REQUIRE_EQ(f.only_link().mode, std::string("bus"));

    REQUIRE(f.app.add_member(*bus, "three"));
    REQUIRE(f.app.add_member(*bus, "four"));
    REQUIRE_EQ(f.only_link().members, (names{"one", "two", "three", "four"}));

    REQUIRE(f.app.remove_member(*bus, "two"));
    REQUIRE_EQ(f.only_link().members, (names{"one", "three", "four"}));
    // The port is still there, with no link.
    REQUIRE(f.app.find("two") != nullptr);
    f.round_trip();

    REQUIRE_EQ(f.app.add_member(*bus, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_member(*bus, "nobody"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_member(link_id{9999}, "two"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.remove_member(link_id{9999}, "one"),
               make_error_code(errc::not_found));
    REQUIRE_EQ(f.app.add_member(*bus, "one"),
               make_error_code(errc::already_exists));
    REQUIRE_EQ(f.app.remove_member(*bus, "two"),
               make_error_code(errc::not_found));

    // A bus keeps at least two members.
    REQUIRE(f.app.remove_member(*bus, "one"));
    REQUIRE_EQ(f.app.remove_member(*bus, "three"),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(f.only_link().members, (names{"three", "four"}));
}

TEST_CASE("studio members: links that are not buses take no members")
{
    ports_fixture f;
    const auto link = f.app.join({"one", "two"}, "device");
    REQUIRE(link);
    REQUIRE_EQ(f.app.add_member(*link, "three"),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(f.app.remove_member(*link, "one"),
               make_error_code(errc::unsupported));
}

TEST_CASE("studio members: a changed bus survives the JSON round trip")
{
    ports_fixture f;
    const auto bus = f.app.bus({"one", "two"});
    REQUIRE(bus);

    REQUIRE(f.app.add_member(*bus, "three"));
    f.round_trip();
    REQUIRE(f.app.remove_member(*bus, "one"));
    f.round_trip();
    REQUIRE(f.app.add_member(*bus, "one"));
    const auto links = f.round_trip();

    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(links.front().mode, link_mode::bus);
    REQUIRE_EQ(links.front().sources.size(), std::size_t{3});
    REQUIRE_EQ(f.only_link().members, (names{"two", "three", "one"}));
}
