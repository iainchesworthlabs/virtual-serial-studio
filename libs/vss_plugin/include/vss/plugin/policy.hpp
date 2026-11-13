//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The administrator's policy for plugins, `policy.json`.  docs/plugins.md,
// section 8.6, is the design.
//
// A policy narrows what a user may do with plugins on a computer someone
// else looks after.  Nothing here reads a file or checks who owns it: the
// caller does both and hands over the text.  A policy file that is there and
// cannot be used must not be taken as "no policy"; `official_only_policy` is
// what the caller uses then.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The policy format this version reads.
inline constexpr std::uint32_t policy_format_version = 1;

inline constexpr std::size_t max_policy_size = std::size_t{64} * 1024;
inline constexpr std::size_t max_policy_depth = 4;
inline constexpr std::size_t max_policy_keys = 256;
inline constexpr std::size_t max_policy_plugins = 1024;
inline constexpr std::size_t max_key_id_length = 64;

/// Which plugins may run at all.
enum class allow_level
{
    /// Only plugins signed by Virtual Serial Studio.
    official_only,
    /// Also plugins signed with a key the administrator or the user approved.
    signed_only,
    /// Also unsigned plugins, after the user's own decision.
    any
};

/// How far a plugin is kept apart from the user's files, the network and
/// other programs.  The values are in order: a later one is stronger.
enum class sandbox_level { none, partial, full };

std::string_view to_string(allow_level value) noexcept;
std::string_view to_string(sandbox_level value) noexcept;

struct policy
{
    allow_level allow = allow_level::any;
    /// Third-party signing keys the administrator approves for everyone.
    std::vector<std::string> allow_keys;
    /// Plugins that never run, by id.
    std::vector<std::string> deny_plugins;
    /// A plugin that is not official runs only when this computer can keep
    /// it apart at least this far.
    sandbox_level min_sandbox = sandbox_level::partial;
    /// Whether a user may let one plugin run below `min_sandbox`.
    bool allow_no_sandbox = false;
    /// Whether the per-user plugin directory is searched.
    bool user_plugins = true;
    /// Forces what an in-line plugin's bytes do when it does not answer.
    std::optional<timeout_action> on_timeout;
    /// Permissions no plugin gets, whatever the user decides.
    std::vector<permission> permissions_denied;

    friend bool operator==(const policy&, const policy&) = default;
};

/// Reads the text of a `policy.json`.
///
/// `errc::config_error` for anything that is not a policy this version
/// reads: more than `max_policy_size` bytes, text that is not JSON, a key
/// the format does not have or one given twice, a wrong type, an unknown
/// word, a `policy_version` other than `policy_format_version`, or a list
/// over its limit.  A key the file leaves out has the value a default
/// `policy` has.  `detail`, when given, receives one line for a person.
///
/// The caller treats an error as `official_only_policy()`, never as no
/// policy.
result<policy> parse_policy(std::string_view text,
                            std::string* detail = nullptr);

/// The policy used when a policy file is there and cannot be trusted or
/// read: only official plugins, no per-user plugins, the strongest sandbox
/// and no override.
policy official_only_policy();

/// Whether `text` may be the id of a signing key: 1 to `max_key_id_length`
/// ASCII letters, digits, dots, hyphens and underscores.
bool is_key_id(std::string_view text) noexcept;

} // namespace vss::plugin
