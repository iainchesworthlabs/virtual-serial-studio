//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the rest of Studio says about plugins
// (docs/gui-design/docs/screens/10-plugins.md, sections "Toolbox", "A plugin's
// parts on the canvas and in the lists", "Plugin notices" and "A plugin is
// reading traffic"): the toolbox tiles of a plugin's fixture types, the note
// for a port, a link or a generator whose plugin is not available, and the
// chip that says a plugin reads data. These are pure functions of what the
// host reported, so every state can be read in a test.
//

#pragma once

#include "backend/plugin_part_view.hpp"
#include "backend/plugin_view.hpp"

#include "backend/view_types.hpp"

#include <vss/config/document.hpp>

#include <QHash>
#include <QString>

#include <vector>

namespace vss::gui::support
{

// -- the toolbox --------------------------------------------------------------

/// One tile of the Plugins group: a fixture type of a plugin that is on.
struct plugin_tile_text
{
    QString plugin_id;
    QString type_id;
    /// The type's name, which is the tile's label.
    QString label;
    /// "<type name>, plugin".
    QString accessible_name;
    /// "Fixture from the plugin <plugin name>. <trust line>." and the type's
    /// own description.
    QString accessible_description;
    /// Three lines when the tile is enabled, the reason when it is not.
    QString tooltip;
    /// Why the tile is disabled, or empty.
    QString reason;
    bool enabled = false;
};

/// The tiles of the Plugins group, ordered by plugin name and then type name.
/// A plugin the user switched off, or cannot switch on, has none: the rule of
/// the spec is to hide what the user did not choose and to show, disabled
/// and with its reason, what they did choose and cannot use now.
/// `fixtures_can_run` is `capabilities::plugin_fixtures`: without it a tile of
/// a fixture type is disabled, with the reason, however well the plugin is.
std::vector<plugin_tile_text>
plugin_toolbox_tiles(const plugin_snapshot& snapshot,
                     bool fixtures_can_run = true);

// -- decoders -----------------------------------------------------------------

/// One decoder type of a plugin, as "Decode as..." offers it.
struct decoder_choice
{
    QString plugin_id;
    QString plugin_name;
    QString type_id;
    QString type_name;
    /// The id of the setting that names the place to read, or empty.
    QString place_setting;
    /// It can be started from "Decode as...". When not, `reason` says why.
    bool usable = false;
    QString reason;
};

/// The decoder types of the plugins that are on, ordered by plugin name and
/// then type name. A plugin that is off or cannot be switched on has none, as
/// for the toolbox; one that is on and cannot be used now is listed with the
/// reason. A decoder with a setting other than the place to read is listed
/// as one to set up in Plugins.
std::vector<decoder_choice> decoder_choices(const plugin_snapshot& snapshot);

// -- parts on the canvas ------------------------------------------------------

enum class part_kind
{
    fixture,
    generator,
    link_behaviour
};

/// What a saved setup records about one thing a plugin provides.
struct plugin_part
{
    QString plugin_id;
    /// The plugin's name as the setup recorded it, for when it is not
    /// installed. Plain text.
    QString plugin_name;
    QString type_name;
    /// The port's or the link's name.
    QString target_name;
    part_kind kind = part_kind::fixture;
    /// The fixture answers with fixed replies of its own when its plugin
    /// stops.
    bool fixed_replies = false;
    /// A link behaviour that was set to stop data when the plugin does not
    /// answer.
    bool drops_on_timeout = false;
};

using vss::gui::part_action;
using vss::gui::part_description;
using vss::gui::part_tone;

/// `plugin` is the plugin the part names, or null when none with that id is
/// installed.
part_description describe_part(const plugin_part& part,
                               const plugin_view* plugin);

QString part_action_text(part_action action);

/// The plugin parts of the ports of a setup: for each port that has a fixture
/// or a generator from a plugin, what it says about that plugin now. A port
/// whose plugin is available is in the result too, with `running` set.
QHash<vss::endpoint_id, part_description>
describe_port_parts(const std::vector<port_view>& ports,
                    const vss::config::document& setup,
                    const plugin_snapshot& snapshot);

// -- the chip in the status bar -----------------------------------------------

struct traffic_chip_text
{
    bool shown = false;
    /// "Recording" or "Reading data".
    QString text;
    bool recording = false;
    QString accessible_name;
};

traffic_chip_text traffic_chip(const std::vector<plugin_reader>& readers);

// -- notices in the main window -----------------------------------------------

enum class plugin_notice_kind
{
    new_plugin,
    changed,
    stopped,
    withdrawn
};

struct plugin_notice_text
{
    plugin_notice_kind kind = plugin_notice_kind::new_plugin;
    /// "info" and "warn" as the banner kinds.
    bool warn = false;
    QString plugin_id;
    QString folder;
    QString text;
    /// The first action, which opens Plugins or the decision.
    QString action;
    bool action_decides = false;
    /// "Start again", for a stopped plugin; empty otherwise.
    QString second;
};

/// The notices that follow from the host's snapshot changing from `before` to
/// `after`. Nothing is said about a plugin that was already known and did not
/// change. `used` is the set of plugin ids this setup uses.
std::vector<plugin_notice_text> plugin_notices(const plugin_snapshot& before,
                                               const plugin_snapshot& after,
                                               const QStringList& used);

} // namespace vss::gui::support
