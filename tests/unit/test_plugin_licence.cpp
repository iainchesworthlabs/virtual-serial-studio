//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/license.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/verify.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

license::time_point at(int year, unsigned month, unsigned day)
{
    return license::time_point{
        std::chrono::sys_days{std::chrono::year{year} /
                              std::chrono::month{month} /
                              std::chrono::day{day}}};
}

constexpr std::string_view licence_name = "VSS-2026-000123";

/// A licence for 13 November 2026 to 13 November 2027 with 28 days of grace.
license::licence licence_with(std::vector<std::string> features)
{
    license::licence value;
    value.licence_id = std::string(licence_name);
    value.licensee_name = "Example Lab Pty Ltd";
    value.issued_at = at(2026, 11, 13);
    value.starts_at = at(2026, 11, 13);
    value.expires_at = at(2027, 11, 13);
    value.grace_days = 28;
    value.features = std::move(features);
    return value;
}

/// The status of that licence on a day, judged by the licence library.
license::status judged(std::vector<std::string> features,
                       license::time_point now)
{
    license::context where;
    where.now = now;
    return license::evaluate(licence_with(std::move(features)), nullptr,
                             where);
}

/// The status in the middle of the licence's term: 316 days are left.
license::status valid_with(std::vector<std::string> features)
{
    return judged(std::move(features), at(2027, 1, 1));
}

manifest plugin_with(std::string id, std::string edition)
{
    manifest value;
    value.id = std::move(id);
    value.needs.edition = std::move(edition);
    return value;
}

manifest pro_plugin()
{
    return plugin_with("com.example.x", "pro");
}

const std::vector<trust_level>& all_levels()
{
    static const std::vector<trust_level> levels{
        trust_level::official, trust_level::third_party_signed,
        trust_level::signed_unknown_key, trust_level::unsigned_plugin};
    return levels;
}

/// Every state a licence can be in, for the plugins that are not asked about.
std::vector<license::status> every_status()
{
    return {license::no_licence(),
            valid_with({"plugins.official"}),
            valid_with({}),
            judged({"plugins.official"}, at(2028, 1, 1))};
}

void require_not_licensed(const result<void>& checked)
{
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == make_error_code(plugin_errc::not_licensed));
}

} // namespace

TEST_CASE("plugin licence: the feature of a plugin")
{
    REQUIRE_EQ(plugin_feature("com.example.x"),
               std::string("plugins.com.example.x"));
    REQUIRE_EQ(plugin_feature("com.virtualserialstudio.protocol"),
               std::string("plugins.com.virtualserialstudio.protocol"));
    REQUIRE_EQ(std::string(all_official_plugins_feature),
               std::string("plugins.official"));
}

TEST_CASE("plugin licence: only an official plugin with an edition is asked")
{
    REQUIRE(needs_licence(trust_level::official, plugin_with("a.b", "pro")));
    REQUIRE(needs_licence(trust_level::official,
                          plugin_with("a.b", "enterprise")));
    REQUIRE(!needs_licence(trust_level::official, plugin_with("a.b", "")));

    for (const trust_level level : all_levels())
    {
        if (level == trust_level::official)
            continue;
        REQUIRE(!needs_licence(level, plugin_with("a.b", "pro")));
        REQUIRE(!needs_licence(level, plugin_with("a.b", "enterprise")));
        REQUIRE(!needs_licence(level, plugin_with("a.b", "")));
    }
}

TEST_CASE("plugin licence: a plugin that is not asked about is granted")
{
    struct not_asked_case
    {
        trust_level level;
        manifest declared;
    };
    const std::vector<not_asked_case> cases{
        {trust_level::official, plugin_with("a.b", "")},
        {trust_level::third_party_signed, plugin_with("a.b", "pro")},
        {trust_level::signed_unknown_key, plugin_with("a.b", "enterprise")},
        {trust_level::unsigned_plugin, plugin_with("a.b", "pro")}};

    for (const license::status& current : every_status())
    {
        for (const not_asked_case& item : cases)
        {
            REQUIRE(!needs_licence(item.level, item.declared));
            // The answer starts as something other than "granted" so that
            // the check is seen to write it.
            licence_answer answer;
            answer.asked = true;
            answer.granted = false;
            answer.reason = license::reason::expired;
            answer.licence_id = "left over";
            answer.days_left = 7;

            const result<void> checked =
                check_licence(item.level, item.declared, current, &answer);
            REQUIRE(checked.has_value());
            REQUIRE(!answer.asked);
            REQUIRE(answer.granted);
            REQUIRE(answer.reason == license::reason::ok);
            REQUIRE(answer.licence_id.empty());
            REQUIRE_EQ(answer.days_left, 0);
            REQUIRE(answer == licence_answer{});
        }
    }
}

TEST_CASE("plugin licence: a licence that names all official plugins")
{
    licence_answer answer;
    const result<void> checked =
        check_licence(trust_level::official, pro_plugin(),
                      valid_with({"plugins.official"}), &answer);
    REQUIRE(checked.has_value());
    REQUIRE(answer.asked);
    REQUIRE(answer.granted);
    REQUIRE(answer.reason == license::reason::ok);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
    REQUIRE_EQ(answer.days_left, 316);
}

TEST_CASE("plugin licence: a licence that names the plugin itself")
{
    licence_answer answer;
    const result<void> checked =
        check_licence(trust_level::official, pro_plugin(),
                      valid_with({"plugins.com.example.x"}), &answer);
    REQUIRE(checked.has_value());
    REQUIRE(answer.asked);
    REQUIRE(answer.granted);
    REQUIRE(answer.reason == license::reason::ok);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
    REQUIRE_EQ(answer.days_left, 316);
}

