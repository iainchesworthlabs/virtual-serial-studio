//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/com_number_uses.hpp"
#include "backend/com_numbers.hpp"
#include "backend/fake_studio_backend.hpp"

#include "test_framework.hpp"

#include <algorithm>
#include <string>
#include <vector>

using vss::gui::com_numbers_in_use;
using vss::gui::first_com_candidate;
using vss::gui::kFirstGuiComNumber;
using vss::gui::kLastGuiComNumber;
using vss::gui::next_com_candidate;
using vss::gui::reusable_com_candidates;

TEST_CASE("com numbers: reads COM paths and ignores everything else")
{
    const auto numbers =
        com_numbers_in_use({"COM1", "com12", "COM255", "/dev/ttyUSB0", "COM",
                            "COM0", "COMx", "COM1234567", "CNCA0", "COM7 "});
    REQUIRE_EQ(numbers.size(), std::size_t{3});
    REQUIRE_EQ(numbers[0], 1U);
    REQUIRE_EQ(numbers[1], 12U);
    REQUIRE_EQ(numbers[2], 255U);
}

TEST_CASE("com numbers: a new port starts above every port in use")
{
    REQUIRE_EQ(first_com_candidate({3, 5, 7, 9, 13, 15, 16}), 17U);
    REQUIRE_EQ(first_com_candidate({1, 2}), kFirstGuiComNumber);
    REQUIRE_EQ(first_com_candidate({}), kFirstGuiComNumber);
    REQUIRE_EQ(first_com_candidate({4}), 5U);
}

TEST_CASE("com numbers: never COM1 or COM2")
{
    // Nothing in use, or only low legacy ports in use: COM3, not COM1.
    REQUIRE(first_com_candidate({}) >= 3U);
    REQUIRE(first_com_candidate({1}) >= 3U);
    REQUIRE(first_com_candidate({2}) >= 3U);
}

TEST_CASE("com numbers: falls back below the highest when nothing is above")
{
    std::vector<unsigned> in_use{kLastGuiComNumber, 3, 4};
    REQUIRE_EQ(first_com_candidate(in_use), 5U);

    in_use.clear();
    for (unsigned number = kFirstGuiComNumber; number <= kLastGuiComNumber;
         ++number)
        in_use.push_back(number);
    REQUIRE_EQ(first_com_candidate(in_use), 0U);
}

TEST_CASE("com numbers: the next candidate skips what is in use and stops")
{
    const std::vector<unsigned> in_use{3, 5, 7};
    // Refused 6: 7 is in use, so move to 8.
    REQUIRE_EQ(next_com_candidate(6, 4, in_use), 8U);
    // Refused 7: 8 is next.
    REQUIRE_EQ(next_com_candidate(7, 4, in_use), 8U);
    // Past the top it wraps to the first GUI number, skipping what is taken.
    REQUIRE_EQ(next_com_candidate(kLastGuiComNumber, 100, in_use), 4U);
    // It stops once it has gone all the way round to where it started.
    REQUIRE_EQ(next_com_candidate(9, 10, in_use), 0U);
}

TEST_CASE("com numbers: kept numbers are offered for reuse, lowest first")
{
    // COM7 and COM9 are listed, so they are not reusable; COM1 and COM2 never.
    const auto numbers = reusable_com_candidates(
        {3, 7, 9}, {12, 7, 1, 5, 9, 5, 2, 300, 4}, 10);
    REQUIRE_EQ(numbers.size(), std::size_t{3});
    REQUIRE_EQ(numbers[0], 4U);
    REQUIRE_EQ(numbers[1], 5U);
    REQUIRE_EQ(numbers[2], 12U);
}

TEST_CASE("com numbers: reuse is limited and empty when nothing is kept")
{
    REQUIRE(reusable_com_candidates({5}, {}, 6).empty());
    const auto few =
        reusable_com_candidates({}, {20, 19, 18, 17, 16, 15, 14, 13}, 3);
    REQUIRE_EQ(few.size(), std::size_t{3});
    REQUIRE_EQ(few[0], 13U);
    REQUIRE_EQ(few[2], 15U);
}

TEST_CASE("com numbers: a removed Studio port's number is free to choose")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::capabilities caps;
    caps.com_numbers = true;
    backend.set_capabilities(caps);
    const auto holder = [](unsigned number, vss::com_holder_kind kind,
                           const char* description)
    {
        vss::com_number_holder row;
        row.number = number;
        row.kind = kind;
        row.description = description;
        return row;
    };
    using kind = vss::com_holder_kind;
    backend.set_com_number_holders({
        holder(1, kind::studio_port_gone, "Virtual Serial Studio COM Port"),
        holder(2, kind::database_only, ""),
        holder(3, kind::present_device, "com0com - serial port emulator"),
        holder(4, kind::studio_port_gone, "Virtual Serial Studio COM Port"),
        holder(5, kind::name_only, ""),
        holder(10, kind::other_port_gone, "USB Serial Device (COM10)"),
    });

    const auto uses = vss::gui::com_number_uses(backend);
    // COM1, COM2 and COM4 can be used again.
    REQUIRE(!uses.contains(1));
    REQUIRE(!uses.contains(2));
    REQUIRE(!uses.contains(4));
    // The others are held, each with its own reason.
    REQUIRE(uses.at(3).holder == vss::gui::com_holder::device);
    REQUIRE(uses.at(5).holder == vss::gui::com_holder::reserved);
    REQUIRE(uses.at(10).holder == vss::gui::com_holder::unplugged);
    REQUIRE(vss::gui::com_use_text(uses.at(10)).contains(
        QStringLiteral("Unplugged Windows device USB Serial Device (COM10)")));

    // COM1 and COM2 are offered, lowest first.
    const auto free = vss::gui::free_com_numbers(uses, 1, 4);
    REQUIRE_EQ(free.size(), std::size_t{4});
    REQUIRE_EQ(free[0], 1U);
    REQUIRE_EQ(free[1], 2U);
    REQUIRE_EQ(free[2], 4U);
}
