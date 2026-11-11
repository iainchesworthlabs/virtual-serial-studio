//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/fake_studio_backend.hpp"
#include "backend/share_edit.hpp"

#include "test_framework.hpp"

#include <vss/core/error.hpp>

#include <QString>

namespace
{

using namespace vss::gui;

struct shared_device
{
    FakeStudioBackend backend;
    share_view share;

    explicit shared_device(int programs)
    {
        const auto shared = backend.share_serial(
            QStringLiteral("serial://COM3?baud=115200"), programs);
        REQUIRE(shared.has_value());
        share = *shared;
    }

    std::optional<share_ref> find(vss::endpoint_id port) const
    {
        return find_share(backend.ports(), backend.links(), port);
    }
};

} // namespace

TEST_CASE("share edit: finds the share from the device or any program port")
{
    shared_device fixture(3);
    for (const vss::endpoint_id id :
         {fixture.share.source, fixture.share.listeners.at(0),
          fixture.share.listeners.at(2)})
    {
        const auto found = fixture.find(id);
        REQUIRE(found.has_value());
        REQUIRE_EQ(found->link, fixture.share.link);
        REQUIRE_EQ(found->device, fixture.share.source);
        REQUIRE_EQ(found->programs.size(), 3);
    }
    REQUIRE(!fixture.find(9999).has_value());
    const auto by_link = find_share_of_link(
        fixture.backend.ports(), fixture.backend.links(), fixture.share.link);
    REQUIRE(by_link.has_value());
    REQUIRE_EQ(by_link->device, fixture.share.source);
}

TEST_CASE("share edit: a pair of virtual ports is not a share")
{
    FakeStudioBackend backend;
    const auto first = backend.create_auto_port(QStringLiteral("a"), true);
    const auto second = backend.create_auto_port(QStringLiteral("b"), true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(backend.connect_ports(*first, *second, {}).has_value());
    REQUIRE(!find_share(backend.ports(), backend.links(), *first).has_value());
    REQUIRE(!find_share(backend.ports(), backend.links(), *second).has_value());
}

TEST_CASE("share edit: an attached device is a share with one program")
{
    FakeStudioBackend backend;
    const auto device =
        backend.attach_serial(QStringLiteral("device-input"),
                              QStringLiteral("serial://COM3"), {}, true);
    const auto port = backend.create_auto_port(QStringLiteral("device"), true);
    REQUIRE(device.has_value());
    REQUIRE(port.has_value());
    REQUIRE(backend.split(*device, {*port}, {}).has_value());
    const auto found = find_share(backend.ports(), backend.links(), *port);
    REQUIRE(found.has_value());
    REQUIRE_EQ(found->device, *device);
    REQUIRE_EQ(found->programs.size(), 1);
}

TEST_CASE("share edit: adding a program keeps the old ports and the link")
{
    shared_device fixture(2);
    const auto share = fixture.find(fixture.share.source);
    REQUIRE(share.has_value());
    const auto name = next_program_name(fixture.backend.ports());
    REQUIRE_EQ(name, QStringLiteral("program-1"));
    const vss::link_id before = fixture.backend.links().front().id;

    const auto added = add_program_port(fixture.backend, *share, name, true);
    REQUIRE(added.has_value());
    REQUIRE_EQ(added->link, before);
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{4});
    REQUIRE_EQ(fixture.backend.links().size(), std::size_t{1});
    REQUIRE_EQ(fixture.backend.links().front().id, before);
    const auto link = fixture.backend.link(added->link);
    REQUIRE(link.has_value());
    REQUIRE_EQ(link->targets.size(), 3);
    REQUIRE(link->targets.contains(fixture.share.listeners.at(0)));
    REQUIRE(link->targets.contains(fixture.share.listeners.at(1)));
    REQUIRE(link->targets.contains(added->port));
    REQUIRE_EQ(link->sources.size(), 1);
    REQUIRE_EQ(link->sources.front(), fixture.share.source);
    REQUIRE_EQ(next_program_name(fixture.backend.ports()),
               QStringLiteral("program-2"));
}

