//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Versions of the update feed: Semantic Versioning 2.0.0 without build
// metadata (docs/updates.md, section 4.3).
//

#include <vss/update/version.hpp>

#include "test_framework.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace vss::update;

namespace
{

version must_parse(std::string_view text)
{
    const auto parsed = parse_version(text);
    if (!parsed)
        throw vss_test::failure("not a version: " + std::string(text));
    return *parsed;
}

} // namespace

TEST_CASE("update version: a final release reads and writes back")
{
    const auto parsed = parse_version("1.22.333");
    REQUIRE(parsed.has_value());
    REQUIRE_EQ(parsed->major, 1);
    REQUIRE_EQ(parsed->minor, 22);
    REQUIRE_EQ(parsed->patch, 333);
    REQUIRE(parsed->prerelease.empty());
    REQUIRE(!parsed->is_prerelease());
    REQUIRE_EQ(to_string(*parsed), std::string("1.22.333"));
}

TEST_CASE("update version: a pre-release reads and writes back")
{
    const auto parsed = parse_version("0.3.0-rc.1");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_prerelease());
    REQUIRE_EQ(parsed->prerelease.size(), 2);
    REQUIRE_EQ(parsed->prerelease[0], std::string("rc"));
    REQUIRE_EQ(parsed->prerelease[1], std::string("1"));
    REQUIRE_EQ(to_string(*parsed), std::string("0.3.0-rc.1"));
}

TEST_CASE("update version: every accepted spelling writes back unchanged")
{
    const std::array<std::string_view, 21> texts{
        "0.0.0",
        "0.1.0",
        "0.2.0",
        "999999.999999.999999",
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-0",
        "1.0.0-0a",
        "1.0.0-a-b",
        "1.0.0--",
        "1.0.0---.-",
        "1.0.0-0.0.0",
        "1.0.0-x.7.z.92",
        "1.0.0-A",
        "1.0.0-Z9",
        "0.1.0-dev",
        "0.1.0-rc.4",
        "10.20.30-beta.11",
        "1.0.0-99999999999999999999",
        "1.0.0-1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16"};
    for (const std::string_view text : texts)
    {
        const auto parsed = parse_version(text);
        if (!parsed)
            throw vss_test::failure("refused: " + std::string(text));
        REQUIRE_EQ(to_string(*parsed), std::string(text));
    }
}

TEST_CASE("update version: text that is not a version is refused")
{
    const std::vector<std::string> refused{
        // Shape.
        "", "1", "1.0", "1.0.0.0", "1..0", ".1.0", "1.0.", "1.0.0.", "a.b.c",
        "1.0.x",
        // Surroundings.
        " 1.0.0", "1.0.0 ", "1.0.0\n", "v1.0.0", "V1.0.0", "=1.0.0", "-1.0.0",
        "+1.0.0",
        // Build metadata.
        "1.0.0+build", "1.0.0+20260101", "1.0.0-rc.1+build", "1.0.0+",
        // Numbers.
        "01.0.0", "1.00.0", "1.0.00", "1.0.01", "-1.0.0", "1.-1.0",
        "1000000.0.0", "1.1000000.0", "1.0.1000000", "4294967296.0.0",
        "99999999999999999999.0.0", "1.0.0e1", "1.0.0x", "1,0.0",
        // Pre-releases.
        "1.0.0-", "1.0.0-.", "1.0.0-.rc", "1.0.0-rc.", "1.0.0-rc..1",
        "1.0.0-rc.01", "1.0.0-00", "1.0.0-01", "1.0.0-rc_1", "1.0.0-rc 1",
        "1.0.0-rc+1", "1.0.0-r\xc3\xa9", std::string("1.0.0-rc.1\0", 11),
        // Seventeen identifiers.
        "1.0.0-1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17"};
    for (const std::string& text : refused)
    {
        if (parse_version(text).has_value())
            throw vss_test::failure("accepted: " + text);
    }
}

TEST_CASE("update version: sixteen identifiers and sixty-four characters are "
          "the limit")
{
    REQUIRE(parse_version("1.0.0-1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16"));
    REQUIRE(!parse_version("1.0.0-1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17"));
    const std::string longest = "1.0.0-" + std::string(58, 'a');
    REQUIRE_EQ(longest.size(), 64);
    REQUIRE(parse_version(longest));
    REQUIRE(!parse_version(longest + "a"));
}

