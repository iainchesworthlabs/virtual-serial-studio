//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::plan_start`, the one place that says whether an
// installed plugin runs.  The function does no I/O, so each test builds an
// `inspection` by hand in memory and nothing is created on disk.  The tests
// follow the rules of the header in order (the inspection, the licence,
// trust, on and off, permissions, the program) and then look at the sandbox
// profile, the least level and the line of detail a refusal gives.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/host/start_plan.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using vss::plugin::api_range;
using vss::plugin::api_version;
using vss::plugin::file_entry;
using vss::plugin::inspection;
using vss::plugin::make_error_code;
using vss::plugin::manifest;
using vss::plugin::permission;
using vss::plugin::permission_request;
using vss::plugin::plugin_decision;
using vss::plugin::plugin_errc;
using vss::plugin::sandbox_level;
using vss::plugin::to_string;
using vss::plugin::trust_level;
using vss::plugin::verify_result;
using vss::plugin::host::data_directory_of;
using vss::plugin::host::plan_context;
using vss::plugin::host::plan_start;
using vss::plugin::host::process_request;
using vss::plugin::host::sandbox_profile;
using vss::plugin::host::start_plan;

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view official_id = "com.virtualserialstudio.test";
constexpr std::string_view third_party_id = "com.example.plugin";
constexpr std::string_view program_path = "bin/plugin";

fs::path plugin_directory()
{
    return fs::temp_directory_path() / "vss-plan-test" / "plugin";
}

fs::path data_root()
{
    return fs::temp_directory_path() / "vss-plan-test" / "data";
}

/// A path as UTF-8, whatever the system keeps it as.
std::string utf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

bool holds(const std::vector<permission>& list, permission wanted)
{
    return std::ranges::find(list, wanted) != list.end();
}

permission_request ask(permission name, bool required = true)
{
    permission_request request;
    request.name = name;
    request.required = required;
    return request;
}

/// What a manifest asks for: each of `required`, then each of `optional`.
std::vector<permission_request> asking(const std::vector<permission>& required,
                                       const std::vector<permission>& optional)
{
    std::vector<permission_request> requests;
    requests.reserve(required.size() + optional.size());
    for (const permission name : required)
        requests.push_back(ask(name, true));
    for (const permission name : optional)
        requests.push_back(ask(name, false));
    return requests;
}

manifest manifest_for(std::string_view id,
                      std::vector<permission_request> permissions)
{
    manifest declared;
    declared.id = std::string(id);
    declared.name = "Plan test";
    declared.version.major = 1;
    declared.version.minor = 2;
    declared.version.patch = 0;
    declared.needs.api = api_range{api_version{1, 0}, 1, std::nullopt};
    declared.run.executable = std::string(program_path);
    declared.run.args = {"--serve"};
    declared.run.platforms = {"linux-x64"};
    declared.permissions = std::move(permissions);
    return declared;
}

/// An inspection that passed every check, as `inspect_plugin` would give it.
inspection inspection_for(manifest declared, trust_level level,
                          bool below_min_sandbox)
{
    inspection found;
    found.directory = plugin_directory();

    file_entry program;
    program.path = declared.run.executable;
    found.files.push_back(program);

    verify_result verified;
    verified.level = level;
    verified.below_min_sandbox = below_min_sandbox;

    found.declared = std::move(declared);
    found.level = level;
    found.verified = verified;
    return found;
}

std::vector<permission_request> a_few_permissions()
{
    return asking({permission::ports_read, permission::traffic_tap},
                  {permission::net_outbound});
}

/// An official plugin, on a computer that cannot confine it as far as the
/// minimum asks.
inspection good_official()
{
    return inspection_for(manifest_for(official_id, a_few_permissions()),
                          trust_level::official, true);
}

inspection good_third_party()
{
    return inspection_for(manifest_for(third_party_id, a_few_permissions()),
                          trust_level::third_party_signed, false);
}

void deny_permissions(inspection& found, const std::vector<permission>& denied)
{
    REQUIRE(found.verified.has_value());
    found.verified->denied_permissions = denied;
}

plan_context context_for_test(sandbox_level least = sandbox_level::partial)
{
    plan_context context;
    context.data_root = data_root();
    context.capabilities = {"engine_rules", "traffic_tap"};
    context.min_sandbox = least;
    context.seed = std::uint64_t{42};
    return context;
}

