//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a snapshot of the plugin service becomes what the Plugins page shows.
// The snapshots are made by hand: no plugin runs here.
//

#include "backend/service_mapping.hpp"

#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/manifest.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <string>

using namespace vss::gui;
namespace plug = vss::plugin;
namespace host = vss::plugin::host;
using namespace std::chrono_literals;

namespace
{

host::service_plugin make_entry(const char* id)
{
    host::service_plugin plugin;
    plugin.id = id;
    plugin.name = "Example";
    plugin.version = "1.2.0";
    plugin.author = "Example Ltd";
    plugin.directory = std::filesystem::path("C:/plugins") / id;
    plugin.scope = plug::plugin_scope::for_user;
    plugin.hash.bytes[0] = 0x12;
    plugin.hash.bytes[1] = 0xab;
    plugin.level = plug::trust_level::unsigned_plugin;
    return plugin;
}

host::service_plugin with_problem(const char* id, plug::plugin_errc code,
                                  const char* detail = "")
{
    host::service_plugin plugin = make_entry(id);
    plugin.problem = vss::error_code(code);
    plugin.detail = detail;
    return plugin;
}

std::shared_ptr<host::runner_snapshot> run_in(plug::run_state state)
{
    auto run = std::make_shared<host::runner_snapshot>();
    run->state = state;
    run->sandbox.applied.assign(std::begin(host::all_sandbox_parts),
                                std::end(host::all_sandbox_parts));
    return run;
}

plugin_view one(const host::service_plugin& plugin,
                const service_mapping_inputs& inputs = {},
                std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now())
{
    host::service_snapshot snapshot;
    snapshot.plugins.push_back(plugin);
    const plugin_snapshot mapped = map_service_snapshot(snapshot, inputs, now);
    REQUIRE_EQ(mapped.plugins.size(), std::size_t{1});
    return mapped.plugins.front();
}

std::chrono::year_month_day day(int year, unsigned month, unsigned today)
{
    return std::chrono::year_month_day{std::chrono::year{year},
                                       std::chrono::month{month},
                                       std::chrono::day{today}};
}

std::string bidi_mark()
{
    // U+202E, the right-to-left override, as its bytes.
    return std::string{static_cast<char>(0xe2), static_cast<char>(0x80),
                       static_cast<char>(0xae)};
}

} // namespace

TEST_CASE("service mapping: a run is Available, Starting, Starting again or "
          "Stopped after failing")
{
    const auto now = std::chrono::steady_clock::now();

    host::service_plugin ready = make_entry("org.example.ready");
    ready.enabled = true;
    ready.run = run_in(plug::run_state::ready);
    plugin_view view = one(ready);
    REQUIRE(view.state == plugin_state::available);
    REQUIRE(view.wished_on);
    REQUIRE(view.note == plugin_note::none);

    host::service_plugin starting = make_entry("org.example.starting");
    starting.enabled = true;
    starting.run = run_in(plug::run_state::starting);
    view = one(starting);
    REQUIRE(view.state == plugin_state::starting);
    REQUIRE(view.note == plugin_note::starting);

    // Backing off: how long until the next start, and which attempt it is.
    host::service_plugin waiting = make_entry("org.example.waiting");
    waiting.enabled = true;
    auto run = run_in(plug::run_state::backing_off);
    run->failures = 2;
    run->failure_limit = 5;
    run->next_start = now + 4s;
    run->last_failure = vss::error_code(plug::plugin_errc::crashed);
    waiting.run = run;
    view = one(waiting, {}, now);
    REQUIRE(view.state == plugin_state::starting);
    REQUIRE(view.note == plugin_note::backing_off);
    REQUIRE_EQ(view.back_off_seconds, 4);
    REQUIRE_EQ(view.attempt, 3);

    // Quarantined: stopped after failing, and why.
    host::service_plugin stopped = make_entry("org.example.stopped");
    stopped.enabled = true;
    run = run_in(plug::run_state::quarantined);
    run->failures = 5;
    run->failure_limit = 5;
    run->last_failure = vss::error_code(plug::plugin_errc::crashed);
    run->detail = "exited with code 3";
    stopped.run = run;
    view = one(stopped);
    REQUIRE(view.state == plugin_state::stopped);
    REQUIRE(view.note == plugin_note::stopped_failures);
    REQUIRE_EQ(view.failure_count, 5);
    REQUIRE_EQ(view.reason_code, QStringLiteral("crashed"));
    REQUIRE_EQ(view.detail, QStringLiteral("exited with code 3"));
    REQUIRE(view.wished_on);

    run->last_failure = vss::error_code(plug::plugin_errc::unresponsive);
    stopped.run = run;
    REQUIRE(one(stopped).note == plugin_note::stopped_unresponsive);
    run->last_failure = vss::error_code(plug::plugin_errc::misbehaving);
    run->detail = "sent too much (frames per second)";
    stopped.run = run;
    view = one(stopped);
    REQUIRE(view.note == plugin_note::stopped_limit);
    REQUIRE_EQ(view.limit_name, QStringLiteral("frames per second"));

    // No run and not switched on: Off, with the wish not made.
    host::service_plugin off = make_entry("org.example.off");
    view = one(off);
    REQUIRE(view.state == plugin_state::off);
    REQUIRE(!view.wished_on);
    // Switched on and not yet running: about to start.
    off.enabled = true;
    REQUIRE(one(off).state == plugin_state::starting);
}