TEST_CASE("share edit: a failed add leaves the share as it was")
{
    shared_device fixture(2);
    const auto share = fixture.find(fixture.share.source);
    REQUIRE(share.has_value());
    const vss::link_id before = fixture.backend.links().front().id;
    const auto unchanged = [&]
    {
        REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{3});
        REQUIRE_EQ(fixture.backend.links().size(), std::size_t{1});
        REQUIRE_EQ(fixture.backend.links().front().id, before);
        REQUIRE_EQ(fixture.backend.links().front().targets.size(), 2);
    };

    // The port cannot be created.
    fixture.backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(
        !add_program_port(fixture.backend, *share, QStringLiteral("p"), true)
             .has_value());
    unchanged();

    // The port is created but cannot be added to the link: it is removed
    // again.  The second fallible call is the add.
    fixture.backend.fail_after(1, vss::make_error_code(vss::errc::io_error));
    const auto failed =
        add_program_port(fixture.backend, *share, QStringLiteral("p"), true);
    REQUIRE(!failed.has_value());
    REQUIRE(failed.error() == vss::make_error_code(vss::errc::io_error));
    unchanged();

    // A stale share description is refused before anything is created.
    share_ref stale = *share;
    stale.link = 9999;
    REQUIRE(!add_program_port(fixture.backend, stale, QStringLiteral("p"), true)
                 .has_value());
    unchanged();
}

TEST_CASE("share edit: removing one program keeps the others shared")
{
    shared_device fixture(3);
    const auto share = fixture.find(fixture.share.source);
    REQUIRE(share.has_value());
    const vss::endpoint_id gone = fixture.share.listeners.at(1);
    const vss::link_id before = fixture.backend.links().front().id;

    const auto link = remove_program_port(fixture.backend, *share, gone);
    REQUIRE(link.has_value());
    REQUIRE_EQ(*link, before);
    REQUIRE(!fixture.backend.port(gone).has_value());
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{3});
    REQUIRE_EQ(fixture.backend.links().size(), std::size_t{1});
    const auto remaining = fixture.backend.links().front();
    REQUIRE_EQ(remaining.id, before);
    REQUIRE_EQ(remaining.targets.size(), 2);
    REQUIRE(remaining.targets.contains(fixture.share.listeners.at(0)));
    REQUIRE(remaining.targets.contains(fixture.share.listeners.at(2)));
    REQUIRE_EQ(remaining.sources.front(), fixture.share.source);

    // The device is not a program port.
    const auto again = fixture.find(fixture.share.source);
    REQUIRE(again.has_value());
    REQUIRE(!remove_program_port(fixture.backend, *again, fixture.share.source)
                 .has_value());
}

TEST_CASE("share edit: a failed remove leaves the share as it was")
{
    shared_device fixture(3);
    const auto share = fixture.find(fixture.share.source);
    REQUIRE(share.has_value());
    const vss::endpoint_id program = fixture.share.listeners.at(1);
    const vss::link_id before = fixture.backend.links().front().id;
    const auto unchanged = [&]
    {
        REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{4});
        REQUIRE_EQ(fixture.backend.links().size(), std::size_t{1});
        REQUIRE_EQ(fixture.backend.links().front().id, before);
        REQUIRE_EQ(fixture.backend.links().front().targets.size(), 3);
        REQUIRE(fixture.backend.links().front().targets.contains(program));
        REQUIRE(fixture.backend.port(program).has_value());
    };

    // The port cannot be taken off the link.
    fixture.backend.fail_next(vss::make_error_code(vss::errc::io_error));
    REQUIRE(!remove_program_port(fixture.backend, *share, program).has_value());
    unchanged();

    // It is taken off the link but cannot be removed: it goes back on.
    fixture.backend.fail_after(1, vss::make_error_code(vss::errc::io_error));
    const auto failed = remove_program_port(fixture.backend, *share, program);
    REQUIRE(!failed.has_value());
    REQUIRE(failed.error() == vss::make_error_code(vss::errc::io_error));
    unchanged();

    // A stale description is refused.
    share_ref stale = *share;
    stale.link = 9999;
    REQUIRE(!remove_program_port(fixture.backend, stale, program).has_value());
    unchanged();
}

TEST_CASE("share edit: removing the last program leaves the bare device")
{
    shared_device fixture(1);
    const auto share = fixture.find(fixture.share.source);
    REQUIRE(share.has_value());
    const auto link = remove_program_port(fixture.backend, *share,
                                          fixture.share.listeners.at(0));
    REQUIRE(link.has_value());
    REQUIRE_EQ(*link, vss::link_id{0});
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{1});
    REQUIRE(fixture.backend.links().empty());
}
