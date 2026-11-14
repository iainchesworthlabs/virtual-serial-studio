//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Setup check rows and the firewall dialog that come from the permissions
// layer. The facts are injected, so every state is checked on every system
// without reading or changing the real firewall.
//

#include "test_framework.hpp"

#include "support/firewall_dialog.hpp"
#include "support/permission_rows.hpp"
#include "support/setup_facts.hpp"
#include "widgets/notes.hpp"

#include <QPushButton>

#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace support = vss::gui::support;
namespace perm = vss::permissions;

perm::fact firewall(std::string_view why, perm::state status,
                    bool with_step = false)
{
    perm::fact fact;
    fact.what = perm::topic::windows_firewall;
    fact.status = status;
    fact.why = std::string(why);
    fact.subject = "C:\\Studio\\bin\\vss_gui.exe";
    fact.port = 4001;
    if (with_step)
    {
        perm::firewall_rule_spec spec;
        spec.program = fact.subject;
        spec.port = 4001;
        fact.step = spec;
        fact.can_act = true;
        fact.remedy = perm::netsh_add_command(spec);
    }
    return fact;
}

perm::fact elevation(std::string_view why)
{
    perm::fact fact;
    fact.what = perm::topic::windows_elevation;
    fact.status = perm::state::info;
    fact.why = std::string(why);
    return fact;
}

vss::gui::setup_facts windows_engine_facts()
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = true;
    facts.serial_devices = 1;
    facts.elevation_known = true;
    return facts;
}

support::platform_facts windows_platform(std::vector<perm::fact> facts)
{
    support::platform_facts platform;
    platform.elevation_known = true;
    platform.elevated = false;
    platform.pty_known = true;
    platform.permissions = std::move(facts);
    return platform;
}

const support::setup_row* find_row(const support::setup_report& report,
                                   const QString& title)
{
    for (const support::setup_row& row : report.rows)
    {
        if (row.title == title)
            return &row;
    }
    return nullptr;
}

support::setup_report report_with(std::vector<perm::fact> facts)
{
    return support::build_setup_report(windows_engine_facts(),
                                       windows_platform(std::move(facts)),
                                       {QStringLiteral("COM3")});
}

} // namespace

TEST_CASE("permission rows: no facts, no firewall row")
{
    const auto report = report_with({});
    REQUIRE(find_row(report, QStringLiteral("Windows Firewall")) == nullptr);
}

TEST_CASE("permission rows: the firewall row says what it knows")
{
    const QString title = QStringLiteral("Windows Firewall");

    // Not listening for other computers: fine, and said so.
    auto report =
        report_with({firewall(perm::reason::loopback_only, perm::state::ok)});
    const auto* row = find_row(report, title);
    REQUIRE(row != nullptr);
    REQUIRE(row->state == support::setup_state::ok);
    REQUIRE(row->action == support::setup_action::none);

    // A listener with no rule: attention, with the offer.
    report = report_with(
        {firewall(perm::reason::no_allow_rule, perm::state::attention, true)});
    row = find_row(report, title);
    REQUIRE(row->state == support::setup_state::attention);
    REQUIRE(row->action == support::setup_action::firewall_help);
    REQUIRE(row->highlight);
    REQUIRE(row->sentence.contains(QStringLiteral("TCP 4001")));
    REQUIRE_EQ(row->action_text, QStringLiteral("Allow through the firewall"));
    REQUIRE_EQ(report.header_chip, QStringLiteral("1 thing to fix"));

    // The same without the step in this build: it only explains.
    report = report_with(
        {firewall(perm::reason::no_allow_rule, perm::state::attention)});
    REQUIRE_EQ(find_row(report, title)->action_text,
               QStringLiteral("How to allow it"));

    // A block rule is never removed by Studio.
    report = report_with(
        {firewall(perm::reason::blocked_by_rule, perm::state::attention)});
    row = find_row(report, title);
    REQUIRE(row->sentence.contains(QStringLiteral("never removes")));
    REQUIRE_EQ(row->action_text, QStringLiteral("How to change it"));

    // Rules that could not be read: not checked, nothing claimed.
    report =
        report_with({firewall(perm::reason::not_asked, perm::state::unknown)});
    row = find_row(report, title);
    REQUIRE(row->state == support::setup_state::not_checked);
    REQUIRE_EQ(row->chip, QStringLiteral("Not checked yet"));
}

TEST_CASE("permission rows: the firewall row sits before Continue without a "
          "driver")
{
    const auto report =
        report_with({firewall(perm::reason::loopback_only, perm::state::ok)});
    REQUIRE(report.rows.size() >= 2);
    REQUIRE(report.rows.back().action == support::setup_action::continue_test);
    REQUIRE_EQ(report.rows.at(report.rows.size() - 2).title,
               QStringLiteral("Windows Firewall"));
}

TEST_CASE("permission rows: the administrator row says whether Windows will "
          "ask for consent or a password")
{
    const auto text = [](std::string_view why)
    {
        const auto report = report_with({elevation(why)});
        return find_row(report, QStringLiteral("Administrator rights"))
            ->sentence;
    };
    REQUIRE(text(perm::reason::admin_not_elevated)
                .contains(QStringLiteral("ask you to confirm")));
    REQUIRE(text(perm::reason::standard_user)
                .contains(QStringLiteral("administrator's password")));
}

TEST_CASE("permission rows: listeners come from listen addresses only")
{
    const auto ask = support::permission_context_for(
        QStringLiteral("tcp-listen://0.0.0.0:4001"));
    REQUIRE_EQ(ask.listeners.size(), std::size_t{1});
    REQUIRE_EQ(ask.listeners.front().port, std::uint16_t{4001});
    REQUIRE(
        support::permission_context_for(QStringLiteral("tcp://10.0.0.5:4001"))
            .listeners.empty());
}

TEST_CASE("firewall dialog: the rule is shown exactly and nothing runs unasked")
{
    const auto fact =
        firewall(perm::reason::no_allow_rule, perm::state::attention, true);

    // The step is possible: the button is enabled, says what will happen, and
    // is not the default, so Enter does not press it.
    support::FirewallDialog able(fact, true);
    REQUIRE(able.allow_button() != nullptr);
    REQUIRE(able.allow_button()->isEnabled());
    REQUIRE(!able.allow_button()->isDefault());
    REQUIRE(able.allow_button()->toolTip().contains(
        QStringLiteral("administrator permission")));
    REQUIRE(!able.rule_added());
    REQUIRE(!able.result_note()->isVisibleTo(&able));

    // Not available in this build: only explains, button disabled and says why.
    support::FirewallDialog explain(fact, false);
    REQUIRE(!explain.allow_button()->isEnabled());
    REQUIRE_EQ(explain.allow_button()->toolTip(),
               QStringLiteral("Not available in this build"));
    explain.allow();
    REQUIRE(!explain.rule_added());

    // A block rule offers no button at all.
    support::FirewallDialog blocked(
        firewall(perm::reason::blocked_by_rule, perm::state::attention), true);
    REQUIRE(blocked.allow_button() == nullptr);
}
