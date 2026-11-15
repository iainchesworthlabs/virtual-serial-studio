//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// From what is known of an installed plugin to how it is started, or to
// the reason it is not.  docs/plugins.md, sections 5.1, 5.4, 8.4, 8.7, 9.1
// and 9.2, are the design.
//
// This is the one place that says whether a plugin runs.  It does no I/O:
// it is given the result of `inspect_plugin`, the user's stored decision
// for this exact copy of the plugin, and a few facts of the computer, and
// it answers with the options of a `plugin_runner` or with a reason code.
// So every rule can be tested from memory.
//
// The rules, in the order they are checked.  The first that fails is the
// answer.
//
//  1. The plugin could be read and its checks passed (`inspect_plugin`:
//     manifest, layout, files, signature, revocation, expiry, policy, the
//     plugin API, capabilities, the sandbox minimum).
//  2. An official plugin that needs a licence is covered by one:
//     `not_licensed` otherwise.
//  3. Somebody trusts it.  An official plugin is trusted by the owner's
//     signature.  Any other plugin needs the user's decision for this
//     exact copy (its hash and signing key): `key_unknown` for a signed
//     plugin with none, `signature_missing` for an unsigned one.  Both
//     mean "needs your decision".
//  4. It is switched on.  An official plugin is on unless the user's
//     decision says off; any other is on only when the decision says so.
//     A plugin that is off is not an error: the answer has `enabled`
//     false, and nothing is started.
//  5. Every permission the manifest marks as required is granted and not
//     denied by the administrator: `permission_denied` otherwise.  An
//     official plugin is granted what it asks for; any other is granted
//     what the decision lists.
//  6. Its program is one of its files.
//
// The sandbox: the profile is made from the permissions that were
// granted, never from what was asked.  The least level the start will
// take is the administrator's minimum, except where the checks of rule 1
// already said this plugin may run below it: an official plugin, or one
// the user let run without isolation where that is allowed.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/trust_store.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace vss::plugin::host
{

/// The facts of this computer and this run of Studio that a start needs.
struct plan_context
{
    /// Where plugins keep their data: `<root>/<plugin id>/data` is the
    /// data directory of a plugin.
    std::filesystem::path data_root;
    /// The engine capabilities this build has, as "hello_ack" names them.
    std::vector<std::string> capabilities;
    /// The least sandbox level a plugin must reach unless it may run
    /// below it: the administrator's, or the default.
    sandbox_level min_sandbox = sandbox_level::partial;
    /// Chooses the random part of the waits between starts.
    std::uint64_t seed = 0;
};

/// How a plugin is started, when it may be.
struct start_plan
{
    /// False when the plugin may run and is switched off.  Then `options`
    /// is not filled in.
    bool enabled = false;
    /// The permissions the plugin gets.
    std::vector<permission> granted;
    /// Permissions it asked for as optional and does not get.
    std::vector<permission> withheld;
    /// What to give a `plugin_runner`.
    runner_options options;
};

/// The data directory of a plugin under `data_root`.
std::filesystem::path data_directory_of(const std::filesystem::path& data_root,
                                        std::string_view plugin_id);

/// Decides.  `decision` is the user's stored decision for this copy of
/// the plugin (`trust_store::find` with its hash and key id), or null.
/// `detail`, when given, receives one line for a person about a refusal.
result<start_plan> plan_start(const inspection& found,
                              const plugin_decision* decision,
                              const plan_context& context,
                              std::string* detail = nullptr);

} // namespace vss::plugin::host
