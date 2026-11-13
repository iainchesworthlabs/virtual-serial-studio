//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>

#include <string_view>
#include <type_traits>

namespace vss::plugin
{

/// Why a plugin is not running, or why one of its files was refused.  These
/// are the reason codes of docs/plugins.md, section 12.  A value never changes
/// its number once a release has carried it: new ones are added at the end.
enum class plugin_errc
{
    success = 0,
    /// `plugin.json` is not a manifest this version can read: bad JSON, an
    /// unknown or repeated key, a wrong type or a value outside its limits.
    manifest_invalid,
    /// `plugin.json` is larger than `max_manifest_size`.
    manifest_too_large,
    /// `manifest_version` is higher than this version reads.
    manifest_unsupported_version,
    /// More plugins are installed than the host takes.
    too_many,
    /// Another plugin with the same id was found first.
    shadowed,
    /// A path in the manifest, or the plugin's directory, breaks the layout
    /// rules: a link, an absolute path, a reserved name, a case clash.
    layout_invalid,
    /// A file the manifest lists is not there.
    file_missing,
    /// The directory of a signed plugin holds a file the manifest does not
    /// list.
    unlisted_file,
    /// A signature was required and there is none.
    signature_missing,
    /// The signature does not match the manifest, or a file does not match
    /// its hash in the manifest.
    signature_invalid,
    /// The signature was made with a key that is neither official nor
    /// approved by the user.
    key_unknown,
    /// The files are not the ones the user approved.
    changed,
    /// The plugin, its version or its signing key is on the revocation list.
    revoked,
    /// The manifest's `not_after` date has passed.
    expired,
    /// The administrator's policy does not allow this plugin.
    policy,
    /// The licence does not cover this plugin now.
    not_licensed,
    /// The plugin needs a plugin API this host does not speak.
    api_incompatible,
    /// The plugin needs an engine capability this build does not have.
    capability_missing,
    /// This computer cannot isolate the plugin as far as the policy requires.
    sandbox_insufficient,
    /// The plugin asked for something it was not granted.
    permission_denied,
    /// The process started and did not say `hello` in time.
    no_hello,
    /// The process said a `hello` that does not match its manifest.
    bad_hello,
    /// The process stopped answering.
    unresponsive,
    /// The process broke a limit of the plugin API and kept doing so.
    misbehaving,
    /// The process ended on a signal or an exception.
    crashed,
    /// The process ended by itself without being asked to.
    exited,
    /// The process could not be started.
    start_failed,
    /// The plugin failed too often and is not started again until the user
    /// asks.
    quarantined
};

/// The category of `plugin_errc`, named `vss.plugin`.
const boost::system::error_category& plugin_category() noexcept;
boost::system::error_code make_error_code(plugin_errc value) noexcept;

/// The code as it is written in logs, in `vss_cli plugin list --json` and in
/// "Copy details": the enumerator's own name, such as `manifest_invalid`.
std::string_view to_string(plugin_errc value) noexcept;

} // namespace vss::plugin

namespace boost::system
{
template <>
struct is_error_code_enum<vss::plugin::plugin_errc> : std::true_type
{
};
} // namespace boost::system
