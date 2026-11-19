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

/// A setting Studio asks for when it makes an instance: it has no default,
/// and it is text, so one field takes it.
struct plugin_parameter_ask
{
    QString id;
    /// The plugin's label and help. Untrusted.
    QString label;
    QString help;
    /// It names a port or a link of the setup (`port`, `place`, `places`).
    bool place_name = false;
};

struct plugin_addition
{
    plugin_addition_kind kind = plugin_addition_kind::fixture;
    QString type_name;
    /// The type's id in the plugin's description, which a setup records.
    QString type_id;
    /// The type's own description, for a toolbox tile.
    QString description;
    /// How many instances of it the plugin allows.
    int max_instances = 1;
    /// Settings with no default that Studio asks for in a small dialog.
    std::vector<plugin_parameter_ask> asks{};
    /// It has another setting with no default, which Studio cannot ask for
    /// yet, so Studio cannot make an instance of it.
    bool needs_settings = false;
    /// The point is a decoder: it reads the data of a port or a link and
    /// reports what it found. It is a link behaviour in `kind`, as the
    /// plugin's manifest has no separate word for it in the page.
    bool decoder = false;
};

/// What the plugin's instances are doing, in the words of the page.
enum class plugin_instance_state
{
    /// The plugin is not running: the instance is kept for when it does.
    not_running,
    waiting,
    starting,
    running,
    stopping,
    refused
};

/// One instance Studio asked the plugin for.
struct plugin_instance_view
{
    /// Studio's number for it.
    quint64 number = 0;
    QString point_id;
    /// The name of its extension point in the plugin's description.
    QString point_name;
    plugin_instance_state state = plugin_instance_state::not_running;
    /// Studio's sentence for why it was refused.
    QString refusal;
    /// The plugin's own words about a refusal. Untrusted.
    QString reason;
    /// The plugin has older settings than the ones set.
    bool settings_pending = false;
    /// An instance of a decoder: what it found can be shown.
    bool decoder = false;
};

/// What a decoder found, as one row of the page's table.
struct plugin_found_row
{
    /// Seconds on the hub's clock, in nanoseconds.
    quint64 time_ns = 0;
    QString place;
    bool received = true;
    /// A frame's summary, or a trigger's text. The plugin's words.
    QString what;
    /// A frame's message name, or a trigger's kind; the plugin's own code for
    /// a custom trigger. Studio's name for the kinds it knows.
    QString kind;
    bool trigger = false;
};