TEST_CASE("plugin licence: a licence that names every plugin by prefix")
{
    licence_answer answer;
    const result<void> checked =
        check_licence(trust_level::official, pro_plugin(),
                      valid_with({"support", "plugins.*"}), &answer);
    REQUIRE(checked.has_value());
    REQUIRE(answer.asked);
    REQUIRE(answer.granted);
    REQUIRE(answer.reason == license::reason::ok);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
    REQUIRE_EQ(answer.days_left, 316);
}

TEST_CASE("plugin licence: a licence that names the Enterprise plugin only")
{
    const manifest enterprise = plugin_with("com.example.big", "enterprise");
    licence_answer answer;
    const license::status current = valid_with({"plugins.com.example.big"});
    REQUIRE(check_licence(trust_level::official, enterprise, current, &answer)
                .has_value());
    REQUIRE(answer.granted);

    // The same licence does not cover the other plugin.
    answer = {};
    require_not_licensed(
        check_licence(trust_level::official, pro_plugin(), current, &answer));
    REQUIRE(answer.asked);
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == license::reason::not_in_licence);
}

TEST_CASE("plugin licence: a licence that names neither feature")
{
    const std::vector<std::vector<std::string>> lists{
        {},
        {"support", "driver.signed"},
        {"plugins.com.example.y"},
        {"plugins.com.example.y", "plugins.other.*"},
        {"plugins"},
        {"plugins.officials"}};
    for (const std::vector<std::string>& features : lists)
    {
        licence_answer answer;
        require_not_licensed(check_licence(trust_level::official, pro_plugin(),
                                           valid_with(features), &answer));
        REQUIRE(answer.asked);
        REQUIRE(!answer.granted);
        REQUIRE(answer.reason == license::reason::not_in_licence);
        REQUIRE_EQ(answer.licence_id, std::string(licence_name));
        REQUIRE_EQ(answer.days_left, 316);
    }
}

TEST_CASE("plugin licence: no licence")
{
    licence_answer answer;
    require_not_licensed(check_licence(trust_level::official, pro_plugin(),
                                       license::no_licence(), &answer));
    REQUIRE(answer.asked);
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == license::reason::no_licence);
    REQUIRE(answer.licence_id.empty());
    REQUIRE_EQ(answer.days_left, 0);
}

TEST_CASE("plugin licence: an expired licence")
{
    // Past the end of the grace period: 13 November 2027 and 28 days.
    const license::status expired =
        judged({"plugins.official"}, at(2027, 12, 12));
    REQUIRE(expired.state == license::state::expired);

    licence_answer answer;
    require_not_licensed(check_licence(trust_level::official, pro_plugin(),
                                       expired, &answer));
    REQUIRE(answer.asked);
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == license::reason::expired);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
    REQUIRE_EQ(answer.days_left, 0);

    // The licence names the plugin itself, and that does not help either.
    answer = {};
    require_not_licensed(check_licence(
        trust_level::official, pro_plugin(),
        judged({"plugins.com.example.x"}, at(2028, 6, 1)), &answer));
    REQUIRE(answer.reason == license::reason::expired);
}

TEST_CASE("plugin licence: a licence in its grace period")
{
    // 1 December 2027 is after the end of the term and has ten days of grace
    // left.
    const license::status grace =
        judged({"plugins.official"}, at(2027, 12, 1));
    REQUIRE(grace.state == license::state::grace);

    licence_answer answer;
    const result<void> checked =
        check_licence(trust_level::official, pro_plugin(), grace, &answer);
    REQUIRE(checked.has_value());
    REQUIRE(answer.asked);
    REQUIRE(answer.granted);
    REQUIRE(answer.reason == license::reason::ok);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
    REQUIRE_EQ(answer.days_left, 10);
}

TEST_CASE("plugin licence: a licence that has not started")
{
    licence_answer answer;
    require_not_licensed(check_licence(
        trust_level::official, pro_plugin(),
        judged({"plugins.official"}, at(2026, 11, 12)), &answer));
    REQUIRE(answer.asked);
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == license::reason::not_yet_valid);
    REQUIRE_EQ(answer.licence_id, std::string(licence_name));
}

TEST_CASE("plugin licence: the answer is optional")
{
    const std::vector<license::status> statuses{
        license::no_licence(),
        valid_with({"plugins.official"}),
        valid_with({"plugins.com.example.x"}),
        valid_with({"support"}),
        judged({"plugins.official"}, at(2027, 12, 1)),
        judged({"plugins.official"}, at(2028, 1, 1))};
    const std::vector<manifest> manifests{pro_plugin(),
                                          plugin_with("com.example.x", "")};

    for (const license::status& current : statuses)
    {
        for (const manifest& declared : manifests)
        {
            for (const trust_level level : all_levels())
            {
                licence_answer answer;
                const result<void> with =
                    check_licence(level, declared, current, &answer);
                const result<void> without =
                    check_licence(level, declared, current);
                REQUIRE_EQ(with.has_value(), without.has_value());
                REQUIRE_EQ(with.has_value(), answer.granted);
                if (with.has_error())
                    REQUIRE(with.error() == without.error());
            }
        }
    }
}

TEST_CASE("plugin licence: the error is not_licensed")
{
    const result<void> checked = check_licence(
        trust_level::official, pro_plugin(), license::no_licence());
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == make_error_code(plugin_errc::not_licensed));
    REQUIRE(checked.error() == plugin_errc::not_licensed);
}
