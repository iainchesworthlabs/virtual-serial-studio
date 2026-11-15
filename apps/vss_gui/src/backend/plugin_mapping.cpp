//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/plugin_mapping.hpp"

#include <vss/plugin/access.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/signature.hpp>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace vss::gui
{
namespace
{

namespace plug = vss::plugin;
namespace fs = std::filesystem;

QString path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

QString from_std(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::optional<std::string> read_text_file(const fs::path& path,
                                          std::size_t limit)
{
    if (path.empty())
        return std::nullopt;
    std::error_code ignored;
    if (!fs::is_regular_file(path, ignored))
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    if (text.size() > limit)
        return std::nullopt;
    return text;
}

plugin_trust gui_trust(plug::trust_level level)
{
    switch (level)
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

plug::trust_level level_of(const plug::signature_finding& finding)
{
    switch (finding.state)
    {
    case plug::signature_state::valid:
        return finding.origin == plug::key_origin::official
                   ? plug::trust_level::official
                   : plug::trust_level::third_party_signed;
    case plug::signature_state::unknown_key:
        return plug::trust_level::signed_unknown_key;
    case plug::signature_state::absent:
    case plug::signature_state::invalid:
        return plug::trust_level::unsigned_plugin;
    }
    return plug::trust_level::unsigned_plugin;
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

QString reason_text(const vss::error_code& problem)
{
    if (!problem.failed())
        return {};
    if (problem.category() == plug::plugin_category())
    {
        return from_std(std::string(
            plug::to_string(static_cast<plug::plugin_errc>(problem.value()))));
    }
    return QString::fromStdString(problem.message());
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

/// The part of a view that comes from the manifest.
void fill_from_manifest(plugin_view& view, const plug::manifest& declared)
{
    view.id = from_std(declared.id);
    view.name = from_std(declared.name);
    view.version = from_std(plug::to_string(declared.version));
    view.author = from_std(declared.author.name);
    view.description = from_std(declared.description);
    if (declared.not_after.has_value())
    {
        const auto day = declared.not_after.value();
        view.expires = QDate(static_cast<int>(day.year()),
                             static_cast<int>(unsigned(day.month())),
                             static_cast<int>(unsigned(day.day())));
    }

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
        addition.description = from_std(std::string(point.description.best("")));
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

    for (const plug::permission_request& request : declared.permissions)
    {
        plugin_permission_view permission;
        permission.id = from_std(std::string(plug::to_string(request.name)));
        permission.required = request.required;
        permission.reason = from_std(request.reason);
        for (const std::string& scope : request.scope)
            permission.scope << from_std(scope);
        view.permissions.push_back(permission);
    }
}

bool has_permission(const std::vector<plug::permission>& list,
                    const QString& id)
{
    return std::any_of(
        list.begin(), list.end(), [&id](plug::permission each)
        { return from_std(std::string(plug::to_string(each))) == id; });
}

} // namespace

QString hash_text(const plug::plugin_hash& hash)
{
    return from_std(plug::to_hex(hash.bytes));
}

std::optional<plug::plugin_hash> parse_hash_text(const QString& text)
{
    if (text.size() != 128)
        return std::nullopt;
    plug::plugin_hash hash;
    for (qsizetype index = 0; index < 64; ++index)
    {
        bool ok = false;
        const int value = text.mid(index * 2, 2).toInt(&ok, 16);
        if (!ok)
            return std::nullopt;
        hash.bytes[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value);
    }
    return hash;
}

plugin_reason_state state_for_reason(plug::plugin_errc code,
                                     bool override_possible)
{
    using code_t = plug::plugin_errc;
    plugin_reason_state out;
    switch (code)
    {
    case code_t::success:
        out.state = plugin_state::off;
        out.note = plugin_note::none;
        break;
    case code_t::manifest_unsupported_version:
    case code_t::api_incompatible:
        out.state = plugin_state::needs_newer_studio;
        out.note = plugin_note::newer_studio_version;
        break;
    case code_t::capability_missing:
        out.state = plugin_state::needs_newer_studio;
        out.note = plugin_note::newer_studio_capability;
        break;
    case code_t::shadowed:
        out.state = plugin_state::not_used;
        out.note = plugin_note::not_used_same;
        break;
    case code_t::reserved_id:
        out.state = plugin_state::not_used;
        out.note = plugin_note::not_used_reserved;
        break;
    case code_t::policy:
        out.state = plugin_state::blocked;
        out.note = plugin_note::blocked_named;
        break;
    case code_t::sandbox_insufficient:
        out.state = override_possible ? plugin_state::needs_isolation
                                      : plugin_state::blocked;
        out.note = override_possible ? plugin_note::isolation_override
                                     : plugin_note::isolation_no_override;
        break;
    case code_t::not_licensed:
        out.state = plugin_state::needs_licence;
        out.note = plugin_note::needs_licence;
        break;
    case code_t::signature_missing:
    case code_t::key_unknown:
    case code_t::changed:
        out.state = plugin_state::needs_decision;
        out.note = plugin_note::decision_first;
        break;
    case code_t::quarantined:
    case code_t::start_failed:
    case code_t::no_hello:
    case code_t::bad_hello:
    case code_t::crashed:
    case code_t::exited:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_failures;
        break;
    case code_t::unresponsive:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_unresponsive;
        break;
    case code_t::misbehaving:
    case code_t::permission_denied:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_limit;
        break;
    case code_t::signature_invalid:
    case code_t::unlisted_file:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::invalid_signature;
        break;
    case code_t::revoked:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::withdrawn;
        break;
    case code_t::expired:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::expired;
        break;
    case code_t::manifest_invalid:
    case code_t::manifest_too_large:
    case code_t::too_many:
    case code_t::layout_invalid:
    case code_t::file_missing:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::invalid_manifest;
        break;
    }
    return out;
}

plugin_gathered gather_plugins(const plugin_gather_options& options,
                               const plug::trust_store& store)
{
    plugin_gathered out;
    plugin_snapshot& snapshot = out.snapshot;
    snapshot.can_run_plugins = options.can_run_plugins;
    snapshot.machine_folder = from_std(options.machine_folder_text);
    snapshot.user_folder = from_std(options.user_folder_text);
    out.trust_store_file = options.trust_store_file;

    // -- the administrator's policy
    bool policy_unusable = false;
    if (!options.policy_file.empty())
    {
        const plug::loaded_policy loaded =
            plug::load_policy(options.policy_file);
        out.rules = loaded.rules;
        if (loaded.rules.has_value() && loaded.problem.failed())
        {
            policy_unusable = true;
            snapshot.policy_detail = QString::fromStdString(
                loaded.detail.empty() ? loaded.problem.message()
                                      : loaded.detail);
        }
    }
    const plug::policy* rules =
        out.rules.has_value() ? &out.rules.value() : nullptr;
    snapshot.user_plugins_allowed = rules == nullptr || rules->user_plugins;

    plug::install_kind install = plug::install_kind::for_everyone;
    if (options.install.has_value())
    {
        install = options.install.value();
    }
    else if (!options.program_directory.empty())
    {
        const auto kind = plug::kind_of_install(options.program_directory);
        if (kind.has_value())
            install = kind.value();
    }

    // -- keys and lists
    if (const auto text =
            read_text_file(options.official_keys_file, std::size_t{1} << 20))
    {
        auto list = plug::read_key_list(
            *text, options.root_keys.empty()
                       ? plug::builtin_root_keys()
                       : std::span<const plug::root_key>(options.root_keys));
        if (list.has_value())
            out.official = std::move(list.value());
    }
    std::optional<plug::revocation_list> revocations;
    if (out.official.has_value())
    {
        if (const auto text = read_text_file(options.revocations_file,
                                             plug::max_revocations_size))
        {
            auto list = plug::read_revocations(*text, out.official.value());
            if (list.has_value())
                revocations = std::move(list.value());
        }
    }
    std::vector<plug::approved_signer> administrator_keys;
    if (rules != nullptr)
    {
        for (const plug::policy_key& key : rules->allow_keys)
            administrator_keys.push_back({key.key_id, key.public_key});
    }
    std::vector<plug::approved_signer> user_keys;
    for (const plug::approved_key& key : store.keys())
        user_keys.push_back({key.key_id, key.public_key});

    std::chrono::sys_days today =
        options.today.value_or(std::chrono::floor<std::chrono::days>(
            std::chrono::system_clock::now()));
    if (store.ledger().latest_date.has_value())
    {
        today = std::max<std::chrono::sys_days>(
            today, std::chrono::sys_days(store.ledger().latest_date.value()));
    }

    // -- what is installed
    std::vector<plug::plugin_root> roots;
    for (const plug::plugin_root& root : options.roots)
    {
        if (root.scope == plug::plugin_scope::for_user &&
            !snapshot.user_plugins_allowed)
            continue;
        roots.push_back(root);
        out.root_folders.push_back(root.directory);
        if (root.scope == plug::plugin_scope::for_user)
            out.user_root = root.directory;
    }
    const plug::discovery found = plug::discover_plugins(roots);

    // The signature of every plugin whose manifest could be read.
    std::vector<plug::signature_finding> signatures(found.plugins.size());
    for (std::size_t index = 0; index < found.plugins.size(); ++index)
    {
        const plug::found_plugin& each = found.plugins[index];
        if (!each.declared.has_value() || each.manifest_bytes.empty())
            continue;
        plug::signature_input input;
        input.manifest_bytes = each.manifest_bytes;
        input.declared = &each.declared.value();
        if (each.signature_bytes.has_value())
            input.signature_text = std::string_view(*each.signature_bytes);
        input.official =
            out.official.has_value() ? &out.official.value() : nullptr;
        input.administrator_keys = administrator_keys;
        input.user_keys = user_keys;
        input.today = today;
        signatures[index] = plug::check_signature(input);
    }

    bool official_installed = false;
    for (std::size_t index = 0; index < found.plugins.size(); ++index)
    {
        const plug::found_plugin& each = found.plugins[index];
        const plug::signature_finding& signature = signatures[index];
        plugin_view view;
        view.folder = path_text(each.directory);
        view.installed_for = installed_for_scope(each.scope);
        view.reason_code = reason_text(each.problem);
        view.detail = QString::fromStdString(each.detail);
        view.studio_version = from_std(options.studio_version);
        if (!each.files.empty())
        {
            view.hash = hash_text(each.hash);
            view.fingerprint = from_std(each.hash.short_text());
        }
        const plug::trust_level level = level_of(signature);
        view.trust = gui_trust(level);
        view.key_id = from_std(signature.key_id);
        view.isolation = isolation_of(options.sandbox);
        view.isolation_missing = from_std(options.isolation_missing);
        out.plugin_folders.push_back(each.directory);

        if (each.declared.has_value())
            fill_from_manifest(view, each.declared.value());
        else
        {
            const QString folder_name = path_text(each.directory.filename());
            view.id = folder_name;
            view.name = folder_name;
        }
        if (each.declared.has_value() && !signature.key_id.empty())
        {
            if (const plug::approved_key* key =
                    store.find_key(signature.key_id))
            {
                view.key_name = from_std(key->name);
                view.key_approved_on =
                    QDate(static_cast<int>(key->approved_on.year()),
                          static_cast<int>(unsigned(key->approved_on.month())),
                          static_cast<int>(unsigned(key->approved_on.day())));
            }
            else if (rules != nullptr)
            {
                for (const plug::policy_key& allowed : rules->allow_keys)
                {
                    if (allowed.key_id == signature.key_id)
                        view.key_name = from_std(allowed.name);
                }
            }
        }
        if (level == plug::trust_level::official)
            official_installed = true;

        plugin_record record;
        record.id = view.id;
        record.hash = view.hash;
        record.folder = view.folder;
        record.plugin_hash = each.hash;
        record.level = level;
        record.key_id = signature.key_id;
        record.scope = each.scope;
        record.name =
            each.declared.has_value() ? each.declared->name : std::string();
        if (each.declared.has_value())
        {
            for (const plug::permission_request& request :
                 each.declared->permissions)
            {
                record.declared.emplace_back(
                    std::string(plug::to_string(request.name)),
                    request.required);
            }
        }

        const auto finish = [&](plugin_state state, plugin_note note)
        {
            view.state = state;
            view.note = note;
            out.records.push_back(record);
            snapshot.plugins.push_back(view);
        };
        const auto fail_with = [&](plug::plugin_errc code)
        {
            const plugin_reason_state mapped = state_for_reason(code, false);
            view.reason_code = from_std(std::string(plug::to_string(code)));
            finish(mapped.state, mapped.note);
        };

        // -- a plugin that was found but cannot be read, or is shadowed
        if (each.problem.failed())
        {
            if (each.problem.category() == plug::plugin_category())
            {
                const auto code =
                    static_cast<plug::plugin_errc>(each.problem.value());
                if (code == plug::plugin_errc::shadowed)
                {
                    const plug::found_plugin* winner =
                        each.shadowed_by.has_value() &&
                                each.shadowed_by.value() < found.plugins.size()
                            ? &found.plugins[each.shadowed_by.value()]
                            : nullptr;
                    plugin_note note = plugin_note::not_used_same;
                    if (winner != nullptr)
                    {
                        const plug::signature_finding& other =
                            signatures[each.shadowed_by.value()];
                        view.used_copy_folder = path_text(winner->directory);
                        view.used_copy_trust = gui_trust(level_of(other));
                        view.used_copy_id = from_std(
                            winner->declared.has_value() ? winner->declared->id
                                                         : std::string());
                        QString winner_author =
                            winner->declared.has_value()
                                ? from_std(winner->declared->author.name)
                                : QString();
                        if (const plug::approved_key* key =
                                store.find_key(other.key_id);
                            key != nullptr &&
                            view.used_copy_trust == plugin_trust::third_party)
                            winner_author = from_std(key->name);
                        view.used_copy_author = winner_author;
                        if (level_of(other) != level ||
                            other.key_id != signature.key_id)
                            note = plugin_note::not_used_different_signer;
                        if (winner->declared.has_value() &&
                            each.declared.has_value())
                        {
                            view.newer_than_used_copy =
                                plug::compare(each.declared->version,
                                              winner->declared->version) ==
                                std::strong_ordering::greater;
                        }
                    }
                    finish(plugin_state::not_used, note);
                    continue;
                }
                if (code == plug::plugin_errc::signature_invalid)
                {
                    fail_with(code);
                    continue;
                }
                fail_with(code);
                continue;
            }
            // A system error: the files could not be read.
            finish(plugin_state::cannot_be_used, plugin_note::invalid_manifest);
            continue;
        }
        if (!each.declared.has_value())
        {
            finish(plugin_state::cannot_be_used, plugin_note::invalid_manifest);
            continue;
        }
        const plug::manifest& declared = each.declared.value();

        // -- the host's verdict
        plug::verify_input input;
        input.declared = &declared;
        input.hash = each.hash;
        input.signature = signature;
        input.revocations =
            revocations.has_value() ? &revocations.value() : nullptr;
        input.rules = policy_unusable ? nullptr : rules;
        const plug::policy unusable_rules = plug::official_only_policy();
        if (policy_unusable)
            input.rules = &unusable_rules;
        input.today = today;
        input.host_api = options.host_api;
        input.capabilities = options.capabilities;
        input.sandbox = options.sandbox;
        input.install = install;
        const plug::plugin_decision* decision =
            store.find(declared.id, each.hash, signature.key_id);
        input.sandbox_override =
            decision != nullptr && decision->run_without_sandbox;
        const std::string expired_key =
            declared.id + "|" + plug::to_hex(each.hash.bytes);
        input.run_expired = options.run_expired.contains(expired_key) &&
                            level != plug::trust_level::official;

        std::string verdict_detail;
        const vss::result<plug::verify_result> verdict =
            plug::verify(input, &verdict_detail);
        if (!verdict_detail.empty())
            view.detail = QString::fromStdString(verdict_detail);

        bool sandbox_gate = false;
        plug::verify_result accepted;
        if (verdict.has_value())
        {
            accepted = verdict.value();
        }
        else
        {
            const vss::error_code why = verdict.error();
            view.reason_code = reason_text(why);
            if (why.category() != plug::plugin_category())
            {
                finish(plugin_state::cannot_be_used,
                       plugin_note::invalid_manifest);
                continue;
            }
            const auto code = static_cast<plug::plugin_errc>(why.value());
            if (code == plug::plugin_errc::sandbox_insufficient)
            {
                sandbox_gate = true;
            }
            else
            {
                plugin_reason_state mapped = state_for_reason(code, false);
                if (code == plug::plugin_errc::policy)
                {
                    const bool official_only =
                        input.rules != nullptr &&
                        input.rules->allow ==
                            plug::allow_level::official_only &&
                        level != plug::trust_level::official;
                    const bool named =
                        input.rules != nullptr &&
                        std::find(input.rules->deny_plugins.begin(),
                                  input.rules->deny_plugins.end(),
                                  declared.id) !=
                            input.rules->deny_plugins.end();
                    mapped.note =
                        named ? plugin_note::blocked_named
                              : (official_only
                                     ? plugin_note::blocked_official_only
                                     : plugin_note::blocked_named);
                }
                else if (code == plug::plugin_errc::capability_missing)
                {
                    for (const std::string& need : declared.needs.capabilities)
                    {
                        if (std::find(options.capabilities.begin(),
                                      options.capabilities.end(),
                                      need) == options.capabilities.end())
                        {
                            view.capability_name = from_std(need);
                            break;
                        }
                    }
                }
                else if (code == plug::plugin_errc::expired)
                {
                    view.can_run_expired = level != plug::trust_level::official;
                }
                finish(mapped.state, mapped.note);
                continue;
            }
        }

        // The signature, the files and the policy hold. Denied permissions
        // are shown as turned down.
        for (plugin_permission_view& permission : view.permissions)
        {
            if (has_permission(accepted.denied_permissions, permission.id))
                permission.turned_down = true;
        }

        view.override_allowed = plug::may_override_sandbox(rules, install);
        view.override_active =
            decision != nullptr && decision->run_without_sandbox;
        if (decision != nullptr && decision->decided_on.ok())
        {
            view.override_on =
                QDate(static_cast<int>(decision->decided_on.year()),
                      static_cast<int>(unsigned(decision->decided_on.month())),
                      static_cast<int>(unsigned(decision->decided_on.day())));
        }
        view.wished_on = decision != nullptr && decision->enabled;

        // -- the user's own decision comes before anything that would run it
        if (decision == nullptr)
        {
            const plug::plugin_decision* before =
                store.previous(declared.id, each.hash, signature.key_id);
            if (before != nullptr)
            {
                view.differs_from_approved = true;
                for (plugin_permission_view& permission : view.permissions)
                {
                    permission.is_new =
                        !has_permission(before->granted, permission.id);
                }
            }
            view.reason_code = before != nullptr ? QStringLiteral("changed")
                                                 : QStringLiteral("untrusted");
            finish(plugin_state::needs_decision,
                   before != nullptr ? plugin_note::decision_changed
                                     : plugin_note::decision_first);
            continue;
        }
        for (plugin_permission_view& permission : view.permissions)
        {
            if (!permission.required &&
                !has_permission(decision->granted, permission.id))
                permission.turned_down = true;
        }

        if (sandbox_gate)
        {
            const bool policy_forbids =
                rules != nullptr && !rules->allow_no_sandbox;
            const bool override_possible = view.override_allowed;
            if (override_possible)
                finish(plugin_state::needs_isolation,
                       plugin_note::isolation_override);
            else
                finish(plugin_state::blocked,
                       policy_forbids ? plugin_note::blocked_protection
                                      : plugin_note::isolation_no_override);
            continue;
        }

        // -- the licence
        plug::licence_answer answer;
        const vss::result<void> licensed =
            plug::check_licence(level, declared, options.licence, &answer);
        if (!licensed.has_value())
        {
            view.licence_reason = answer.reason;
            view.licence_status = options.licence;
            view.reason_code = QStringLiteral("not_licensed");
            finish(plugin_state::needs_licence, plugin_note::needs_licence);
            continue;
        }

        // Everything allows it. This build cannot start a plugin, so it is
        // Off, and the wish stays what the user said.
        finish(options.can_run_plugins && decision->enabled
                   ? plugin_state::available
                   : plugin_state::off,
               plugin_note::none);
    }

    // -- the note at the top of the page
    if (policy_unusable)
        snapshot.banner = plugin_banner::policy_untrusted;
    else if (rules != nullptr &&
             rules->allow == plug::allow_level::official_only)
        snapshot.banner = plugin_banner::official_only;
    else if (!snapshot.user_plugins_allowed)
        snapshot.banner = plugin_banner::user_plugins_off;
    else if (official_installed &&
             options.licence.state == vss::license::state::none)
        snapshot.banner = plugin_banner::no_licence_for_official;
    return out;
}

plugin_key_file_view read_key_file_view(const fs::path& path,
                                        const plugin_gathered& found,
                                        const plug::trust_store& store,
                                        plug::approved_key* key_out)
{
    plugin_key_file_view view;
    view.path = path_text(path);
    view.failure = plugin_key_file_view::problem::not_a_key_file;

    const auto text = read_text_file(path, std::size_t{16} * 1024);
    if (!text.has_value())
        return view;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(*text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return view;
    const QJsonObject object = document.object();
    const QString key_id = object.value(QStringLiteral("key_id")).toString();
    const QString public_key =
        object.value(QStringLiteral("public_key")).toString();
    const QString name = object.value(QStringLiteral("name")).toString();
    if (key_id.isEmpty() || public_key.size() != 64 || name.isEmpty())
        return view;
    if (!plug::is_key_id(key_id.toStdString()) ||
        name.toUtf8().size() > static_cast<qsizetype>(plug::max_name_length))
        return view;
    // A name with a control character or a direction mark is not plain text.
    for (const QChar letter : name)
    {
        const char16_t unit = letter.unicode();
        if (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f) ||
            (unit >= 0x202a && unit <= 0x202e) ||
            (unit >= 0x2066 && unit <= 0x2069) || unit == 0x200e ||
            unit == 0x200f || unit == 0x061c || unit == 0x2028 ||
            unit == 0x2029)
        {
            view.failure = plugin_key_file_view::problem::text_not_plain;
            return view;
        }
    }
    plug::approved_key approved;
    approved.key_id = key_id.toStdString();
    approved.name = name.toStdString();
    for (qsizetype index = 0; index < 32; ++index)
    {
        bool ok = false;
        const int value = public_key.mid(index * 2, 2).toInt(&ok, 16);
        if (!ok)
            return view;
        approved.public_key[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value);
    }

    view.author = name;
    view.key_id = key_id;
    const plug::sha512_digest digest = plug::sha512_of(std::string_view(
        reinterpret_cast<const char*>(approved.public_key.data()),
        approved.public_key.size()));
    view.fingerprint = from_std(plug::to_hex(digest)).left(16);

    // An id that an official key or an administrator's key has is never taken.
    bool taken_by_other = false;
    if (found.official.has_value() &&
        found.official->find(approved.key_id, plug::plugin_purpose) != nullptr)
        taken_by_other = true;
    if (found.rules.has_value())
    {
        for (const plug::policy_key& each : found.rules->allow_keys)
        {
            if (each.key_id == approved.key_id)
                taken_by_other = true;
        }
    }
    if (taken_by_other)
    {
        view.failure =
            plugin_key_file_view::problem::id_is_official_or_administrators;
        return view;
    }
    if (const plug::approved_key* existing = store.find_key(approved.key_id);
        existing != nullptr && existing->public_key != approved.public_key)
    {
        view.failure = plugin_key_file_view::problem::id_in_use;
        return view;
    }

    // Is the file inside a plugin folder?
    std::error_code ignored;
    const fs::path where = fs::weakly_canonical(path, ignored);
    for (const fs::path& folder : found.plugin_folders)
    {
        const fs::path canonical = fs::weakly_canonical(folder, ignored);
        const auto [mismatch, unused] = std::mismatch(
            canonical.begin(), canonical.end(), where.begin(), where.end());
        static_cast<void>(unused);
        if (mismatch == canonical.end())
            view.inside_plugin_folder = true;
    }

    view.failure = plugin_key_file_view::problem::none;
    if (key_out != nullptr)
        *key_out = approved;
    return view;
}

} // namespace vss::gui