/// What the store of one decoder instance holds, newest row first. Immutable.
struct plugin_found
{
    std::vector<plugin_found_row> rows;
    /// How many rows the store holds in all, when more than `rows`.
    quint64 held = 0;
    quint64 dropped = 0;
    quint64 refused = 0;
    quint64 over_budget = 0;
    /// Changes whenever the store does, so that a page can tell.
    quint64 revision = 0;
};
using plugin_found_ptr = std::shared_ptr<const plugin_found>;

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
    // The text, dates and lists come first and the numbers, enumerators and
    // flags last, each kind together, so that the struct has no gaps.

    // -- identity, as the plugin says it. Plain text, never markup.
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString folder;

    // -- what the host found
    /// The id of the signing key, or empty.
    QString key_id;
    /// The name the user saw when they approved the key, or empty.
    QString key_name;
    QDate key_approved_on;
    /// The first 16 characters of the plugin's hash.
    QString fingerprint;
    /// The whole hash, as the decision is tied to it.
    QString hash;

    /// The reason code of docs/plugins.md section 12, or empty.
    QString reason_code;
    /// The host's own line, for "Copy details" only.
    QString detail;
    /// What the running plugin last said of itself. Plain text, untrusted.
    QString status_text;

    // -- names the notes use
    QString limit_name;
    QString capability_name;
    QString studio_version;
    QDate expires;
    vss::license::status licence_status = vss::license::no_licence();
    /// For Not used: the copy that is used, as a row of this list.
    QString used_copy_folder;
    QString used_copy_author;
    QString used_copy_id;
    QString this_version_text;

    // -- isolation
    /// What is missing, for "Partial: <what is missing>".
    QString isolation_missing;
    QDate override_on;

    /// The program of this computer it runs with, or empty.
    QString runtime_name;

    std::vector<plugin_addition> adds;
    std::vector<plugin_permission_view> permissions;
    std::vector<plugin_use> used_in;
    /// What Studio runs of this plugin.
    std::vector<plugin_instance_view> instances;
    std::vector<plugin_log_line> log;

    // -- enumerators and numbers
    plugin_installed_for installed_for = plugin_installed_for::you_only;
    plugin_trust trust = plugin_trust::unsigned_plugin;
    plugin_state state = plugin_state::off;
    plugin_note note = plugin_note::none;
    /// For Needs a licence.
    vss::license::reason licence_reason = vss::license::reason::ok;
    /// The plugin told Studio what its own licence check found
    /// (`licence.report`). Only a reason other than `ok` is shown.
    bool has_licence_report = false;
    vss::license::reason licence_report_reason = vss::license::reason::ok;
    plugin_trust used_copy_trust = plugin_trust::unsigned_plugin;
    /// Where the copy that runs instead of this one is installed.
    plugin_installed_for used_copy_installed_for =
        plugin_installed_for::everyone;
    /// The copy that runs instead of this one is known.
    bool has_used_copy = false;
    plugin_isolation isolation = plugin_isolation::none;
    plugin_timing timing = plugin_timing::by_studio;
    int back_off_seconds = 0;
    int attempt = 0;
    int failure_count = 0;
    int budget_us = 0;

    // -- flags
    /// The user switched it on. In a build that cannot run plugins the state
    /// stays Off and this is only the wish.
    bool wished_on = false;
    bool newer_than_used_copy = false;
    /// The user may let it run without isolation (docs/plugins.md 9.2).
    bool override_allowed = false;
    bool override_active = false;
    /// This version is not the one the user approved.
    bool differs_from_approved = false;
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
        /// A plugin with this id is installed for everyone on the computer,
        /// and that copy is the one that runs.
        installed_for_everyone,
        /// A plugin with this id came with Studio, and that copy is the one
        /// that runs.
        came_with_studio,
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
    add_instance,
    remove_instance
};

enum class plugin_request_error
{
    none,
    /// "That folder is not a plugin..."
    not_a_plugin,
    /// "<plugin name> is already installed for you..."
    already_installed,
    /// "<plugin name> is already installed for everyone..."
    installed_for_everyone,
    /// "<plugin name> came with Studio..."
    came_with_studio,
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

/// A secret a running plugin asked Studio to show once (`secret.show`). The
/// text is the plugin's own; it is held only while the dialog is open and is
/// never written anywhere.
struct plugin_secret_request
{
    /// Names the request in `answer_secret`.
    quint64 id = 0;
    QString plugin_id;
    QString plugin_name;
    QString label;
    QString text;
    /// How long the secret is good for, from the moment it was asked.
    int seconds = 0;
};

/// A control session a running plugin asked for. Every text is the plugin's.
struct plugin_control_request
{
    quint64 id = 0;
    QString plugin_id;
    QString plugin_name;
    QString peer;
    QString reason;
    /// Scope names of the remote control, such as `monitor`.
    QStringList scopes;
    /// How long the session's token would live.
    int life_seconds = 0;
};

} // namespace vss::gui

Q_DECLARE_METATYPE(vss::gui::plugin_secret_request)
Q_DECLARE_METATYPE(vss::gui::plugin_control_request)
Q_DECLARE_METATYPE(vss::gui::plugin_request_result)
Q_DECLARE_METATYPE(vss::gui::plugin_key_file_view)
Q_DECLARE_METATYPE(vss::gui::plugin_inspection_view)
