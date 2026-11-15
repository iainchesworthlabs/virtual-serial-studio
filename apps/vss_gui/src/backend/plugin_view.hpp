//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Plugins page is told (docs/gui-design/docs/screens/10-plugins.md,
// "Where the plugin state comes from"). A screen never reads a plugin's
// files, checks a signature or decides a state: it shows these values. Text
// that came from a plugin is kept exactly as it was given; the screen removes
// control and direction-override characters before it shows any of it.
//

#pragma once

#include <vss/license/license.hpp>

#include <QDate>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace vss::gui
{

/// The state groups of the table in the spec. Every plugin is in exactly one.
/// `docs/plugins.md` section 10 names ten; the screen spec splits two of them
/// further (Needs more isolation, Not used), and the screen spec is the copy
/// to build from.
enum class plugin_state
{
    available,
    starting,
    off,
    needs_decision,
    blocked,
    needs_isolation,
    not_used,
    needs_licence,
    stopped,
    needs_newer_studio,
    cannot_be_used,
    not_installed
};

/// Who stands behind a plugin. Only `official` is signed by Virtual Serial
/// Studio. The level comes from the host, never from anything a plugin says.
enum class plugin_trust
{
    official,
    /// Signed with a key the user or an administrator approved.
    third_party,
    /// Signed with a key nobody approved on this computer.
    unknown_key,
    unsigned_plugin
};

enum class plugin_installed_for
{
    with_studio,
    everyone,
    you_only
};

/// The sentence under "Why and what to do" for the state, where the state has
/// more than one (the notes table of the spec).
enum class plugin_note
{
    none,
    starting,
    backing_off,
    decision_first,
    decision_changed,
    blocked_official_only,
    blocked_named,
    blocked_protection,
    isolation_no_override,
    isolation_override,
    not_used_same,
    not_used_different_signer,
    not_used_reserved,
    needs_licence,
    stopped_failures,
    stopped_unresponsive,
    stopped_limit,
    newer_studio_version,
    newer_studio_capability,
    older_plugin,
    invalid_manifest,
    invalid_signature,
    withdrawn,
    expired,
    not_installed
};

enum class plugin_isolation
{
    full,
    partial,
    none
};

enum class plugin_timing
{
    by_studio,
    in_path,
    beside
};

enum class plugin_addition_kind
{
    fixture,
    generator,
    link_behaviour,
    connection
};

struct plugin_addition
{
    plugin_addition_kind kind = plugin_addition_kind::fixture;
    QString type_name;
    /// The type's id in the plugin's description, which a setup records.
    QString type_id;
    /// The type's own description, for a toolbox tile.
    QString description;
};

/// One permission a plugin declares, by its name in the manifest
/// (`ports.read`, `net.listen`, ...).
struct plugin_permission_view
{
    QString id;
    bool required = true;
    /// The plugin's own words on why it asks. Plain text; shown quoted.
    QString reason;
    /// The addresses of `net.listen` or the hosts of `net.outbound`.
    QStringList scope;
    /// The user (or an administrator) turned it down.
    bool turned_down = false;
    /// The permission was not in the version the user approved.
    bool is_new = false;
};

struct plugin_log_line
{
    QDateTime time;
    /// As the plugin wrote it. Plain text.
    QString text;
};

struct plugin_use
{
    /// "<type name> on <port or link name>".
    QString text;
    /// Selects it on the canvas.
    QString target_id;
};

struct plugin_view
{
    // -- identity, as the plugin says it. Plain text, never markup.
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString folder;

    // -- what the host found
    plugin_installed_for installed_for = plugin_installed_for::you_only;
    plugin_trust trust = plugin_trust::unsigned_plugin;
    /// The id of the signing key, or empty.
    QString key_id;
    /// The name the user saw when they approved the key, or empty.
    QString key_name;
    QDate key_approved_on;
    /// The first 16 characters of the plugin's hash.
    QString fingerprint;
    /// The whole hash, as the decision is tied to it.
    QString hash;

    plugin_state state = plugin_state::off;
    plugin_note note = plugin_note::none;
    /// The reason code of docs/plugins.md section 12, or empty.
    QString reason_code;
    /// The host's own line, for "Copy details" only.
    QString detail;
    /// What the running plugin last said of itself. Plain text, untrusted.
    QString status_text;

    /// The user switched it on. In a build that cannot run plugins the state
    /// stays Off and this is only the wish.
    bool wished_on = false;

    // -- numbers and names the notes use
    int back_off_seconds = 0;
    int attempt = 0;
    int failure_count = 0;
    QString limit_name;
    QString capability_name;
    QString studio_version;
    QDate expires;
    /// For Needs a licence.
    vss::license::reason licence_reason = vss::license::reason::ok;
    vss::license::status licence_status = vss::license::no_licence();
    /// For Not used: the copy that is used, as a row of this list.
    QString used_copy_folder;
    plugin_trust used_copy_trust = plugin_trust::unsigned_plugin;
    QString used_copy_author;
    QString used_copy_id;
    bool newer_than_used_copy = false;
    QString this_version_text;
    /// An expired plugin that is not official can be run until Studio closes.
    bool can_run_expired = false;

    // -- isolation
    plugin_isolation isolation = plugin_isolation::none;
    /// What is missing, for "Partial: <what is missing>".
    QString isolation_missing;
    /// The user may let it run without isolation (docs/plugins.md 9.2).
    bool override_allowed = false;
    bool override_active = false;
    QDate override_on;

    plugin_timing timing = plugin_timing::by_studio;
    int budget_us = 0;
    /// The program of this computer it runs with, or empty.
    QString runtime_name;

    std::vector<plugin_addition> adds;
    std::vector<plugin_permission_view> permissions;
    /// This version is not the one the user approved.
    bool differs_from_approved = false;
    std::vector<plugin_use> used_in;
    std::vector<plugin_log_line> log;
};

/// The note at the top of the dialog, one at a time, in this priority.
enum class plugin_banner
{
    none,
    policy_untrusted,
    official_only,
    user_plugins_off,
    no_licence_for_official
};

/// A plugin that reads the data of ports or links now.
struct plugin_reader
{
    QString plugin_name;
    /// It records the data, not only shows it.
    bool recording = false;
    int port_count = 0;
};

struct plugin_snapshot
{
    std::vector<plugin_view> plugins;
    /// Plugins that read or record data now. A plugin never reads data
    /// without being here.
    std::vector<plugin_reader> readers;
    plugin_banner banner = plugin_banner::none;
    /// The folder an administrator puts a plugin in, for "Add a plugin...".
    QString machine_folder;
    QString user_folder;
    /// An administrator turned per-user plugins off.
    bool user_plugins_allowed = true;
    /// This build can start a plugin. False until a sandbox exists: every
    /// plugin that would be allowed is then shown as Off.
    bool can_run_plugins = false;
    /// The plugin folders have not been read yet.
    bool looking = false;
    /// One line of the host for "Copy details" of a policy that could not be
    /// trusted.
    QString policy_detail;
};

using plugin_snapshot_ptr = std::shared_ptr<const plugin_snapshot>;

/// A public key file the user chose, as the backend read it.
struct plugin_key_file_view
{
    enum class problem
    {
        none,
        not_a_key_file,
        id_in_use,
        id_is_official_or_administrators,
        text_not_plain
    };
    problem failure = problem::none;
    QString path;
    QString author;
    QString key_id;
    /// Sixteen hex characters.
    QString fingerprint;
    /// The file lies inside a plugin folder.
    bool inside_plugin_folder = false;
};

/// What the host found in a plugin folder the user chose, before anything is
/// copied. `plugin` holds the verdict as the list would show it once the
/// plugin was installed for this user.
struct plugin_inspection_view
{
    enum class problem
    {
        none,
        /// The folder has no readable plugin.json.
        not_a_plugin,
        /// A plugin with this id is installed for the user already.
        already_installed,
        /// The user's own plugin folder is off, or cannot be written.
        cannot_write
    };
    problem failure = problem::not_a_plugin;
    /// The folder the user chose.
    QString folder;
    plugin_view plugin;
    /// The plugin can be added: nothing is wrong that adding would not fix.
    bool can_install = false;
};

enum class plugin_request_kind
{
    decide,
    set_enabled,
    approve_key,
    remove_key,
    set_no_isolation,
    rescan,
    add_plugin,
    remove_plugin,
    start_again,
    run_expired
};

enum class plugin_request_error
{
    none,
    /// "That folder is not a plugin..."
    not_a_plugin,
    /// "<plugin name> is already installed for you..."
    already_installed,
    /// The per-user folders are off, or the copy failed.
    cannot_write,
    /// The key file's fingerprint is not the one the user confirmed.
    fingerprint_changed
};

struct plugin_request_result
{
    plugin_request_kind kind = plugin_request_kind::rescan;
    plugin_request_error error = plugin_request_error::none;
    QString plugin_id;
    /// The name of the plugin the result is about, where there is one.
    QString plugin_name;
    /// `rescan`: how many plugins were found. `add_plugin`: unused.
    int count = 0;
    /// `approve_key`: the author's name.
    QString author;
};

/// A decision about one plugin version, for its hash only.
struct plugin_decision_request
{
    QString plugin_id;
    QString hash;
    QString key_id;
    /// The permissions the user left on, by id. The needed ones are always
    /// in it.
    QStringList granted;
    bool switch_on = false;
};

} // namespace vss::gui

Q_DECLARE_METATYPE(vss::gui::plugin_request_result)
Q_DECLARE_METATYPE(vss::gui::plugin_key_file_view)
Q_DECLARE_METATYPE(vss::gui::plugin_inspection_view)
