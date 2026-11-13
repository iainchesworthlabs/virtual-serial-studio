//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decision whether a plugin may run on this computer, once its manifest
// was read, its files were checked and its signature was looked at.
// docs/plugins.md, section 5.3 (steps 4 and 6 to 8) and sections 8 and 9.2,
// are the design.
//
// `verify` is a function of its input and nothing else: no file, no clock,
// no stored state.  The same input gives the same answer, so every rule can
// be tested from memory.  Two things are left to other code on purpose: the
// user's own decision about a plugin (the trust store), and the licence.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// Who stands behind a plugin.  A level says who signed it and that its
/// files are unchanged.  It never says that the plugin is safe.
enum class trust_level
{
    /// Signed with a key the official key list names for plugins.
    official,
    /// Signed with a key the administrator or the user approved.
    third_party_signed,
    /// Signed with a key nobody approved.  Treated as unsigned, with the
    /// key shown.
    signed_unknown_key,
    /// Not signed.
    unsigned_plugin
};

std::string_view to_string(trust_level value) noexcept;

/// What the check of `plugin.sig` found.
enum class signature_state
{
    /// There is no `plugin.sig`.
    absent,
    /// There is one and it does not hold: it is damaged, was made for other
    /// manifest bytes or another plugin, has another purpose, or does not
    /// verify with the key it names.
    invalid,
    /// It verifies with the key it names, for this manifest.
    valid
};

/// Where the key of a valid signature is known from.
enum class key_origin
{
    /// Nowhere: only the signature itself names it.
    unknown,
    /// The official key list, with the purpose `plugin`.
    official,
    /// `allow_keys` of the administrator's policy.
    administrator,
    /// The user's own approved keys.
    user
};

struct signature_finding
{
    signature_state state = signature_state::absent;
    /// The id of the signing key.  Empty unless `state` is `valid`.
    std::string key_id;
    key_origin origin = key_origin::unknown;
};

/// Whether an administrator or the person using it controls the files of
/// this copy of Studio.  docs/plugins.md, section 5.2.
enum class install_kind { for_everyone, for_one_user };

struct verify_input
{
    /// The manifest as read, after `check_files` accepted the directory.
    const manifest* declared = nullptr;
    plugin_hash hash;
    signature_finding signature;
    /// The newest list whose signature was checked, or null.
    const revocation_list* revocations = nullptr;
    /// The administrator's policy, or null when there is no policy file.
    /// For a file that is there and cannot be used, `official_only_policy`.
    const policy* rules = nullptr;
    /// The later of the system's date and the latest date ever seen, so
    /// that setting the clock back revives nothing.
    std::chrono::sys_days today;
    /// The plugin API this host speaks.
    api_version host_api;
    /// The engine capabilities of this build, by name.
    std::span<const std::string> capabilities;
    /// How far this computer can keep this plugin apart.
    sandbox_level sandbox = sandbox_level::none;
    /// The user's stored choice to run this plugin, with this hash, below
    /// the minimum.  It counts only where `may_override_sandbox` holds.
    bool sandbox_override = false;
    install_kind install = install_kind::for_everyone;
    /// The user's choice, for this run of Studio, to run this plugin after
    /// its `not_after`.  It never counts for an official plugin.
    bool run_expired = false;
};

struct verify_result
{
    trust_level level = trust_level::unsigned_plugin;
    /// The signing key, when there is a valid signature.
    std::string key_id;
    /// Optional permissions the plugin asks for and the policy denies.  The
    /// plugin runs without them.
    std::vector<permission> denied_permissions;
    /// What the policy forces an in-line plugin's bytes to do on a timeout.
    std::optional<timeout_action> forced_on_timeout;
    /// The plugin runs although this computer cannot keep it apart as far
    /// as the minimum asks: it is official, or the user overrode it.
    bool below_min_sandbox = false;

    friend bool operator==(const verify_result&,
                           const verify_result&) = default;
};

/// Decides whether the plugin may run.  The checks are made in this order
/// and the first that fails is the answer:
///
/// 1. `signature_invalid`: the signature does not hold, or a signed plugin
///    does not list its files.
/// 2. `manifest_invalid`: an official plugin has no `not_after`.
/// 3. `reserved_id`: the id belongs to official plugins and this is not one.
/// 4. `revoked`: the signing key, the hash, or the id with this version is
///    on the revocation list.
/// 5. `expired`: `not_after` is before `today`, and the user did not choose
///    to run it anyway (which an official plugin never offers).
/// 6. `policy`: the plugin is in `deny_plugins`, is not official under
///    `official_only`, or needs a permission the policy denies.  Under
///    `signed`: `signature_missing` for an unsigned plugin and `key_unknown`
///    for a key nobody approved.
/// 7. `api_incompatible`, then `capability_missing`.
/// 8. `sandbox_insufficient`: the plugin is not official, this computer
///    cannot keep it apart as far as the minimum asks, and there is no
///    override that counts.
///
/// `errc::invalid_argument` when `declared` is null.  `detail`, when given,
/// receives one line for a person.
result<verify_result> verify(const verify_input& input,
                             std::string* detail = nullptr);

/// Whether a user may let a plugin run below the minimum on this computer.
/// With a policy the policy says.  With none, only on a copy of Studio that
/// the user controls anyway.
bool may_override_sandbox(const policy* rules, install_kind install) noexcept;

} // namespace vss::plugin
