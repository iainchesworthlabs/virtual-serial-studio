//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/com_numbers.hpp"

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