TEST_CASE("service mapping: each reason code leads to its group")
{
    using code = plug::plugin_errc;
    struct row
    {
        code reason;
        plugin_state state;
        plugin_note note;
    };
    const row rows[] = {
        {code::signature_missing, plugin_state::needs_decision,
         plugin_note::decision_first},
        {code::key_unknown, plugin_state::needs_decision,
         plugin_note::decision_first},
        {code::changed, plugin_state::needs_decision,
         plugin_note::decision_changed},
        {code::policy, plugin_state::blocked, plugin_note::blocked_named},
        {code::not_licensed, plugin_state::needs_licence,
         plugin_note::needs_licence},
        {code::api_incompatible, plugin_state::needs_newer_studio,
         plugin_note::newer_studio_version},
        {code::manifest_unsupported_version, plugin_state::needs_newer_studio,
         plugin_note::newer_studio_version},
        {code::capability_missing, plugin_state::needs_newer_studio,
         plugin_note::newer_studio_capability},
        {code::manifest_invalid, plugin_state::cannot_be_used,
         plugin_note::invalid_manifest},
        {code::layout_invalid, plugin_state::cannot_be_used,
         plugin_note::invalid_manifest},
        {code::signature_invalid, plugin_state::cannot_be_used,
         plugin_note::invalid_signature},
        {code::revoked, plugin_state::cannot_be_used, plugin_note::withdrawn},
        {code::expired, plugin_state::cannot_be_used, plugin_note::expired},
        {code::reserved_id, plugin_state::not_used,
         plugin_note::not_used_reserved},
    };
    for (const row& each : rows)
    {
        const plugin_view view =
            one(with_problem("org.example.thing", each.reason, "a line"));
        REQUIRE(view.state == each.state);
        REQUIRE(view.note == each.note);
        // The code and the host's line are kept, for Show details.
        REQUIRE_EQ(view.reason_code, QString::fromStdString(std::string(
                                         plug::to_string(each.reason))));
        REQUIRE_EQ(view.detail, QStringLiteral("a line"));
    }
    // A capability that is missing is named by the host's line.
    REQUIRE_EQ(one(with_problem("org.example.thing", code::capability_missing,
                                "engine_rules"))
                   .capability_name,
               QStringLiteral("engine_rules"));
    // Any other reason is "Can't be used".
    REQUIRE(one(with_problem("org.example.thing", code::file_missing)).state ==
            plugin_state::cannot_be_used);
}

TEST_CASE("service mapping: policy, licence and isolation reasons")
{
    using code = plug::plugin_errc;
    // Only official plugins by policy.
    service_mapping_inputs inputs;
    inputs.rules = plug::policy{};
    inputs.rules->allow = plug::allow_level::official_only;
    plugin_view view =
        one(with_problem("org.example.thing", code::policy), inputs);
    REQUIRE(view.note == plugin_note::blocked_official_only);
    // A plugin the administrator named.
    inputs.rules->allow = plug::allow_level::any;
    inputs.rules->deny_plugins = {"org.example.thing"};
    view = one(with_problem("org.example.thing", code::policy), inputs);
    REQUIRE(view.note == plugin_note::blocked_named);

    // The licence's own reason.
    view = one(with_problem("org.example.thing", code::not_licensed));
    REQUIRE(view.licence_reason == vss::license::reason::no_licence);

    // The isolation reasons: the user may override on an install of their
    // own; nobody may on an install for everyone with no policy; a policy
    // that forbids it says so.
    service_mapping_inputs mine;
    mine.install = plug::install_kind::for_one_user;
    view = one(with_problem("org.example.thing", code::sandbox_insufficient),
               mine);
    REQUIRE(view.state == plugin_state::needs_isolation);
    REQUIRE(view.note == plugin_note::isolation_override);
    REQUIRE(view.override_allowed);

    service_mapping_inputs everyone;
    everyone.install = plug::install_kind::for_everyone;
    view = one(with_problem("org.example.thing", code::sandbox_insufficient),
               everyone);
    REQUIRE(view.state == plugin_state::blocked);
    REQUIRE(view.note == plugin_note::isolation_no_override);
    REQUIRE(!view.override_allowed);

    everyone.rules = plug::policy{};
    everyone.rules->allow_no_sandbox = false;
    view = one(with_problem("org.example.thing", code::sandbox_insufficient),
               everyone);
    REQUIRE(view.note == plugin_note::blocked_protection);

    // An override stored for this copy shows in the isolation row.
    service_mapping_inputs overridden;
    plug::plugin_decision decision;
    decision.plugin_id = "org.example.thing";
    decision.hash.bytes[0] = 0x12;
    decision.hash.bytes[1] = 0xab;
    decision.level = plug::trust_level::unsigned_plugin;
    decision.enabled = true;
    decision.run_without_sandbox = true;
    decision.decided_on = day(2026, 11, 14);
    REQUIRE(overridden.trust.decide(decision).has_value());
    host::service_plugin running = make_entry("org.example.thing");
    running.enabled = true;
    running.run = run_in(plug::run_state::ready);
    view = one(running, overridden);
    REQUIRE(view.override_active);
    REQUIRE_EQ(view.override_on, QDate(2026, 11, 14));
}

