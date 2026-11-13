//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the manifest of a plugin. A manifest is written by
// someone else and is read before anything about the plugin is trusted, so
// the reader must refuse bad input with an error and never crash. What it
// accepts must be inside every limit the header promises, must read the same
// a second time, and must name only paths that stay inside the plugin. The
// input is also given to the path check as a path, because a manifest that
// reaches the path check is rare among random bytes.
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/trust_store.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace plugin = vss::plugin;

/// A property did not hold: stop, so the fuzzer keeps the input.
void require(bool holds)
{
    if (!holds)
    {
        std::abort();
    }
}

/// What `parse_trust_store` promises about a store it accepts.
void require_trust_store(const plugin::trust_store& store)
{
    require(store.keys().size() <= plugin::max_approved_keys);
    require(store.decisions().size() <= plugin::max_decisions);
    for (const plugin::approved_key& key : store.keys())
    {
        require(plugin::is_key_id(key.key_id));
        require(!key.name.empty());
        require(key.name.size() <= plugin::max_name_length);
        require(key.approved_on.ok());
        require(store.find_key(key.key_id) == &key);
    }
    std::map<std::string, std::size_t> per_plugin;
    for (const plugin::plugin_decision& decision : store.decisions())
    {
        require(plugin::is_key_id(decision.key_id) ||
                decision.key_id.empty());
        require((decision.level == plugin::trust_level::unsigned_plugin) ==
                decision.key_id.empty());
        require(decision.decided_on.ok());
        require(decision.granted.size() <= plugin::max_permissions);
        require(store.find(decision.plugin_id, decision.hash,
                           decision.key_id) == &decision);
        require(++per_plugin[decision.plugin_id] <=
                plugin::max_decisions_per_plugin);
    }
    require(store.ledger().revocation_serial <=
            plugin::max_revocations_serial);
    require(store.ledger().key_list_serial <= plugin::max_revocations_serial);
    require(!store.ledger().latest_date ||
            store.ledger().latest_date->ok());
}