plugin_decision decision_for(std::string_view id, bool enabled,
                             std::vector<permission> granted)
{
    plugin_decision decision;
    decision.plugin_id = std::string(id);
    decision.level = trust_level::third_party_signed;
    decision.enabled = enabled;
    decision.granted = std::move(granted);
    return decision;
}

/// The plan, which the test requires to be a value.
start_plan planned(const inspection& found, const plugin_decision* decision,
                   const plan_context& context = context_for_test())
{
    const vss::result<start_plan> plan = plan_start(found, decision, context);
    REQUIRE(plan.has_value());
    return plan.value();
}

/// Requires the refusal `expected` and a line of detail, and returns it.
std::string refused(const inspection& found, const plugin_decision* decision,
                    plugin_errc expected)
{
    std::string detail;
    const vss::result<start_plan> plan =
        plan_start(found, decision, context_for_test(), &detail);
    REQUIRE(plan.has_error());
    REQUIRE(plan.error() == make_error_code(expected));
    REQUIRE(!detail.empty());
    return detail;
}

std::optional<std::string> environment_value(const process_request& request,
                                             std::string_view name)
{
    for (const auto& entry : request.environment)
    {
        if (entry.first == name)
            return entry.second;
    }
    return std::nullopt;
}

bool mentions(const std::string& text, std::string_view word)
{
    return std::string_view(text).find(word) != std::string_view::npos;
}

} // namespace

// -- the plan ---------------------------------------------------------------

TEST_CASE("plugin start plan: an official plugin is planned and enabled")
{
    const inspection found = good_official();
    const plan_context context = context_for_test();
    const start_plan plan = planned(found, nullptr, context);
    const fs::path data = data_directory_of(context.data_root, official_id);

    REQUIRE(plan.enabled);

    const process_request& process = plan.options.process;
    REQUIRE_EQ(process.program, found.directory / "bin/plugin");
    REQUIRE_EQ(process.arguments, std::vector<std::string>{"--serve"});
    REQUIRE_EQ(process.working_directory, data);

    const std::optional<std::string> id =
        environment_value(process, "VSS_PLUGIN_ID");
    REQUIRE(id.has_value());
    REQUIRE_EQ(*id, official_id);
    const std::optional<std::string> data_variable =
        environment_value(process, "VSS_PLUGIN_DATA");
    REQUIRE(data_variable.has_value());
    REQUIRE_EQ(*data_variable, utf8(data));

    const auto& session = plan.options.session;
    REQUIRE_EQ(session.plugin_id, official_id);
    REQUIRE_EQ(session.version, "1.2.0");
    REQUIRE(found.declared.has_value());
    REQUIRE(session.accepted == found.declared->needs.api);
    REQUIRE_EQ(session.ack.capabilities, context.capabilities);
    REQUIRE(!session.ack.data_directory.empty());
    REQUIRE_EQ(session.ack.data_directory, utf8(data));

    REQUIRE_EQ(plan.options.seed, std::uint64_t{42});
}

TEST_CASE("plugin start plan: the data directory is <root>/<id>/data")
{
    const fs::path root = data_root();
    REQUIRE_EQ(data_directory_of(root, "com.example.plugin"),
               root / "com.example.plugin" / "data");
    REQUIRE_EQ(data_directory_of(root, official_id),
               root / fs::path(std::string(official_id)) / "data");
    REQUIRE_NE(data_directory_of(root, "com.example.one"),
               data_directory_of(root, "com.example.two"));
}

// -- rule 1: the inspection -------------------------------------------------

TEST_CASE("plugin start plan: a problem or run refusal is passed on")
{
    const std::vector<plugin_errc> problems{plugin_errc::layout_invalid,
                                            plugin_errc::manifest_invalid,
                                            plugin_errc::file_missing};
    for (const plugin_errc code : problems)
    {
        inspection found = good_official();
        found.problem = make_error_code(code);
        found.detail = "the folder is not a plugin";
        const std::string detail = refused(found, nullptr, code);
        REQUIRE_EQ(detail, found.detail);
    }

    const std::vector<plugin_errc> runs{plugin_errc::revoked,
                                        plugin_errc::expired,
                                        plugin_errc::sandbox_insufficient};
    for (const plugin_errc code : runs)
    {
        inspection found = good_third_party();
        found.run = make_error_code(code);
        found.run_detail = "the plugin may not run";
        const std::string detail = refused(found, nullptr, code);
        REQUIRE_EQ(detail, found.run_detail);
    }
}