TEST_CASE("service mapping: the sandbox shown is the one the run reached, or "
          "what this computer can give")
{
    host::service_plugin plugin = make_entry("org.example.thing");
    plugin.enabled = true;
    auto run = run_in(plug::run_state::ready);
    plugin.run = run;
    REQUIRE(one(plugin).isolation == plugin_isolation::full);

    // Without the network part the level is lower, and the missing part is
    // named.
    run->sandbox.applied.erase(std::remove(run->sandbox.applied.begin(),
                                           run->sandbox.applied.end(),
                                           host::sandbox_part::network),
                               run->sandbox.applied.end());
    plugin.run = run;
    const plugin_view view = one(plugin);
    REQUIRE(view.isolation != plugin_isolation::full);
    if (view.isolation == plugin_isolation::partial)
    {
        REQUIRE(view.isolation_missing.contains(
            QStringLiteral("no network isolation")));
    }
    REQUIRE(sandbox_missing_text(run->sandbox)
                .contains(QStringLiteral("no network isolation")));

    // Nothing applied: none.
    run->sandbox.applied.clear();
    plugin.run = run;
    REQUIRE(one(plugin).isolation == plugin_isolation::none);

    // A plugin that is not running shows what this computer can apply.
    const host::service_plugin idle = make_entry("org.example.idle");
    service_mapping_inputs inputs;
    inputs.available.applied.assign(std::begin(host::all_sandbox_parts),
                                    std::end(host::all_sandbox_parts));
    REQUIRE(one(idle, inputs).isolation == plugin_isolation::full);
    REQUIRE(one(idle).isolation == plugin_isolation::none);
}

