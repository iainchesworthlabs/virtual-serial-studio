//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pieces the plugin backend shares: what a reason code of the plugin library
// leads to on the page, the checks on a public key file, and the text of a
// plugin's hash.
//

#pragma once

#include "backend/plugin_view.hpp"

#include <vss/plugin/hash.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vss::gui
{

/// The state a reason code leads to when nothing else is known, and the note
/// for it. The table of docs/gui-design/docs/screens/10-plugins.md, in code.
struct plugin_reason_state
{
    plugin_state state = plugin_state::cannot_be_used;
    plugin_note note = plugin_note::invalid_manifest;
};
plugin_reason_state state_for_reason(vss::plugin::plugin_errc code,
                                     bool override_possible);

/// What a key file is checked against: the keys that are official or an
/// administrator's, and the plugin folders it must not lie in.
struct key_check_context
{
    std::optional<vss::plugin::key_list> official;
    std::optional<vss::plugin::policy> rules;
    std::vector<std::filesystem::path> plugin_folders;
};

/// The public key file's fields, read and checked. `failure` says why not.
plugin_key_file_view read_key_file_view(const std::filesystem::path& path,
                                        const key_check_context& context,
                                        const vss::plugin::trust_store& store,
                                        vss::plugin::approved_key* key_out);

/// The hash text of a plugin, 128 hex characters.
QString hash_text(const vss::plugin::plugin_hash& hash);
std::optional<vss::plugin::plugin_hash> parse_hash_text(const QString& text);

} // namespace vss::gui
