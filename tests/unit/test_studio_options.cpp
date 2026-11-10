//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// studio::set_link_options, and what a saved topology keeps of a link whose
// options changed, for each kind of link.

#include "support/test_support.hpp"

#include <vss/config/document.hpp>
#include <vss/studio/studio.hpp>

#include <string>
#include <vector>

using namespace vss;

namespace
{

const auto not_found = make_error_code(errc::not_found);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);

struct ports_fixture
{
    studio app;

    ports_fixture()
    {
        for (const char* name : {"device", "one", "two", "three"})
            REQUIRE(app.create_mock_port(name));
    }

    link_info only_link() const
    {
        const auto links = app.link_infos();
        REQUIRE_EQ(links.size(), std::size_t{1});
        return links.front();
    }

    /// Saves the topology, reads it back into a new studio, and returns the
    /// link that studio has.  The saved entry is `entry`.
    link_info round_trip(config::link_entry& entry) const
    {
        const auto saved = app.snapshot();
        REQUIRE(!config::validate(saved));
        REQUIRE_EQ(saved.links.size(), std::size_t{1});
        entry = saved.links.front();

        const auto text = config::to_json(saved);
        REQUIRE(text);
        const auto parsed = config::from_json(*text);
        REQUIRE(parsed);

        studio again;
        REQUIRE(again.apply(*parsed));
        const auto links = again.link_infos();
        REQUIRE_EQ(links.size(), std::size_t{1});

        // Saving the second studio gives the same document.
        const auto resaved = again.snapshot();
        REQUIRE_EQ(resaved.links.size(), std::size_t{1});
        REQUIRE_EQ(resaved.links.front().mode, entry.mode);
        REQUIRE_EQ(resaved.links.front().sources, entry.sources);
        REQUIRE_EQ(resaved.links.front().targets, entry.targets);
        REQUIRE_EQ(resaved.links.front().members, entry.members);
        return links.front();
    }
};

/// The options a saved file keeps; the wiring is not among them.
void saved_options(const link_info& read_back, const link_options& expected)
{
    REQUIRE_EQ(read_back.options.emulate_modem_lines,
               expected.emulate_modem_lines);
    REQUIRE(read_back.options.policy == expected.policy);
    REQUIRE_EQ(read_back.options.delay_ms, expected.delay_ms);
    REQUIRE_EQ(read_back.options.jitter_ms, expected.jitter_ms);
    // The wiring lives in the running hub only: a file loads the default.
    REQUIRE(read_back.options.wiring == modem_wiring{});
}

modem_wiring crossed()
{
    modem_wiring wiring;
    wiring.dtr_to_dsr = false;
    wiring.dtr_to_cts = true;
    return wiring;
}

/// Options that differ from the defaults in every field.
link_options busy()
{
    link_options options;
    options.emulate_modem_lines = false;
    options.wiring = crossed();
    options.policy = merge_policy::owner;
    options.delay_ms = 40;
    options.jitter_ms = 15;
    return options;
}
} // namespace

TEST_CASE("studio options: a link is changed by id, whatever its ports are")
{
    ports_fixture f;
    const auto link = f.app.connect("one", "two");
    REQUIRE(link);

    REQUIRE_EQ(f.app.set_link_options(link_id{9999}, link_options{}),
               not_found);
    link_options bad;
    bad.delay_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(f.app.set_link_options(*link, bad), invalid_argument);
    REQUIRE_EQ(f.only_link().options.delay_ms, 0u);

    REQUIRE(f.app.set_link_options(*link, busy()));
    const auto info = f.only_link();
    REQUIRE_EQ(info.id, *link);
    REQUIRE_EQ(info.options.delay_ms, 40u);
    REQUIRE_EQ(info.options.jitter_ms, 15u);
    REQUIRE(info.options.policy == merge_policy::owner);
    REQUIRE(!info.options.emulate_modem_lines);
    REQUIRE(info.options.wiring == crossed());
}

TEST_CASE("studio options: a bus takes interleave only and no modem lines")
{
    ports_fixture f;
    const auto bus = f.app.bus({"one", "two", "three"});
    REQUIRE(bus);

    link_options options = busy();
    REQUIRE_EQ(f.app.set_link_options(*bus, options), unsupported);
    REQUIRE_EQ(f.only_link().options.delay_ms, 0u);

    options.policy = merge_policy::interleave;
    options.emulate_modem_lines = true;
    REQUIRE(f.app.set_link_options(*bus, options));
    REQUIRE(!f.only_link().options.emulate_modem_lines);
    REQUIRE_EQ(f.only_link().options.delay_ms, 40u);
}

