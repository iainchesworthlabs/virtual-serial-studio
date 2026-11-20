//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of the Plugins page and its dialogs
// (docs/gui-design/docs/screens/10-plugins.md), apart from the widgets, so a
// test can read every sentence for every state. Text that comes from a plugin
// goes through `plain_text` and is only ever an argument of a sentence, never
// part of its format.
//

#pragma once

#include "backend/plugin_view.hpp"
#include "widgets/chips.hpp"
#include "widgets/notes.hpp"

#include <QString>
#include <QStringList>

#include <vector>

namespace vss::gui::support
{

/// `text` with every control character, line break and direction-override
/// character removed. What a plugin says is shown as this, as plain text.
QString plain_text(const QString& text);

/// `plain_text`, with a zero-width break after every run of 24 characters
/// that has no space, so a very long word wraps instead of widening a layout.
QString wrappable_text(const QString& text);

/// `text` in curly quotes, after `plain_text`.
QString quoted(const QString& text);

/// The author as Studio shows it: "Virtual Serial Studio" for an official
/// plugin whatever its manifest says, the name the user saw when approving the
/// key for a signed one, else what the plugin says.
QString display_author(const plugin_view& plugin);

QString trust_line(plugin_trust trust, const QString& author);
QString trust_line(const plugin_view& plugin);

/// "On", "Off", ... the chip text and the state in an accessible name.
QString state_text(plugin_state state);
StatusChip::kind chip_kind(plugin_state state);

/// "<name>, <trust line>, <state>".
QString row_accessible_name(const plugin_view& plugin);
/// "<trust line>, version <version>".
QString row_second_line(const plugin_view& plugin);

/// Whether the Switch of a row can be used, and the reason it cannot.
bool switch_usable(const plugin_view& plugin);

/// Where the Switch is shown as on. In a build that cannot run plugins it is
/// never on, whatever the user wished.
bool switch_shown_on(const plugin_view& plugin);

QString not_available_in_build();

/// The sentence of a permission.
QString permission_sentence(const plugin_permission_view& permission);
/// The six permissions that are drawn amber with the warning icon.
bool is_amber_permission(const QString& id);

QString signature_sentence(const plugin_view& plugin);
QString isolation_text(const plugin_view& plugin);
QString timing_text(plugin_timing timing);
QString timing_sentence(const plugin_view& plugin);
QString installed_for_text(plugin_installed_for installed);
/// Why a plugin cannot be added for the user when a copy of it is already
/// installed in `place` (everyone, or with Studio) and runs instead.
QString installed_elsewhere_text(plugin_installed_for place,
                                 const QString& name);
QString addition_text(const plugin_addition& addition);
QString runtime_line(const plugin_view& plugin);

QString plugin_mark_text();
QString official_badge_text();

/// An action under a banner.
enum class plugin_action
{
    start_now,
    review_and_decide,
    show_used_copy,
    copy_details,
    run_without_isolation,
    licence,
    start_again,
    show_log
};

QString action_text(plugin_action action);

/// Whether an action is the step to take next, which is drawn as a normal
/// button. The others are quiet. "Run without isolation..." is never the next
/// step, however alone it stands, because it gives a plugin more access.
bool is_next_step(plugin_action action);

struct plugin_note_text
{
    bool shown = false;
    NoteBanner::kind kind = NoteBanner::kind::info;
    QString text;
    std::vector<plugin_action> actions;
};

/// "Why and what to do" for the plugin's state. Not shown for Available and
/// Off.
plugin_note_text note_for(const plugin_view& plugin);

struct banner_text
{
    bool shown = false;
    NoteBanner::kind kind = NoteBanner::kind::info;
    QString text;
    /// Empty for no action.
    QString action;
};

banner_text top_note_text(plugin_banner banner);

QString dialog_title();
QString dialog_sub();
/// "<n> on" or "<n> need attention".
QString header_chip_text(int on, int attention);
bool needs_attention(plugin_state state);

QString copy_details_text(const plugin_view& plugin);

QString decision_title(const plugin_view& plugin);
/// The top banner of the decision dialog, one to three lines.
struct decision_banner_text
{
    NoteBanner::kind kind = NoteBanner::kind::info;
    QStringList lines;
};
decision_banner_text decision_banner(const plugin_view& plugin);
QString decision_run_button(const plugin_view& plugin);
/// The isolation line of the decision dialog, and whether the plugin can be
/// run at all on this computer.
struct decision_isolation_text
{
    QString text;
    /// The button is "Decide anyway": the plugin cannot be kept apart.
    bool decide_anyway = false;
};
decision_isolation_text decision_isolation(const plugin_view& plugin);

/// "Run <name>?" strings of the isolation dialog.
QString no_isolation_title(const plugin_view& plugin);

} // namespace vss::gui::support
