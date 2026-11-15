//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the plugin library's findings become what the Plugins page shows: the
// plugin's discovery, its signature, `verify()`, the licence check and the
// user's stored decision are put through one function that gives each plugin
// its state group, its reason and the sentence that goes with it. Nothing here
// starts a plugin, and nothing here touches the GUI: it runs on the worker
// thread of `LibraryPluginBackend` and in tests.
//

#pragma once

#include "backend/plugin_view.hpp"

#include <vss/license/license.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vss::gui
{

struct plugin_gather_options
{
    /// The plugin folders, widest scope first, as `plugin_locations_for`
    /// gives them.
    std::vector<vss::plugin::plugin_root> roots;
    /// The folder of Studio's programs, for who controls the install. Not
    /// used when `install` is set.
    std::filesystem::path program_directory;
    std::optional<vss::plugin::install_kind> install;
    /// The administrator's policy file. Empty: there is none.
    std::filesystem::path policy_file;
    /// The user's trust decisions and approved keys (`trust.json`).
    std::filesystem::path trust_store_file;
    /// The signed official key list. Empty or missing: no key is official.
    std::filesystem::path official_keys_file;
    /// The signed revocation list. Empty or missing: nothing is revoked.
    std::filesystem::path revocations_file;
    /// The keys that may sign the official key list. Empty: the ones built
    /// into Studio. Only a test sets it.
    std::vector<vss::plugin::root_key> root_keys;

    /// How far this computer can keep a plugin apart. This build has no
    /// sandbox yet, so it is `none` unless a test says otherwise.
    vss::plugin::sandbox_level sandbox = vss::plugin::sandbox_level::none;
    /// What is missing, for "Partial: <what is missing>".
    std::string isolation_missing;
    /// This build can start a plugin. It cannot yet: every plugin that may
    /// run is then Off.
    bool can_run_plugins = false;
    /// The engine capabilities of this build, by name.
    std::vector<std::string> capabilities;
    vss::plugin::api_version host_api{1, 0};
    /// This version of Studio, for "This is version <version>".
    std::string studio_version;
    /// The day to judge expiry by. Empty: the system's date.
    std::optional<std::chrono::sys_days> today;
    vss::license::status licence = vss::license::no_licence();
    /// `<id>|<hash>` of the expired plugins the user chose to run until
    /// Studio closes.
    std::set<std::string> run_expired;
    /// A folder as the user's own plugin folder, for "Add a plugin...".
    std::string user_folder_text;
    std::string machine_folder_text;
};

/// What the worker keeps of one plugin besides what it shows.
struct plugin_record
{
    QString id;
    QString hash;
    QString folder;
    vss::plugin::plugin_hash plugin_hash;
    vss::plugin::trust_level level = vss::plugin::trust_level::unsigned_plugin;
    std::string key_id;
    vss::plugin::plugin_scope scope = vss::plugin::plugin_scope::for_user;
    /// The permissions the plugin declares, by id, and whether each is needed.
    std::vector<std::pair<std::string, bool>> declared;
    std::string name;
};

struct plugin_gathered
{
    plugin_snapshot snapshot;
    std::vector<plugin_record> records;
    /// The folders found, for "the file lies inside a plugin folder".
    std::vector<std::filesystem::path> plugin_folders;
    /// The roots that were searched.
    std::vector<std::filesystem::path> root_folders;
    std::optional<vss::plugin::policy> rules;
    std::optional<vss::plugin::key_list> official;
    std::filesystem::path user_root;
    std::filesystem::path trust_store_file;
    bool trust_store_readable = true;
};

/// Looks at every installed plugin and builds the snapshot. Reads files; call
/// it only from a worker thread. `store` is the user's decisions; it is not
/// changed.
plugin_gathered gather_plugins(const plugin_gather_options& options,
                               const vss::plugin::trust_store& store);

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
plugin_key_file_view read_key_file_view(const std::filesystem::path& path,
                                        const plugin_gathered& found,
                                        const vss::plugin::trust_store& store,
                                        vss::plugin::approved_key* key_out);

/// The hash text of a plugin, 128 hex characters.
QString hash_text(const vss::plugin::plugin_hash& hash);
std::optional<vss::plugin::plugin_hash> parse_hash_text(const QString& text);

} // namespace vss::gui