TEST_CASE("studio options: a changed two-way link survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.connect("one", "two");
    REQUIRE(link);
    REQUIRE(f.app.set_link_options(*link, busy()));

    config::link_entry entry;
    const auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("duplex"));
    REQUIRE(entry.policy == merge_policy::owner);
    REQUIRE_EQ(entry.delay_ms, 40u);
    REQUIRE_EQ(entry.jitter_ms, 15u);
    REQUIRE(!entry.emulate_modem_lines);
    saved_options(read_back, busy());
}

TEST_CASE("studio options: a changed split survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.split("device", {"one", "two"});
    REQUIRE(link);
    link_options options = busy();
    options.emulate_modem_lines = true;
    REQUIRE(f.app.set_link_options(*link, options));

    config::link_entry entry;
    const auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("split"));
    saved_options(read_back, options);
}

TEST_CASE("studio options: a changed join survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.join({"one", "two"}, "device");
    REQUIRE(link);
    // Without modem lines it is still a join: many sources are one.
    REQUIRE(f.app.set_link_options(*link, busy()));

    config::link_entry entry;
    auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("join"));
    REQUIRE(!entry.emulate_modem_lines);
    saved_options(read_back, busy());

    link_options options = busy();
    options.emulate_modem_lines = true;
    options.policy = merge_policy::interleave;
    options.delay_ms = 0;
    options.jitter_ms = 0;
    REQUIRE(f.app.set_link_options(*link, options));
    read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("join"));
    saved_options(read_back, options);
}

TEST_CASE("studio options: a changed redirect survives the JSON round trip")
{
    ports_fixture f;
    const auto link = f.app.redirect("one", "device");
    REQUIRE(link);
    link_options options = busy();
    options.emulate_modem_lines = true;
    REQUIRE(f.app.set_link_options(*link, options));

    config::link_entry entry;
    const auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("redirect"));
    saved_options(read_back, options);
}

TEST_CASE("studio options: a monitor becomes a redirect when lines come on")
{
    ports_fixture f;
    const auto link = f.app.monitor("one", "device");
    REQUIRE(link);

    config::link_entry entry;
    auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("monitor"));
    REQUIRE(!read_back.options.emulate_modem_lines);

    // Every option may change on a monitor, and it is still saved as one
    // while it carries no lines.
    link_options options = busy();
    REQUIRE(f.app.set_link_options(*link, options));
    read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("monitor"));
    saved_options(read_back, options);

    // With lines on, the file has a redirect: the one flag tells them apart.
    options.emulate_modem_lines = true;
    REQUIRE(f.app.set_link_options(*link, options));
    read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("redirect"));
    saved_options(read_back, options);

    // And off again, a monitor.
    options.emulate_modem_lines = false;
    REQUIRE(f.app.set_link_options(*link, options));
    read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("monitor"));
    saved_options(read_back, options);
}

TEST_CASE("studio options: a changed bus survives the JSON round trip")
{
    ports_fixture f;
    const auto bus = f.app.bus({"one", "two", "three"});
    REQUIRE(bus);

    link_options options;
    options.delay_ms = 25;
    options.jitter_ms = 10;
    REQUIRE(f.app.set_link_options(*bus, options));

    config::link_entry entry;
    const auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("bus"));
    REQUIRE_EQ(entry.members.size(), std::size_t{3});
    options.emulate_modem_lines = false;
    saved_options(read_back, options);
}

TEST_CASE("studio options: the options change with the ports of the link")
{
    ports_fixture f;
    const auto link = f.app.split("device", {"one"});
    REQUIRE(link);
    REQUIRE(f.app.set_link_options(*link, busy()));

    // The options belong to the link: they stay when a target comes, and the
    // link is a split from then on.
    REQUIRE(f.app.add_target(*link, "two"));
    config::link_entry entry;
    const auto read_back = f.round_trip(entry);
    REQUIRE_EQ(entry.mode, std::string("split"));
    REQUIRE_EQ(entry.targets.size(), std::size_t{2});
    saved_options(read_back, busy());
}