TEST_CASE("plugin start plan: an inspection that is not whole is refused")
{
    inspection no_declared = good_official();
    no_declared.declared.reset();
    refused(no_declared, nullptr, plugin_errc::manifest_invalid);

    inspection no_level = good_official();
    no_level.level.reset();
    refused(no_level, nullptr, plugin_errc::manifest_invalid);

    inspection no_verified = good_official();
    no_verified.verified.reset();
    refused(no_verified, nullptr, plugin_errc::manifest_invalid);
}

// -- rule 2: the licence ----------------------------------------------------

TEST_CASE("plugin start plan: the licence must cover the plugin")
{
    inspection refusing = good_official();
    refusing.licence_checked = true;
    refusing.licence.asked = true;
    refusing.licence.granted = false;
    refused(refusing, nullptr, plugin_errc::not_licensed);

    inspection granted = refusing;
    granted.licence.granted = true;
    REQUIRE(planned(granted, nullptr).enabled);

    inspection not_asked = refusing;
    not_asked.licence.asked = false;
    REQUIRE(planned(not_asked, nullptr).enabled);

    inspection not_checked = refusing;
    not_checked.licence_checked = false;
    REQUIRE(planned(not_checked, nullptr).enabled);

    // A licence that could not be read covers nothing, even when the answer
    // that came with it says granted.
    inspection unreadable = granted;
    unreadable.licence_problem = vss::make_error_code(vss::errc::io_error);
    refused(unreadable, nullptr, plugin_errc::not_licensed);
}

// -- rule 3: trust ----------------------------------------------------------

TEST_CASE("plugin start plan: a plugin nobody trusts needs a decision")
{
    struct trust_case
    {
        trust_level level;
        plugin_errc expected;
    };
    const std::vector<trust_case> cases{
        {trust_level::third_party_signed, plugin_errc::key_unknown},
        {trust_level::unsigned_plugin, plugin_errc::signature_missing},
        {trust_level::signed_unknown_key, plugin_errc::key_unknown}};

    for (const trust_case& item : cases)
    {
        const inspection found =
            inspection_for(manifest_for(third_party_id, a_few_permissions()),
                           item.level, false);
        refused(found, nullptr, item.expected);
    }

    // The owner's signature is the decision for an official plugin.
    REQUIRE(planned(good_official(), nullptr).enabled);
}

// -- rule 4: on and off -----------------------------------------------------

TEST_CASE("plugin start plan: a third-party plugin is on only when decided")
{
    const inspection found = good_third_party();

    // Off is not an error, and it does not look at what is granted.
    const plugin_decision off = decision_for(third_party_id, false, {});
    const start_plan stopped = planned(found, &off);
    REQUIRE(!stopped.enabled);
    REQUIRE(stopped.options.process.program.empty());
    REQUIRE(stopped.granted.empty());

    const plugin_decision on =
        decision_for(third_party_id, true,
                     {permission::ports_read, permission::traffic_tap});
    const start_plan started = planned(found, &on);
    REQUIRE(started.enabled);
    REQUIRE(!started.options.process.program.empty());
}

TEST_CASE("plugin start plan: an official plugin is on unless decided off")
{
    const inspection found = good_official();

    const plugin_decision off = decision_for(official_id, false, {});
    const start_plan stopped = planned(found, &off);
    REQUIRE(!stopped.enabled);
    REQUIRE(stopped.options.process.program.empty());

    const plugin_decision on = decision_for(official_id, true, {});
    const start_plan started = planned(found, &on);
    REQUIRE(started.enabled);
    REQUIRE(!started.options.process.program.empty());
}

// -- rule 5: permissions ----------------------------------------------------

TEST_CASE("plugin start plan: an official plugin gets what it asks for")
{
    const std::vector<permission> required{permission::ports_read,
                                           permission::traffic_tap};
    const std::vector<permission> optional{permission::net_outbound,
                                           permission::fs_read};
    const inspection found =
        inspection_for(manifest_for(official_id, asking(required, optional)),
                       trust_level::official, true);

    const start_plan plan = planned(found, nullptr);
    for (const permission name : required)
        REQUIRE(holds(plan.granted, name));
    for (const permission name : optional)
        REQUIRE(holds(plan.granted, name));
    REQUIRE(plan.withheld.empty());
}

