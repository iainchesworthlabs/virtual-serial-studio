//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/verify.hpp>

#include <algorithm>
#include <utility>

namespace vss::plugin
{

namespace
{

result<verify_result> refuse(plugin_errc code, std::string* explain,
                             std::string text)
{
    if (explain != nullptr)
        *explain = std::move(text);
    return make_error_code(code);
}

trust_level level_of(const signature_finding& signature) noexcept
{
    if (signature.state != signature_state::valid)
        return trust_level::unsigned_plugin;
    switch (signature.origin)
    {
    case key_origin::official:
        return trust_level::official;
    case key_origin::administrator:
    case key_origin::user:
        return trust_level::third_party_signed;
    case key_origin::unknown:
        break;
    }
    return trust_level::signed_unknown_key;
}

} // namespace

std::string_view to_string(trust_level value) noexcept
{
    switch (value)
    {
    case trust_level::official:
        return "official";
    case trust_level::third_party_signed:
        return "third_party_signed";
    case trust_level::signed_unknown_key:
        return "signed_unknown_key";
    case trust_level::unsigned_plugin:
        break;
    }
    return "unsigned";
}

bool may_override_sandbox(const policy* rules, install_kind install) noexcept
{
    if (rules != nullptr)
        return rules->allow_no_sandbox;
    return install == install_kind::for_one_user;
}

result<verify_result> verify(const verify_input& input, std::string* explain)
{
    if (input.declared == nullptr)
    {
        if (explain != nullptr)
            *explain = "no manifest was given";
        return make_error_code(errc::invalid_argument);
    }
    const manifest& declared = *input.declared;
    const policy defaults;
    const policy& rules = input.rules != nullptr ? *input.rules : defaults;

    // 1. The signature.  A signature that does not hold is never read as
    //    "not signed": the files may have been changed.
    if (input.signature.state == signature_state::invalid)
    {
        return refuse(plugin_errc::signature_invalid, explain,
                      "the signature does not match the plugin");
    }
    const bool is_signed = input.signature.state == signature_state::valid;
    if (is_signed && input.signature.key_id.empty())
    {
        return refuse(plugin_errc::signature_invalid, explain,
                      "the signature names no key");
    }
    if (is_signed && declared.files.empty())
    {
        return refuse(plugin_errc::signature_invalid, explain,
                      "a signed plugin must list its files");
    }

    verify_result out;
    out.level = level_of(input.signature);
    if (is_signed)
        out.key_id = input.signature.key_id;
    const bool official = out.level == trust_level::official;

    // 2. What only an official plugin must have.
    if (official && !declared.not_after)
    {
        return refuse(plugin_errc::manifest_invalid, explain,
                      "an official plugin must have not_after");
    }

    // 3. Ids that belong to official plugins.
    if (!official && is_reserved_id(declared.id))
    {
        return refuse(plugin_errc::reserved_id, explain,
                      "only an official plugin may use this id");
    }

    // 4. Revocation.  A revoked key revokes whatever it signed, wherever
    //    the key is known from.
    if (input.revocations != nullptr)
    {
        const revocation_list& revoked = *input.revocations;
        if (is_signed && revoked.revokes_key(out.key_id))
        {
            return refuse(plugin_errc::revoked, explain,
                          "its signing key was withdrawn");
        }
        if (revoked.revokes_hash(input.hash))
        {
            return refuse(plugin_errc::revoked, explain,
                          "this copy of the plugin was withdrawn");
        }
        if (revoked.revokes_plugin(declared.id, declared.version))
        {
            return refuse(plugin_errc::revoked, explain,
                          "this version of the plugin was withdrawn");
        }
    }

    // 5. Expiry.  The plugin runs on its last day.
    if (declared.not_after &&
        std::chrono::sys_days{*declared.not_after} < input.today &&
        (official || !input.run_expired))
    {
        return refuse(plugin_errc::expired, explain,
                      "not_after has passed");
    }

    // 6. The administrator's policy.
    if (std::ranges::find(rules.deny_plugins, declared.id) !=
        rules.deny_plugins.end())
    {
        return refuse(plugin_errc::policy, explain,
                      "the policy names this plugin in deny_plugins");
    }
    if (rules.allow == allow_level::official_only && !official)
    {
        return refuse(plugin_errc::policy, explain,
                      "the policy allows only official plugins");
    }
    if (rules.allow == allow_level::signed_only)
    {
        if (out.level == trust_level::unsigned_plugin)
        {
            return refuse(plugin_errc::signature_missing, explain,
                          "the policy allows only signed plugins");
        }
        if (out.level == trust_level::signed_unknown_key)
        {
            return refuse(plugin_errc::key_unknown, explain,
                          "the policy allows only approved signing keys");
        }
    }
    for (const permission_request& request : declared.permissions)
    {
        if (std::ranges::find(rules.permissions_denied, request.name) ==
            rules.permissions_denied.end())
            continue;
        if (request.required)
        {
            std::string text = "the policy denies ";
            text += to_string(request.name);
            text += ", which the plugin needs";
            return refuse(plugin_errc::policy, explain, std::move(text));
        }
        out.denied_permissions.push_back(request.name);
    }
    out.forced_on_timeout = rules.on_timeout;

    // 7. What the plugin needs from this host.
    if (!declared.needs.api.contains(input.host_api))
    {
        return refuse(plugin_errc::api_incompatible, explain,
                      "the plugin does not work with this plugin API");
    }
    for (const std::string& name : declared.needs.capabilities)
    {
        if (std::ranges::find(input.capabilities, name) ==
            input.capabilities.end())
        {
            return refuse(plugin_errc::capability_missing, explain,
                          "this build does not have " + name);
        }
    }

    // 8. Isolation.  An official plugin runs at any level, since its trust
    //    is its signature; the level is still reported.
    if (input.sandbox < rules.min_sandbox)
    {
        const bool overridden =
            input.sandbox_override &&
            may_override_sandbox(input.rules, input.install);
        if (!official && !overridden)
        {
            std::string text = "this computer gives isolation ";
            text += to_string(input.sandbox);
            text += " and the minimum is ";
            text += to_string(rules.min_sandbox);
            return refuse(plugin_errc::sandbox_insufficient, explain,
                          std::move(text));
        }
        out.below_min_sandbox = true;
    }

    if (explain != nullptr)
        explain->clear();
    return out;
}

} // namespace vss::plugin