/// What `check_relative_path` promises about a path it accepts.
void require_safe_path(std::string_view path)
{
    require(!path.empty());
    require(path.size() <= plugin::max_path_length);
    require(path.front() != '/');
    require(path.find('\\') == std::string_view::npos);
    require(path.find(':') == std::string_view::npos);
    require(path.find("//") == std::string_view::npos);
    require(path.back() != '/');
    std::string_view rest = path;
    while (true)
    {
        const std::size_t slash = rest.find('/');
        const std::string_view name = rest.substr(0, slash);
        require(name != "." && name != "..");
        if (slash == std::string_view::npos)
        {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    for (const char c : path)
    {
        require(c >= 0x20 && c <= 0x7e);
    }
}

void require_parameters(const std::vector<plugin::parameter>& parameters)
{
    require(parameters.size() <= plugin::max_parameters);
    for (const plugin::parameter& item : parameters)
    {
        require(!item.id.empty());
        require(!item.label.empty());
        require(item.choices.size() <= plugin::max_choices);
        require(item.max_length <= plugin::max_text_parameter_length);
    }
}

void require_inside_limits(const plugin::manifest& value)
{
    require(value.id.size() >= 3 && value.id.size() <= plugin::max_id_length);
    require(!value.name.empty());
    require(value.name.size() <= plugin::max_name_length);
    require(value.description.size() <= plugin::max_description_length);
    require(!value.run.platforms.empty());
    require(value.run.args.size() <= plugin::max_arguments);
    require_safe_path(value.run.executable);
    require(value.permissions.size() <= plugin::max_permissions);
    require(value.extension_points.size() <= plugin::max_extension_points);
    require(value.bundle_sections.size() <= plugin::max_bundle_sections);
    require(!value.extension_points.empty() ||
            !value.bundle_sections.empty());
    require(value.files.size() <= plugin::max_files);
    for (const plugin::extension_point& point : value.extension_points)
    {
        require(point.max_instances >= 1);
        require(point.max_instances <= plugin::max_instances);
        require_parameters(point.parameters);
        if (point.timing.type == plugin::timing_class::in_line)
        {
            require(point.timing.budget_us >= plugin::min_budget_us);
            require(point.timing.budget_us <= plugin::max_budget_us);
            require(value.find(plugin::permission::modifier_inline) !=
                    nullptr);
        }
        if (point.kind == plugin::extension_kind::decoder)
        {
            require(point.timing.type == plugin::timing_class::async);
            const plugin::permission_request* const tap =
                value.find(plugin::permission::traffic_tap);
            require(tap != nullptr && tap->required);
        }
    }
    for (const plugin::bundle_section& section : value.bundle_sections)
    {
        require_parameters(section.parameters);
    }
    for (const plugin::file_entry& file : value.files)
    {
        require_safe_path(file.path);
    }
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    std::string detail;
    const auto first = plugin::parse_manifest(text, &detail);
    if (first.has_value())
    {
        require(detail.empty());
        require_inside_limits(*first);
        const auto second = plugin::parse_manifest(text);
        require(second.has_value());
        require(*first == *second);
    }
    else
    {
        require(!detail.empty());
        require(first.error().category() == plugin::plugin_category());
        const auto second = plugin::parse_manifest(text);
        require(second.has_error());
        require(second.error() == first.error());
    }
    if (size > plugin::max_manifest_size)
    {
        require(first.has_error());
        require(first.error() == plugin::plugin_errc::manifest_too_large);
    }

    if (!plugin::check_relative_path(text).has_value())
    {
        require_safe_path(text);
        require(plugin::fold_path(text).size() == text.size());
    }

    // The same bytes as a policy and as a revocation list.  Both readers
    // either refuse with a reason or return something inside the limits,
    // and say the same when asked again.
    detail.clear();
    const auto rules = plugin::parse_policy(text, &detail);
    if (rules.has_value())
    {
        require(detail.empty());
        require(size <= plugin::max_policy_size);
        require(rules->allow_keys.size() <= plugin::max_policy_keys);
        require(rules->deny_plugins.size() <= plugin::max_policy_plugins);
        for (const std::string& key : rules->allow_keys)
        {
            require(plugin::is_key_id(key));
        }
        const auto again = plugin::parse_policy(text);
        require(again.has_value() && *again == *rules);
    }
    else
    {
        require(!detail.empty());
        require(plugin::parse_policy(text).has_error());
    }

    detail.clear();
    const auto revoked = plugin::parse_revocations(text, &detail);
    if (revoked.has_value())
    {
        require(detail.empty());
        require(size <= plugin::max_revocations_size);
        require(revoked->serial >= 1);
        require(revoked->serial <= plugin::max_revocations_serial);
        require(revoked->issued_at.ok());
        require(revoked->key_ids.size() <= plugin::max_revoked_entries);
        require(revoked->plugin_hashes.size() <=
                plugin::max_revoked_entries);
        require(revoked->plugins.size() <= plugin::max_revoked_entries);
        for (const plugin::revoked_plugin& entry : revoked->plugins)
        {
            require(revoked->revokes_plugin(entry.id, entry.max_version));
        }
        const auto again = plugin::parse_revocations(text);
        require(again.has_value() && *again == *revoked);
    }
    else
    {
        require(!detail.empty());
        require(plugin::parse_revocations(text).has_error());
    }

    // The same bytes as a trust store.  A store that reads is inside its
    // limits, reads the same a second time, and is written as text that
    // reads back to an equal store.
    detail.clear();
    const auto trusted = plugin::parse_trust_store(text, &detail);
    if (trusted.has_value())
    {
        require(detail.empty());
        require(size <= plugin::max_trust_store_size);
        require_trust_store(*trusted);
        const auto again = plugin::parse_trust_store(text);
        require(again.has_value() && *again == *trusted);
        const std::string written = plugin::to_json(*trusted);
        const auto reread = plugin::parse_trust_store(written);
        require(reread.has_value() && *reread == *trusted);
        require(plugin::to_json(*reread) == written);
    }
    else
    {
        require(!detail.empty());
        require(plugin::parse_trust_store(text).has_error());
    }
    return 0;
}