TEST_CASE("plugin start plan: a third-party plugin gets what is listed")
{
    const inspection found = inspection_for(
        manifest_for(third_party_id,
                     asking({permission::ports_read, permission::traffic_tap},
                            {permission::net_outbound, permission::fs_read})),
        trust_level::third_party_signed, false);

    // `ports_write` is listed but the manifest never asked for it.
    const plugin_decision decision =
        decision_for(third_party_id, true,
                     {permission::ports_read, permission::traffic_tap,
                      permission::fs_read, permission::ports_write});

    const start_plan plan = planned(found, &decision);
    REQUIRE(holds(plan.granted, permission::ports_read));
    REQUIRE(holds(plan.granted, permission::traffic_tap));
    REQUIRE(holds(plan.granted, permission::fs_read));
    REQUIRE(!holds(plan.granted, permission::net_outbound));
    REQUIRE(!holds(plan.granted, permission::ports_write));
    REQUIRE_EQ(plan.withheld.size(), std::size_t{1});
    REQUIRE(holds(plan.withheld, permission::net_outbound));
}

TEST_CASE("plugin start plan: a required permission that is not listed fails")
{
    const std::vector<permission> missing_list{permission::ports_write,
                                               permission::net_listen};
    for (const permission missing : missing_list)
    {
        const inspection found = inspection_for(
            manifest_for(third_party_id,
                         asking({permission::ports_read, missing}, {})),
            trust_level::third_party_signed, false);
        const plugin_decision decision =
            decision_for(third_party_id, true, {permission::ports_read});

        const std::string detail =
            refused(found, &decision, plugin_errc::permission_denied);
        REQUIRE(mentions(detail, to_string(missing)));
    }
}

TEST_CASE("plugin start plan: a permission the policy denies is not granted")
{
    // Required: the plugin cannot start, whoever signed it.
    inspection required = inspection_for(
        manifest_for(official_id, asking({permission::net_outbound}, {})),
        trust_level::official, true);
    deny_permissions(required, {permission::net_outbound});
    const std::string detail =
        refused(required, nullptr, plugin_errc::permission_denied);
    REQUIRE(mentions(detail, to_string(permission::net_outbound)));

    // Optional: the plugin runs without it, even when the user listed it.
    inspection optional = inspection_for(
        manifest_for(official_id, asking({permission::ports_read},
                                         {permission::net_outbound})),
        trust_level::official, true);
    deny_permissions(optional, {permission::net_outbound});
    const start_plan official_plan = planned(optional, nullptr);
    REQUIRE(holds(official_plan.granted, permission::ports_read));
    REQUIRE(!holds(official_plan.granted, permission::net_outbound));
    REQUIRE(holds(official_plan.withheld, permission::net_outbound));

    inspection third_party = inspection_for(
        manifest_for(third_party_id, asking({permission::ports_read},
                                            {permission::net_outbound})),
        trust_level::third_party_signed, false);
    deny_permissions(third_party, {permission::net_outbound});
    const plugin_decision decision =
        decision_for(third_party_id, true,
                     {permission::ports_read, permission::net_outbound});
    const start_plan other_plan = planned(third_party, &decision);
    REQUIRE(!holds(other_plan.granted, permission::net_outbound));
    REQUIRE(holds(other_plan.withheld, permission::net_outbound));
}

TEST_CASE("plugin start plan: a plugin always has its private data")
{
    // The manifest does not ask for it.
    const start_plan official_plan = planned(good_official(), nullptr);
    REQUIRE(holds(official_plan.granted, permission::data_private));

    const plugin_decision nothing = decision_for(third_party_id, true, {});
    const inspection quiet =
        inspection_for(manifest_for(third_party_id, {}),
                       trust_level::third_party_signed, false);
    const start_plan quiet_plan = planned(quiet, &nothing);
    REQUIRE(holds(quiet_plan.granted, permission::data_private));
    REQUIRE_EQ(quiet_plan.granted.size(), std::size_t{1});

    // The manifest asks for it and the decision does not list it.
    const inspection asking_for_it = inspection_for(
        manifest_for(third_party_id, asking({permission::data_private}, {})),
        trust_level::third_party_signed, false);
    const start_plan asked_plan = planned(asking_for_it, &nothing);
    REQUIRE(holds(asked_plan.granted, permission::data_private));
    REQUIRE_EQ(asked_plan.granted.size(), std::size_t{1});
    REQUIRE(asked_plan.withheld.empty());
}

