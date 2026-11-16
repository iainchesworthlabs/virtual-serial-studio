//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/service_mapping.hpp"

#include "backend/plugin_mapping.hpp"

#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/lifecycle.hpp>

#include <QCoreApplication>
#include <QDate>

#include <algorithm>
#include <cmath>

namespace vss::gui
{
namespace
{

namespace plug = vss::plugin;
namespace host = vss::plugin::host;

QString from_std(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString path_text(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

QDate date_of(std::chrono::year_month_day day)
{
    if (!day.ok())
        return {};
    return QDate(static_cast<int>(day.year()),
                 static_cast<int>(unsigned(day.month())),
                 static_cast<int>(unsigned(day.day())));
}

plugin_trust gui_trust(const std::optional<plug::trust_level>& level)
{
    if (!level.has_value())
        return plugin_trust::unsigned_plugin;
    switch (*level)
    {
    case plug::trust_level::official:
        return plugin_trust::official;
    case plug::trust_level::third_party_signed:
        return plugin_trust::third_party;
    case plug::trust_level::signed_unknown_key:
        return plugin_trust::unknown_key;
    case plug::trust_level::unsigned_plugin:
        return plugin_trust::unsigned_plugin;
    }
    return plugin_trust::unsigned_plugin;
}

plugin_installed_for installed_for_scope(plug::plugin_scope scope)
{
    switch (scope)
    {
    case plug::plugin_scope::with_studio:
        return plugin_installed_for::with_studio;
    case plug::plugin_scope::for_everyone:
        return plugin_installed_for::everyone;
    case plug::plugin_scope::for_user:
        return plugin_installed_for::you_only;
    }
    return plugin_installed_for::you_only;
}

bool is_plugin_code(const vss::error_code& code, plug::plugin_errc wanted)
{
    return code.category() == plug::plugin_category() &&
           code.value() == static_cast<int>(wanted);
}

QString code_text(const vss::error_code& code)
{
    if (!code.failed())
        return {};
    if (code.category() == plug::plugin_category())
    {
        return from_std(std::string(
            plug::to_string(static_cast<plug::plugin_errc>(code.value()))));
    }
    return QString::fromStdString(code.message());
}

plugin_addition_kind addition_kind(plug::extension_kind kind)
{
    switch (kind)
    {
    case plug::extension_kind::fixture:
        return plugin_addition_kind::fixture;
    case plug::extension_kind::generator:
        return plugin_addition_kind::generator;
    case plug::extension_kind::modifier:
    case plug::extension_kind::decoder:
        return plugin_addition_kind::link_behaviour;
    case plug::extension_kind::adapter:
        return plugin_addition_kind::connection;
    }
    return plugin_addition_kind::fixture;
}

plugin_isolation isolation_of(plug::sandbox_level level)
{
    switch (level)
    {
    case plug::sandbox_level::full:
        return plugin_isolation::full;
    case plug::sandbox_level::partial:
        return plugin_isolation::partial;
    case plug::sandbox_level::none:
        return plugin_isolation::none;
    }
    return plugin_isolation::none;
}

QString permission_id(plug::permission value)
{
    return from_std(std::string(plug::to_string(value)));
}

bool contains(const std::vector<plug::permission>& list, plug::permission value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool has_hash(const host::service_plugin& plugin)
{
    return plugin.hash != plug::plugin_hash{};
}

void fill_from_manifest(plugin_view& view, const plug::manifest& declared)
{
    view.description = from_std(declared.description);
    if (declared.not_after.has_value())
        view.expires = date_of(*declared.not_after);
    bool in_line = false;
    bool beside = false;
    for (const plug::extension_point& point : declared.extension_points)
    {
        plugin_addition addition;
        addition.kind = addition_kind(point.kind);
        const std::string_view best = point.name.best("");
        addition.type_name =
            from_std(best.empty() ? point.id : std::string(best));
        addition.type_id = from_std(point.id);
        addition.description =
            from_std(std::string(point.description.best("")));
        view.adds.push_back(addition);
        if (point.timing.type == plug::timing_class::in_line)
        {
            in_line = true;
            view.budget_us = std::max<int>(
                view.budget_us, static_cast<int>(point.timing.budget_us));
        }
        else if (point.timing.type == plug::timing_class::async)
        {
            beside = true;
        }
    }
    view.timing =
        in_line ? plugin_timing::in_path
                : (beside ? plugin_timing::beside : plugin_timing::by_studio);
    QStringList runtimes;
    for (const std::string& runtime : declared.needs.runtime)
        runtimes << from_std(runtime);
    view.runtime_name = runtimes.join(QStringLiteral(", "));
}

/// The reason the licence does not cover the plugin, in the licence library's
/// own terms.
vss::license::reason licence_reason_for(const host::service_plugin& plugin,
                                        const vss::license::status& licence)
{
    vss::license::entitlement said =
        vss::license::check(licence, plug::plugin_feature(plugin.id));
    if (!said.granted)
    {
        said = vss::license::check(licence, plug::all_official_plugins_feature);
    }
    return said.reason;
}

/// The name of the limit a plugin went over, from the host's line, when it
/// has one in parentheses: "... (frames per second)".
QString limit_from(const std::string& detail)
{
    const auto open = detail.find('(');
    const auto close = detail.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos ||
        close <= open + 1)
        return {};
    return from_std(detail.substr(open + 1, close - open - 1));
}

} // namespace

void fill_view_from_manifest(plugin_view& view, const plug::manifest& declared)
{
    view.id = from_std(declared.id);
    view.name = from_std(declared.name);
    view.version = from_std(plug::to_string(declared.version));
    view.author = from_std(declared.author.name);
    fill_from_manifest(view, declared);
    for (const plug::permission_request& request : declared.permissions)
    {
        plugin_permission_view permission;
        permission.id = permission_id(request.name);
        permission.required = request.required;
        permission.reason = from_std(request.reason);
        for (const std::string& scope : request.scope)
            permission.scope << from_std(scope);
        view.permissions.push_back(permission);
    }
}

QString sandbox_missing_text(const host::sandbox_report& report)
{
    QStringList parts;
    for (const host::sandbox_part part : report.not_applied())
    {
        switch (part)
        {
        case host::sandbox_part::containment:
            parts << QCoreApplication::translate("PluginSandbox",
                                                 "no containment");
            break;
        case host::sandbox_part::file_system:
            parts << QCoreApplication::translate("PluginSandbox",
                                                 "no file isolation");
            break;
        case host::sandbox_part::network:
            parts << QCoreApplication::translate("PluginSandbox",
                                                 "no network isolation");
            break;
        case host::sandbox_part::other_processes:
            parts << QCoreApplication::translate(
                "PluginSandbox", "other programs are not kept out of reach");
            break;
        case host::sandbox_part::system_calls:
            parts << QCoreApplication::translate(
                "PluginSandbox", "system calls are not limited");
            break;
        case host::sandbox_part::user_interface:
            parts << QCoreApplication::translate(
                "PluginSandbox", "the desktop is not kept out of reach");
            break;
        case host::sandbox_part::limits:
            parts << QCoreApplication::translate("PluginSandbox",
                                                 "no limits on resources");
            break;
        }
    }
    return parts.join(QStringLiteral(", "));
}

plugin_snapshot map_service_snapshot(const host::service_snapshot& snapshot,
                                     const service_mapping_inputs& inputs,
                                     std::chrono::steady_clock::time_point now)
{
    plugin_snapshot out;
    out.can_run_plugins = true;
    // Until the first scan has finished, nothing has been found yet.
    out.looking = snapshot.scans == 0;
    out.machine_folder = inputs.machine_folder;
    out.user_folder = inputs.user_folder;
    out.user_plugins_allowed =
        !inputs.rules.has_value() || inputs.rules->user_plugins;
    const bool override_allowed = plug::may_override_sandbox(
        inputs.rules ? &*inputs.rules : nullptr, inputs.install);

    // A line about the computer that names the policy: the file could not be
    // trusted.
    for (const std::string& note : snapshot.notes)
    {
        const QString line = from_std(note);
        if (line.contains(QStringLiteral("policy"), Qt::CaseInsensitive))
        {
            out.banner = plugin_banner::policy_untrusted;
            out.policy_detail +=
                (out.policy_detail.isEmpty() ? QString()
                                             : QStringLiteral("\n")) +
                line;
        }
    }

    bool official_installed = false;
    for (const host::service_plugin& plugin : snapshot.plugins)
    {
        plugin_view view;
        view.id = from_std(plugin.id);
        view.name = from_std(plugin.name);
        view.version = from_std(plugin.version);
        view.author = from_std(plugin.author);
        view.folder = path_text(plugin.directory);
        view.installed_for = installed_for_scope(plugin.scope);
        view.trust = gui_trust(plugin.level);
        view.key_id = from_std(plugin.key_id);
        view.reason_code = code_text(plugin.problem);
        view.detail = from_std(plugin.detail);
        if (has_hash(plugin))
        {
            view.hash = hash_text(plugin.hash);
            view.fingerprint = from_std(plugin.hash.short_text());
        }
        if (plugin.level == plug::trust_level::official)
            official_installed = true;
        // The key's name and day come from the keys the service holds.
        for (const plug::approved_key& key : snapshot.approved_keys)
        {
            if (key.key_id == plugin.key_id)
            {
                view.key_name = from_std(key.name);
                view.key_approved_on = date_of(key.approved_on);
            }
        }
        // What the manifest says, when the service could read it.
        if (plugin.declared.has_value())
            fill_from_manifest(view, *plugin.declared);
        if (view.name.isEmpty())
            view.name = path_text(plugin.directory.filename());
        if (view.id.isEmpty())
            view.id = view.name;

        // What was asked and what the plugin gets.
        for (const plug::permission_request& request : plugin.asked)
        {
            plugin_permission_view permission;
            permission.id = permission_id(request.name);
            permission.required = request.required;
            permission.reason = from_std(request.reason);
            for (const std::string& scope : request.scope)
                permission.scope << from_std(scope);
            permission.turned_down = contains(plugin.withheld, request.name);
            view.permissions.push_back(permission);
        }

        // The decision, if there is one: the wish and the override.
        const plug::plugin_decision* decision =
            plugin.decision.has_value() ? &*plugin.decision : nullptr;
        view.wished_on = plugin.enabled;
        view.override_allowed = override_allowed;
        if (decision != nullptr)
        {
            view.override_active = decision->run_without_sandbox;
            view.override_on = date_of(decision->decided_on);
        }
        // What this plugin's sandbox is, or what this computer can give it.
        const host::sandbox_report& report =
            plugin.run ? plugin.run->sandbox : inputs.available;
        view.isolation = isolation_of(report.level());
        if (view.isolation == plugin_isolation::partial)
            view.isolation_missing = sandbox_missing_text(report);
        view.studio_version = from_std(inputs.studio_version);

        if (plugin.run)
        {
            const host::runner_snapshot& run = *plugin.run;
            if (run.status.has_value())
                view.status_text = from_std(run.status->text);
            for (const host::runner_log_line& line : run.log)
            {
                plugin_log_line mapped;
                mapped.text = from_std(line.text);
                view.log.push_back(std::move(mapped));
            }
        }

        const auto finish = [&](plugin_state state, plugin_note note)
        {
            view.state = state;
            view.note = note;
            out.plugins.push_back(view);
        };

        // -- the plugin may not run, for a reason the host found
        if (plugin.problem.failed())
        {
            const bool plugin_code =
                plugin.problem.category() == plug::plugin_category();
            const auto code =
                plugin_code
                    ? static_cast<plug::plugin_errc>(plugin.problem.value())
                    : plug::plugin_errc::manifest_invalid;
            using code_t = plug::plugin_errc;
            switch (code)
            {
            case code_t::signature_missing:
            case code_t::key_unknown:
                finish(plugin_state::needs_decision,
                       view.differs_from_approved
                           ? plugin_note::decision_changed
                           : plugin_note::decision_first);
                continue;
            case code_t::changed:
                view.differs_from_approved = true;
                finish(plugin_state::needs_decision,
                       plugin_note::decision_changed);
                continue;
            case code_t::policy:
            {
                const bool official_only =
                    inputs.rules.has_value() &&
                    inputs.rules->allow == plug::allow_level::official_only &&
                    plugin.level != plug::trust_level::official;
                const bool named =
                    inputs.rules.has_value() &&
                    std::find(inputs.rules->deny_plugins.begin(),
                              inputs.rules->deny_plugins.end(),
                              plugin.id) != inputs.rules->deny_plugins.end();
                finish(plugin_state::blocked,
                       named
                           ? plugin_note::blocked_named
                           : (official_only ? plugin_note::blocked_official_only
                                            : plugin_note::blocked_named));
                continue;
            }
            case code_t::sandbox_insufficient:
            {
                const bool policy_forbids =
                    inputs.rules.has_value() && !inputs.rules->allow_no_sandbox;
                if (override_allowed)
                    finish(plugin_state::needs_isolation,
                           plugin_note::isolation_override);
                else
                    finish(plugin_state::blocked,
                           policy_forbids ? plugin_note::blocked_protection
                                          : plugin_note::isolation_no_override);
                continue;
            }
            case code_t::not_licensed:
                view.licence_reason =
                    licence_reason_for(plugin, inputs.licence);
                view.licence_status = inputs.licence;
                view.wished_on = true;
                finish(plugin_state::needs_licence, plugin_note::needs_licence);
                continue;
            case code_t::api_incompatible:
            case code_t::manifest_unsupported_version:
                finish(plugin_state::needs_newer_studio,
                       plugin_note::newer_studio_version);
                continue;
            case code_t::capability_missing:
                view.capability_name = view.detail;
                finish(plugin_state::needs_newer_studio,
                       plugin_note::newer_studio_capability);
                continue;
            case code_t::shadowed:
            {
                plugin_note note = plugin_note::not_used_same;
                for (const host::service_plugin& other : snapshot.plugins)
                {
                    if (&other == &plugin || other.id != plugin.id ||
                        is_plugin_code(other.problem, code_t::shadowed))
                        continue;
                    view.used_copy_folder = path_text(other.directory);
                    view.used_copy_trust = gui_trust(other.level);
                    view.used_copy_author = from_std(other.author);
                    view.used_copy_id = from_std(other.id);
                    if (other.level != plugin.level ||
                        other.key_id != plugin.key_id)
                        note = plugin_note::not_used_different_signer;
                    break;
                }
                finish(plugin_state::not_used, note);
                continue;
            }
            case code_t::reserved_id:
                finish(plugin_state::not_used, plugin_note::not_used_reserved);
                continue;
            case code_t::revoked:
                finish(plugin_state::cannot_be_used, plugin_note::withdrawn);
                continue;
            case code_t::expired:
                finish(plugin_state::cannot_be_used, plugin_note::expired);
                continue;
            case code_t::signature_invalid:
            case code_t::unlisted_file:
                finish(plugin_state::cannot_be_used,
                       plugin_note::invalid_signature);
                continue;
            default:
                finish(plugin_state::cannot_be_used,
                       plugin_note::invalid_manifest);
                continue;
            }
        }

        // -- it may run: what its run says
        if (plugin.run)
        {
            const host::runner_snapshot& run = *plugin.run;
            switch (run.state)
            {
            case plug::run_state::ready:
                finish(plugin_state::available, plugin_note::none);
                continue;
            case plug::run_state::starting:
                finish(plugin_state::starting, plugin_note::starting);
                continue;
            case plug::run_state::stopping:
                finish(plugin_state::starting, plugin_note::none);
                continue;
            case plug::run_state::backing_off:
            {
                view.attempt = static_cast<int>(run.failures) + 1;
                view.failure_count = static_cast<int>(run.failures);
                int seconds = 1;
                if (run.next_start.has_value())
                {
                    const auto wait = *run.next_start - now;
                    seconds = std::max<int>(
                        1, static_cast<int>(std::ceil(
                               std::chrono::duration<double>(wait).count())));
                }
                view.back_off_seconds = seconds;
                finish(plugin_state::starting, plugin_note::backing_off);
                continue;
            }
            case plug::run_state::quarantined:
            {
                view.failure_count = static_cast<int>(run.failures);
                plugin_note note = plugin_note::stopped_failures;
                if (is_plugin_code(run.last_failure,
                                   plug::plugin_errc::unresponsive))
                    note = plugin_note::stopped_unresponsive;
                else if (is_plugin_code(run.last_failure,
                                        plug::plugin_errc::misbehaving))
                {
                    note = plugin_note::stopped_limit;
                    view.limit_name = limit_from(run.detail);
                }
                if (run.last_failure.failed())
                    view.reason_code = code_text(run.last_failure);
                if (!run.detail.empty())
                    view.detail = from_std(run.detail);
                view.wished_on = true;
                finish(plugin_state::stopped, note);
                continue;
            }
            case plug::run_state::stopped:
                break;
            }
        }
        // No run, or one that is stopped: Off when the user did not switch
        // it on, and about to start when they did.
        if (plugin.enabled)
            finish(plugin_state::starting, plugin_note::starting);
        else
            finish(plugin_state::off, plugin_note::none);
    }

    // -- the note at the top of the page
    if (out.banner != plugin_banner::policy_untrusted)
    {
        if (inputs.rules.has_value() &&
            inputs.rules->allow == plug::allow_level::official_only)
            out.banner = plugin_banner::official_only;
        else if (!out.user_plugins_allowed)
            out.banner = plugin_banner::user_plugins_off;
        else if (official_installed &&
                 inputs.licence.state == vss::license::state::none)
            out.banner = plugin_banner::no_licence_for_official;
    }
    return out;
}

} // namespace vss::gui
