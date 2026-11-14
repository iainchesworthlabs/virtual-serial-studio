//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/error.hpp>

#include <string>
#include <utility>

namespace vss::plugin
{
namespace
{

class plugin_category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.plugin"; }

    std::string message(int value) const override
    {
        switch (static_cast<plugin_errc>(value))
        {
        case plugin_errc::success:
            return "success";
        case plugin_errc::manifest_invalid:
            return "the plugin's manifest is not valid";
        case plugin_errc::manifest_too_large:
            return "the plugin's manifest is too large";
        case plugin_errc::manifest_unsupported_version:
            return "the plugin's manifest needs a newer version";
        case plugin_errc::too_many:
            return "too many plugins are installed";
        case plugin_errc::shadowed:
            return "another plugin with the same id was found first";
        case plugin_errc::layout_invalid:
            return "the plugin's files break the layout rules";
        case plugin_errc::file_missing:
            return "a file of the plugin is missing";
        case plugin_errc::unlisted_file:
            return "the plugin has a file its manifest does not list";
        case plugin_errc::signature_missing:
            return "the plugin is not signed";
        case plugin_errc::signature_invalid:
            return "the plugin's signature does not match its files";
        case plugin_errc::key_unknown:
            return "the plugin is signed with a key that is not approved";
        case plugin_errc::changed:
            return "the plugin changed since it was approved";
        case plugin_errc::revoked:
            return "the plugin was withdrawn";
        case plugin_errc::expired:
            return "the plugin has expired";
        case plugin_errc::policy:
            return "the administrator's policy does not allow the plugin";
        case plugin_errc::not_licensed:
            return "the licence does not cover the plugin";
        case plugin_errc::api_incompatible:
            return "the plugin needs another version of the plugin API";
        case plugin_errc::capability_missing:
            return "the plugin needs a capability this build does not have";
        case plugin_errc::sandbox_insufficient:
            return "this computer cannot isolate the plugin as required";
        case plugin_errc::permission_denied:
            return "the plugin was not granted that permission";
        case plugin_errc::no_hello:
            return "the plugin did not start talking in time";
        case plugin_errc::bad_hello:
            return "the plugin is not the one its manifest describes";
        case plugin_errc::unresponsive:
            return "the plugin stopped answering";
        case plugin_errc::misbehaving:
            return "the plugin broke a limit of the plugin API";
        case plugin_errc::crashed:
            return "the plugin crashed";
        case plugin_errc::exited:
            return "the plugin ended unexpectedly";
        case plugin_errc::start_failed:
            return "the plugin could not be started";
        case plugin_errc::quarantined:
            return "the plugin failed too often and was stopped";
        case plugin_errc::reserved_id:
            return "the plugin's id is kept for official plugins";
        }
        return "unknown plugin error";
    }
};

} // namespace

const boost::system::error_category& plugin_category() noexcept
{
    static const plugin_category_impl instance;
    return instance;
}

boost::system::error_code make_error_code(plugin_errc value) noexcept
{
    return {std::to_underlying(value), plugin_category()};
}

std::string_view to_string(plugin_errc value) noexcept
{
    switch (value)
    {
    case plugin_errc::success:
        return "success";
    case plugin_errc::manifest_invalid:
        return "manifest_invalid";
    case plugin_errc::manifest_too_large:
        return "manifest_too_large";
    case plugin_errc::manifest_unsupported_version:
        return "manifest_unsupported_version";
    case plugin_errc::too_many:
        return "too_many";
    case plugin_errc::shadowed:
        return "shadowed";
    case plugin_errc::layout_invalid:
        return "layout_invalid";
    case plugin_errc::file_missing:
        return "file_missing";
    case plugin_errc::unlisted_file:
        return "unlisted_file";
    case plugin_errc::signature_missing:
        return "signature_missing";
    case plugin_errc::signature_invalid:
        return "signature_invalid";
    case plugin_errc::key_unknown:
        return "key_unknown";
    case plugin_errc::changed:
        return "changed";
    case plugin_errc::revoked:
        return "revoked";
    case plugin_errc::expired:
        return "expired";
    case plugin_errc::policy:
        return "policy";
    case plugin_errc::not_licensed:
        return "not_licensed";
    case plugin_errc::api_incompatible:
        return "api_incompatible";
    case plugin_errc::capability_missing:
        return "capability_missing";
    case plugin_errc::sandbox_insufficient:
        return "sandbox_insufficient";
    case plugin_errc::permission_denied:
        return "permission_denied";
    case plugin_errc::no_hello:
        return "no_hello";
    case plugin_errc::bad_hello:
        return "bad_hello";
    case plugin_errc::unresponsive:
        return "unresponsive";
    case plugin_errc::misbehaving:
        return "misbehaving";
    case plugin_errc::crashed:
        return "crashed";
    case plugin_errc::exited:
        return "exited";
    case plugin_errc::start_failed:
        return "start_failed";
    case plugin_errc::quarantined:
        return "quarantined";
    case plugin_errc::reserved_id:
        return "reserved_id";
    }
    return "unknown";
}

} // namespace vss::plugin