TEST_CASE("plugin start plan: what is granted is what the plugin is told")
{
    const start_plan plan = planned(good_official(), nullptr);
    REQUIRE(!plan.granted.empty());
    REQUIRE(plan.options.session.ack.granted == plan.granted);

    // A permission the manifest lists twice is granted once.
    const inspection twice = inspection_for(
        manifest_for(
            official_id,
            asking({permission::ports_read, permission::ports_read}, {})),
        trust_level::official, true);
    const start_plan twice_plan = planned(twice, nullptr);
    REQUIRE_EQ(std::ranges::count(twice_plan.granted, permission::ports_read),
               1);
    REQUIRE_EQ(std::ranges::count(twice_plan.granted, permission::data_private),
               1);
    REQUIRE(twice_plan.options.session.ack.granted == twice_plan.granted);
}

// -- rule 6: the program ----------------------------------------------------

TEST_CASE("plugin start plan: the program must be one of the files")
{
    inspection no_files = good_official();
    no_files.files.clear();
    refused(no_files, nullptr, plugin_errc::file_missing);

    inspection other_file = good_official();
    other_file.files.clear();
    file_entry other;
    other.path = "bin/other";
    other_file.files.push_back(other);
    refused(other_file, nullptr, plugin_errc::file_missing);

    // Third-party plugins are held to the same rule.
    inspection third_party = good_third_party();
    third_party.files.clear();
    const plugin_decision decision =
        decision_for(third_party_id, true,
                     {permission::ports_read, permission::traffic_tap});
    refused(third_party, &decision, plugin_errc::file_missing);
}

// -- the sandbox ------------------------------------------------------------

TEST_CASE("plugin start plan: the profile follows what was granted")
{
    struct profile_case
    {
        std::vector<permission> asked;
        bool listen = false;
        bool outbound = false;
        bool spawn = false;
    };
    const std::vector<profile_case> cases{
        {{}, false, false, false},
        {{permission::net_listen}, true, false, false},
        {{permission::net_outbound}, false, true, false},
        {{permission::process_spawn}, false, false, true},
        {{permission::net_listen, permission::net_outbound,
          permission::process_spawn},
         true,
         true,
         true}};

    const plan_context context = context_for_test();
    for (const profile_case& item : cases)
    {
        const inspection found =
            inspection_for(manifest_for(official_id, asking(item.asked, {})),
                           trust_level::official, true);
        const start_plan plan = planned(found, nullptr, context);

        REQUIRE(plan.options.process.sandbox.has_value());
        const sandbox_profile& profile = *plan.options.process.sandbox;
        REQUIRE_EQ(profile.name, official_id);
        REQUIRE_EQ(profile.plugin_directory, found.directory);
        REQUIRE_EQ(profile.data_directory,
                   data_directory_of(context.data_root, official_id));
        REQUIRE_EQ(profile.network_listen, item.listen);
        REQUIRE_EQ(profile.network_outbound, item.outbound);
        REQUIRE_EQ(profile.start_programs, item.spawn);
    }
}

TEST_CASE("plugin start plan: a permission asked for and not granted is off")
{
    const inspection found = inspection_for(
        manifest_for(third_party_id, asking({permission::ports_read},
                                            {permission::net_outbound,
                                             permission::process_spawn})),
        trust_level::third_party_signed, false);

    const plugin_decision without =
        decision_for(third_party_id, true, {permission::ports_read});
    const start_plan plain = planned(found, &without);
    REQUIRE(plain.options.process.sandbox.has_value());
    REQUIRE(!plain.options.process.sandbox->network_outbound);
    REQUIRE(!plain.options.process.sandbox->start_programs);

    const plugin_decision with =
        decision_for(third_party_id, true,
                     {permission::ports_read, permission::net_outbound});
    const start_plan networked = planned(found, &with);
    REQUIRE(networked.options.process.sandbox.has_value());
    REQUIRE(networked.options.process.sandbox->network_outbound);
    REQUIRE(!networked.options.process.sandbox->start_programs);

    // The policy denies it to an official plugin as well.
    inspection denied = inspection_for(
        manifest_for(official_id, asking({}, {permission::net_outbound})),
        trust_level::official, true);
    deny_permissions(denied, {permission::net_outbound});
    const start_plan official_plan = planned(denied, nullptr);
    REQUIRE(official_plan.options.process.sandbox.has_value());
    REQUIRE(!official_plan.options.process.sandbox->network_outbound);
}

