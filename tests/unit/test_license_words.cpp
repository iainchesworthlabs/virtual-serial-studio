//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plain words the command line tools print for a licence's state and
// reason.
//

#include <vss/license/words.hpp>

#include "test_framework.hpp"

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace lic = vss::license;

TEST_CASE("licence words: every state has a word")
{
    REQUIRE_EQ(std::string(lic::state_word(lic::state::none)), "none");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::valid)), "valid");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::expiring)), "expiring");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::grace)), "grace");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::expired)), "expired");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::not_yet_valid)),
               "not yet valid");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::clock_suspect)),
               "clock suspect");
    REQUIRE_EQ(std::string(lic::state_word(lic::state::invalid)), "invalid");
}

TEST_CASE("licence words: every state has one name for a program to read")
{
    // The names the control gateway's status uses. No spaces.
    REQUIRE_EQ(std::string(lic::state_name(lic::state::none)), "none");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::valid)), "valid");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::expiring)), "expiring");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::grace)), "grace");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::expired)), "expired");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::not_yet_valid)),
               "not_yet_valid");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::clock_suspect)),
               "clock_suspect");
    REQUIRE_EQ(std::string(lic::state_name(lic::state::invalid)), "invalid");
}

TEST_CASE("licence words: the edition is community unless a licence is in "
          "force")
{
    REQUIRE_EQ(std::string(lic::edition_name(lic::no_licence())), "community");

    lic::licence value;
    value.licence_id = "VSS-2026-000123";
    value.licensee_name = "Example Lab Pty Ltd";
    value.starts_at = *lic::parse_time("2026-11-13T00:00:00Z");
    value.issued_at = value.starts_at;
    value.expires_at = *lic::parse_time("2027-11-13T00:00:00Z");
    value.grace_days = 28;
    value.features = {"plugins.official"};
    const auto judged = [&](std::string_view now)
    {
        lic::context where;
        where.now = *lic::parse_time(now);
        return lic::evaluate(value, nullptr, where);
    };

    REQUIRE_EQ(std::string(lic::edition_name(judged("2027-01-01T00:00:00Z"))),
               "pro");
    // In its grace period the licence still works.
    REQUIRE_EQ(std::string(lic::edition_name(judged("2027-12-01T00:00:00Z"))),
               "pro");
    // Ended, or not started: nothing is in force.
    REQUIRE_EQ(std::string(lic::edition_name(judged("2028-06-01T00:00:00Z"))),
               "community");
    REQUIRE_EQ(std::string(lic::edition_name(judged("2026-01-01T00:00:00Z"))),
               "community");

    value.kind = lic::licence_kind::trial;
    REQUIRE_EQ(std::string(lic::edition_name(judged("2027-01-01T00:00:00Z"))),
               "pro");
    value.kind = lic::licence_kind::enterprise;
    value.edition = lic::edition::enterprise;
    REQUIRE_EQ(std::string(lic::edition_name(judged("2027-01-01T00:00:00Z"))),
               "enterprise");
}

TEST_CASE("licence words: every reason has a sentence")
{
    const lic::reason all[] = {lic::reason::ok,
                               lic::reason::no_licence,
                               lic::reason::expired,
                               lic::reason::not_yet_valid,
                               lic::reason::clock_suspect,
                               lic::reason::malformed,
                               lic::reason::newer_version,
                               lic::reason::bad_signature,
                               lic::reason::revoked,
                               lic::reason::wrong_computer,
                               lic::reason::not_in_licence};
    for (std::size_t first = 0; first < std::size(all); ++first)
    {
        const std::string_view sentence = lic::reason_sentence(all[first]);
        REQUIRE(!sentence.empty());
        REQUIRE(sentence != "unknown");
        // Two reasons never share a sentence.
        for (std::size_t second = first + 1; second < std::size(all); ++second)
            REQUIRE(sentence != lic::reason_sentence(all[second]));
    }
    REQUIRE_EQ(std::string(lic::reason_sentence(lic::reason::ok)), "ok");
    REQUIRE_EQ(std::string(lic::reason_sentence(lic::reason::revoked)),
               "the licence or its key was withdrawn");
}
