//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a port that a plugin provides is told about its plugin: the words and
// the action of docs/gui-design/docs/screens/10-plugins.md, "A plugin's parts
// on the canvas and in the lists". The shell works it out from the host's
// snapshot and the setup; the canvas and the Inspector only show it.
//

#pragma once

#include "backend/plugin_view.hpp"

#include <QString>

namespace vss::gui
{

enum class part_action
{
    none,
    switch_on,
    review_and_decide,
    show_in_plugins,
    licence,
    start_again
};

enum class part_tone
{
    info,
    warn,
    danger
};

struct part_description
{
    /// The plugin is available: its parts run, and nothing is said.
    bool running = false;
    /// "<type name>, from the plugin <plugin name> (<trust line>)".
    QString provided_by;
    /// "Plugin not available" while it is not running, else empty.
    QString badge;
    /// "<port name>, <type name>, plugin not available" while it is not
    /// running, else "<port name>, <type name>".
    QString accessible_name;
    QString state_text;
    plugin_state state = plugin_state::not_installed;
    bool note_shown = false;
    part_tone tone = part_tone::info;
    QString note;
    part_action action = part_action::none;
    QString action_text;
    QString plugin_id;
};

} // namespace vss::gui