TEST_CASE("plugin start plan: the least level is the minimum or none")
{
    struct level_case
    {
        bool below_min_sandbox = false;
        sandbox_level minimum = sandbox_level::none;
        sandbox_level expected = sandbox_level::none;
    };
    const std::vector<level_case> cases{
        {true, sandbox_level::partial, sandbox_level::none},
        {true, sandbox_level::full, sandbox_level::none},
        {false, sandbox_level::none, sandbox_level::none},
        {false, sandbox_level::partial, sandbox_level::partial},
        {false, sandbox_level::full, sandbox_level::full}};

    for (const level_case& item : cases)
    {
        const inspection found =
            inspection_for(manifest_for(official_id, a_few_permissions()),
                           trust_level::official, item.below_min_sandbox);
        const start_plan plan =
            planned(found, nullptr, context_for_test(item.minimum));
        REQUIRE(plan.options.process.least == item.expected);
    }
}

// -- the detail text --------------------------------------------------------

TEST_CASE("plugin start plan: every refusal gives a line of detail")
{
    struct refusal
    {
        inspection found;
        plugin_errc expected;
    };
    std::vector<refusal> refusals;

    inspection problem = good_official();
    problem.problem = make_error_code(plugin_errc::layout_invalid);
    problem.detail = "plugin.json is missing";
    refusals.push_back(refusal{problem, plugin_errc::layout_invalid});

    inspection run = good_official();
    run.run = make_error_code(plugin_errc::revoked);
    run.run_detail = "the signing key is revoked";
    refusals.push_back(refusal{run, plugin_errc::revoked});

    inspection not_inspected = good_official();
    not_inspected.declared.reset();
    refusals.push_back(refusal{not_inspected, plugin_errc::manifest_invalid});

    inspection not_checked = good_official();
    not_checked.verified.reset();
    refusals.push_back(refusal{not_checked, plugin_errc::manifest_invalid});

    inspection not_licensed = good_official();
    not_licensed.licence_checked = true;
    not_licensed.licence.asked = true;
    not_licensed.licence.granted = false;
    refusals.push_back(refusal{not_licensed, plugin_errc::not_licensed});

    inspection unreadable = good_official();
    unreadable.licence_problem = vss::make_error_code(vss::errc::io_error);
    refusals.push_back(refusal{unreadable, plugin_errc::not_licensed});

    refusals.push_back(refusal{good_third_party(), plugin_errc::key_unknown});
    refusals.push_back(refusal{
        inspection_for(manifest_for(third_party_id, a_few_permissions()),
                       trust_level::unsigned_plugin, false),
        plugin_errc::signature_missing});

    inspection denied = inspection_for(
        manifest_for(official_id, asking({permission::net_outbound}, {})),
        trust_level::official, true);
    deny_permissions(denied, {permission::net_outbound});
    refusals.push_back(refusal{denied, plugin_errc::permission_denied});

    inspection no_files = good_official();
    no_files.files.clear();
    refusals.push_back(refusal{no_files, plugin_errc::file_missing});

    for (const refusal& item : refusals)
        refused(item.found, nullptr, item.expected);
}

TEST_CASE("plugin start plan: a plan leaves the detail as it was")
{
    const plan_context context = context_for_test();

    std::string detail = "kept as it was";
    const vss::result<start_plan> started =
        plan_start(good_official(), nullptr, context, &detail);
    REQUIRE(started.has_value());
    REQUIRE(started.value().enabled);
    REQUIRE_EQ(detail, "kept as it was");

    // A plugin that is off is not a refusal either.
    const plugin_decision off = decision_for(official_id, false, {});
    const vss::result<start_plan> stopped =
        plan_start(good_official(), &off, context, &detail);
    REQUIRE(stopped.has_value());
    REQUIRE(!stopped.value().enabled);
    REQUIRE_EQ(detail, "kept as it was");

    // Nobody has to ask for the detail.
    inspection problem = good_official();
    problem.problem = make_error_code(plugin_errc::layout_invalid);
    const vss::result<start_plan> refused_plan =
        plan_start(problem, nullptr, context);
    REQUIRE(refused_plan.has_error());
}