TEST_CASE("service mapping: permissions, manifest, decisions, shadowing and "
          "the notes at the top")
{
    host::service_plugin plugin = make_entry("org.example.thing");
    plug::permission_request needed;
    needed.name = plug::permission::ports_read;
    needed.required = true;
    needed.reason = "To read.";
    plug::permission_request optional_one;
    optional_one.name = plug::permission::ports_write;
    optional_one.required = false;
    plugin.asked = {needed, optional_one};
    plugin.granted = {plug::permission::ports_read};
    plugin.withheld = {plug::permission::ports_write};
    plugin.level = plug::trust_level::third_party_signed;
    plugin.key_id = "example-1";

    service_mapping_inputs inputs;
    plug::manifest declared;
    declared.id = "org.example.thing";
    plug::extension_point point;
    point.kind = plug::extension_kind::fixture;
    point.id = "meter";
    point.name.entries.push_back({"", "Meter"});
    declared.extension_points.push_back(point);
    inputs.manifests.emplace(manifest_key(plugin), declared);
    plug::approved_key key;
    key.key_id = "example-1";
    key.name = "Example Instruments";
    key.approved_on = day(2026, 10, 2);
    REQUIRE(inputs.trust.approve_key(key).has_value());

    const plugin_view view = one(plugin, inputs);
    REQUIRE(view.trust == plugin_trust::third_party);
    REQUIRE_EQ(view.key_name, QStringLiteral("Example Instruments"));
    REQUIRE_EQ(view.key_approved_on, QDate(2026, 10, 2));
    REQUIRE_EQ(view.permissions.size(), std::size_t{2});
    REQUIRE(view.permissions[0].required);
    REQUIRE(!view.permissions[0].turned_down);
    REQUIRE_EQ(view.permissions[0].reason, QStringLiteral("To read."));
    REQUIRE(!view.permissions[1].required);
    REQUIRE(view.permissions[1].turned_down);
    REQUIRE_EQ(view.adds.size(), std::size_t{1});
    REQUIRE_EQ(view.adds[0].type_name, QStringLiteral("Meter"));
    REQUIRE_EQ(view.adds[0].type_id, QStringLiteral("meter"));
    REQUIRE_EQ(view.fingerprint.size(), 16);
    REQUIRE_EQ(view.hash.size(), 128);
    REQUIRE(view.installed_for == plugin_installed_for::you_only);

    // A plugin with the same id that another copy shadows names the copy
    // that is used.
    host::service_snapshot snapshot;
    host::service_plugin used = make_entry("org.example.same");
    used.scope = plug::plugin_scope::for_everyone;
    used.level = plug::trust_level::official;
    host::service_plugin shadowed =
        with_problem("org.example.same", plug::plugin_errc::shadowed);
    shadowed.directory = "C:/other/same";
    snapshot.plugins = {used, shadowed};
    const plugin_snapshot mapped = map_service_snapshot(snapshot, {});
    REQUIRE(mapped.plugins[1].state == plugin_state::not_used);
    REQUIRE(mapped.plugins[1].note == plugin_note::not_used_different_signer);
    REQUIRE(mapped.plugins[1].used_copy_trust == plugin_trust::official);
    REQUIRE_EQ(mapped.plugins[1].used_copy_folder,
               QStringLiteral("C:/plugins/org.example.same"));

    // A previous decision about another version makes this one "changed".
    service_mapping_inputs changed;
    plug::plugin_decision before;
    before.plugin_id = "org.example.thing";
    before.hash.bytes[0] = 0x99;
    before.level = plug::trust_level::unsigned_plugin;
    before.enabled = true;
    before.granted = {plug::permission::ports_read};
    before.decided_on = day(2026, 11, 1);
    REQUIRE(changed.trust.decide(before).has_value());
    host::service_plugin again =
        with_problem("org.example.thing", plug::plugin_errc::signature_missing);
    again.asked = {needed, optional_one};
    const plugin_view revised = one(again, changed);
    REQUIRE(revised.note == plugin_note::decision_changed);
    REQUIRE(revised.differs_from_approved);
    REQUIRE(!revised.permissions[0].is_new);
    REQUIRE(revised.permissions[1].is_new);

    // The notes at the top.
    host::service_snapshot noted;
    noted.notes = {"The policy file could not be used: others can write it."};
    REQUIRE(map_service_snapshot(noted, {}).banner ==
            plugin_banner::policy_untrusted);
    service_mapping_inputs official_only;
    official_only.rules = plug::policy{};
    official_only.rules->allow = plug::allow_level::official_only;
    REQUIRE(map_service_snapshot({}, official_only).banner ==
            plugin_banner::official_only);
    service_mapping_inputs no_user;
    no_user.rules = plug::policy{};
    no_user.rules->user_plugins = false;
    const plugin_snapshot closed = map_service_snapshot({}, no_user);
    REQUIRE(closed.banner == plugin_banner::user_plugins_off);
    REQUIRE(!closed.user_plugins_allowed);
    REQUIRE(map_service_snapshot({}, {}).can_run_plugins);
}

TEST_CASE("service mapping: what a plugin says is carried as it was said, and "
          "its log and status come through")
{
    host::service_plugin plugin = make_entry("org.example.thing");
    plugin.name = "Virtual Serial Studio " + bidi_mark() + " <b>x</b> %1";
    plugin.author = "<a href=\"x\">Virtual Serial Studio</a>";
    plugin.enabled = true;
    auto run = run_in(plug::run_state::ready);
    plug::status_message status;
    status.text = "Fine <i>really</i>\x1b[31m";
    run->status = status;
    host::runner_log_line line;
    line.text = "line one " + bidi_mark();
    run->log = {line, line};
    plugin.run = run;
    const plugin_view view = one(plugin);
    // The page cleans these; the mapping does not interpret them.
    REQUIRE(view.name.contains(QStringLiteral("<b>x</b>")));
    REQUIRE(view.name.contains(QStringLiteral("%1")));
    REQUIRE(view.author.contains(QStringLiteral("href")));
    REQUIRE(view.status_text.contains(QStringLiteral("<i>really</i>")));
    REQUIRE_EQ(view.log.size(), std::size_t{2});
    // And a trust level the host did not give is never "official".
    REQUIRE(view.trust == plugin_trust::unsigned_plugin);
}
