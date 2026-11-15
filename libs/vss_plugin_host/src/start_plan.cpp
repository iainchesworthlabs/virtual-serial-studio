//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/start_plan.hpp>

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

namespace
{

void say(std::string* detail, std::string_view text)
{
    if (detail != nullptr)
        *detail = std::string(text);
}

bool holds(const std::vector<permission>& list, permission wanted) noexcept
{
    return std::ranges::find(list, wanted) != list.end();
}

/// A path as UTF-8, whatever the system keeps it as.
std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

std::filesystem::path data_directory_of(const std::filesystem::path& data_root,
                                        std::string_view plugin_id)
{
    return data_root / std::filesystem::path(std::string(plugin_id)) / "data";
}

result<start_plan> plan_start(const inspection& found,
                              const plugin_decision* decision,
                              const plan_context& context, std::string* detail)
{
    // 1. It could be read and its checks passed.
    if (found.problem)
    {
        say(detail, found.detail);
        return found.problem;
    }
    if (!found.declared.has_value() || !found.level.has_value())
    {
        say(detail, "the plugin was not inspected");
        return make_error_code(plugin_errc::manifest_invalid);
    }
    if (found.run)
    {
        say(detail, found.run_detail);
        return found.run;
    }
    if (!found.verified.has_value())
    {
        say(detail, "the plugin was not checked");
        return make_error_code(plugin_errc::manifest_invalid);
    }
    const manifest& declared = *found.declared;
    const verify_result& verified = *found.verified;
    const bool official = *found.level == trust_level::official;

    // 2. The licence.  One that could not be read covers nothing.
    if (found.licence_problem)
    {
        say(detail, "the licence could not be checked");
        return make_error_code(plugin_errc::not_licensed);
    }
    if (found.licence_checked && found.licence.asked && !found.licence.granted)
    {
        say(detail, "the licence on this computer does not cover the plugin");
        return make_error_code(plugin_errc::not_licensed);
    }

    // 3. Somebody trusts it.
    if (!official && decision == nullptr)
    {
        say(detail, "the plugin waits for your decision");
        return make_error_code(*found.level == trust_level::unsigned_plugin
                                   ? plugin_errc::signature_missing
                                   : plugin_errc::key_unknown);
    }

    start_plan plan;
    // 4. It is switched on.
    plan.enabled = decision != nullptr ? decision->enabled : official;
    if (!plan.enabled)
        return plan;

    // 5. Permissions: what is granted, and whether that is enough.
    for (const permission_request& asked : declared.permissions)
    {
        const bool denied = holds(verified.denied_permissions, asked.name);
        const bool given =
            !denied &&
            (official || asked.name == permission::data_private ||
             (decision != nullptr && holds(decision->granted, asked.name)));
        if (given)
        {
            if (!holds(plan.granted, asked.name))
                plan.granted.push_back(asked.name);
            continue;
        }
        if (asked.required)
        {
            std::string text = "the plugin needs the permission ";
            text.append(to_string(asked.name));
            say(detail, text);
            return make_error_code(plugin_errc::permission_denied);
        }
        plan.withheld.push_back(asked.name);
    }
    // Its own data directory is always the plugin's.
    if (!holds(plan.granted, permission::data_private))
        plan.granted.push_back(permission::data_private);

    // 6. The program is one of the plugin's files.
    const bool listed =
        std::ranges::any_of(found.files, [&](const file_entry& file)
                            { return file.path == declared.run.executable; });
    if (!listed)
    {
        say(detail, "the program of the plugin is not one of its files");
        return make_error_code(plugin_errc::file_missing);
    }

    const std::filesystem::path data =
        data_directory_of(context.data_root, declared.id);

    process_request& process = plan.options.process;
    process.program =
        found.directory / std::filesystem::path(declared.run.executable);
    process.arguments = declared.run.args;
    process.working_directory = data;
    process.environment = plugin_environment(declared.id, data);

    sandbox_profile profile;
    profile.name = declared.id;
    profile.plugin_directory = found.directory;
    profile.data_directory = data;
    profile.network_listen = holds(plan.granted, permission::net_listen);
    profile.network_outbound = holds(plan.granted, permission::net_outbound);
    profile.start_programs = holds(plan.granted, permission::process_spawn);
    process.sandbox = std::move(profile);
    // A plugin the checks let run below the minimum is started with what
    // this computer has; any other needs the minimum or does not start.
    process.least =
        verified.below_min_sandbox ? sandbox_level::none : context.min_sandbox;

    session_options& session = plan.options.session;
    session.plugin_id = declared.id;
    session.version = to_string(declared.version);
    session.accepted = declared.needs.api;
    session.ack.granted = plan.granted;
    session.ack.capabilities = context.capabilities;
    session.ack.data_directory = utf8(data);

    plan.options.seed = context.seed;
    return plan;
}

} // namespace vss::plugin::host