TEST_CASE("update version: precedence follows the specification")
{
    // Section 11 of the Semantic Versioning 2.0.0 specification, in order.
    const std::vector<std::string_view> in_order{
        "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    for (std::size_t i = 0; i < in_order.size(); ++i)
    {
        for (std::size_t j = 0; j < in_order.size(); ++j)
        {
            const auto order =
                compare(must_parse(in_order[i]), must_parse(in_order[j]));
            const std::string pair = std::string(in_order[i]) + " against " +
                                     std::string(in_order[j]);
            if (i < j && order >= 0)
                throw vss_test::failure(pair + " is not lower");
            if (i == j && order != 0)
                throw vss_test::failure(pair + " is not equal");
            if (i > j && order <= 0)
                throw vss_test::failure(pair + " is not higher");
        }
    }
}

TEST_CASE("update version: the numbers come before the pre-release")
{
    const std::vector<std::string_view> in_order{
        "0.0.0", "0.0.1",       "0.1.0-rc.1", "0.1.0",     "0.2.0-alpha",
        "0.2.0", "0.10.0",      "1.0.0-rc.9", "1.0.0",     "1.0.1",
        "1.1.0", "2.0.0-alpha", "2.0.0",      "999999.0.0"};
    for (std::size_t i = 1; i < in_order.size(); ++i)
    {
        const auto lower = must_parse(in_order[i - 1]);
        const auto higher = must_parse(in_order[i]);
        if (!(compare(lower, higher) < 0) || !(compare(higher, lower) > 0))
            throw vss_test::failure(std::string(in_order[i - 1]) + " and " +
                                    std::string(in_order[i]));
    }
}

TEST_CASE("update version: identifiers are compared as the specification "
          "says")
{
    const auto lower = [](std::string_view a, std::string_view b)
    {
        const auto left = must_parse(a);
        const auto right = must_parse(b);
        return compare(left, right) < 0 && compare(right, left) > 0;
    };
    // Numbers by value, not by text.
    REQUIRE(lower("1.0.0-2", "1.0.0-10"));
    REQUIRE(lower("1.0.0-9", "1.0.0-10"));
    REQUIRE(lower("1.0.0-rc.2", "1.0.0-rc.10"));
    // However long the digits are.
    REQUIRE(lower("1.0.0-99999999999999999999", "1.0.0-100000000000000000000"));
    // A number is lower than a word, even one that starts with a digit.
    REQUIRE(lower("1.0.0-999", "1.0.0-0a"));
    REQUIRE(lower("1.0.0-1", "1.0.0-a"));
    REQUIRE(lower("1.0.0-rc.1", "1.0.0-rc.a"));
    // Words by ASCII order: upper case first, and a hyphen before a digit.
    REQUIRE(lower("1.0.0-A", "1.0.0-a"));
    REQUIRE(lower("1.0.0-Z", "1.0.0-a"));
    REQUIRE(lower("1.0.0-a", "1.0.0-b"));
    REQUIRE(lower("1.0.0-a", "1.0.0-ab"));
    REQUIRE(lower("1.0.0--", "1.0.0-0a"));
    // A longer list wins when every earlier identifier is equal.
    REQUIRE(lower("1.0.0-alpha", "1.0.0-alpha.0"));
    REQUIRE(lower("1.0.0-alpha.1", "1.0.0-alpha.1.1"));
    // An earlier identifier decides before a later one.
    REQUIRE(lower("1.0.0-a.9", "1.0.0-b.1"));
    REQUIRE(lower("1.0.0-1.9", "1.0.0-2.1"));
}

TEST_CASE("update version: this project's own order")
{
    const std::vector<std::string_view> in_order{
        "0.1.0-dev", "0.1.0-rc.1", "0.1.0-rc.4", "0.1.0-rc.5",
        "0.1.0",     "0.2.0-dev",  "0.2.0-rc.1", "0.2.0"};
    for (std::size_t i = 1; i < in_order.size(); ++i)
    {
        const auto lower = must_parse(in_order[i - 1]);
        const auto higher = must_parse(in_order[i]);
        if (!(compare(lower, higher) < 0) || !(compare(higher, lower) > 0))
            throw vss_test::failure(std::string(in_order[i - 1]) + " and " +
                                    std::string(in_order[i]));
    }
    REQUIRE(compare(must_parse("0.1.0-rc.4"), must_parse("0.1.0-rc.5")) < 0);
    REQUIRE(compare(must_parse("0.1.0-rc.5"), must_parse("0.1.0")) < 0);
    REQUIRE(compare(must_parse("0.1.0-dev"), must_parse("0.1.0-rc.1")) < 0);
}

TEST_CASE("update version: equal versions compare equal and are ==")
{
    const auto left = must_parse("0.3.0-rc.1");
    const auto right = must_parse("0.3.0-rc.1");
    REQUIRE(compare(left, right) == 0);
    REQUIRE(left == right);
    REQUIRE(must_parse("1.2.3") == must_parse("1.2.3"));
    REQUIRE(must_parse("1.2.3") != must_parse("1.2.4"));
    REQUIRE(must_parse("1.2.3") != must_parse("1.2.3-rc.1"));
    REQUIRE(must_parse("1.2.3-rc.1") != must_parse("1.2.3-rc.2"));
}

TEST_CASE("update version: a value made by hand compares as the one read")
{
    const version made{1, 2, 3, {"rc", "4"}};
    REQUIRE(made == must_parse("1.2.3-rc.4"));
    REQUIRE(compare(made, must_parse("1.2.3-rc.4")) == 0);
    REQUIRE_EQ(to_string(made), std::string("1.2.3-rc.4"));
    REQUIRE_EQ(to_string(version{}), std::string("0.0.0"));
    REQUIRE(!version{}.is_prerelease());
}

TEST_CASE("update version: only the single identifier dev is a developer "
          "build")
{
    REQUIRE(is_developer_build(must_parse("0.1.0-dev")));
    REQUIRE(is_developer_build(must_parse("12.0.3-dev")));
    REQUIRE(!is_developer_build(must_parse("0.1.0")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-rc.1")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-dev.1")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-dev.rc")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-rc.dev")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-DEV")));
    REQUIRE(!is_developer_build(must_parse("0.1.0-devel")));
    REQUIRE(!is_developer_build(version{}));
}
