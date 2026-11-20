//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugin_parts.hpp"

#include "support/licence_text.hpp"
#include "support/plugin_text.hpp"

#include <QCoreApplication>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

/// The plugin as the part names it: the host's name when it is installed, else
/// what the setup recorded, else the id.
QString shown_name(const plugin_part& part, const plugin_view* plugin)
{
    if (plugin != nullptr && !plain_text(plugin->name).isEmpty())
        return plain_text(plugin->name);
    if (!plain_text(part.plugin_name).isEmpty())
        return plain_text(part.plugin_name);
    return plain_text(part.plugin_id);
}

bool is_running(const plugin_view* plugin)
{
    return plugin != nullptr && (plugin->state == plugin_state::available ||
                                 plugin->state == plugin_state::starting);
}

} // namespace

QString part_action_text(part_action action)
{
    switch (action)
    {
    case part_action::none:
        return {};
    case part_action::switch_on:
        return QCoreApplication::translate("PluginPart", "Switch on");
    case part_action::review_and_decide:
        return QCoreApplication::translate("PluginPart", "Review and decide");
    case part_action::show_in_plugins:
        return QCoreApplication::translate("PluginPart", "Show in Plugins");
    case part_action::licence:
        return QCoreApplication::translate("PluginPart", "Licence");
    case part_action::start_again:
        return QCoreApplication::translate("PluginPart", "Start again");
    }
    return {};
}

part_description describe_part(const plugin_part& part,
                               const plugin_view* plugin)
{
    // A null plugin means the plugin is not installed. Start with the fields
    // that are true whatever the state is.
    part_description out;
    out.plugin_id = part.plugin_id;
    const QString name = shown_name(part, plugin);
    const QString type = plain_text(part.type_name);
    out.state = plugin != nullptr ? plugin->state : plugin_state::not_installed;
    out.state_text = state_text(out.state);

    // The line saying where the part comes from adds the trust level only
    // when the plugin is known.
    if (plugin != nullptr)
    {
        out.provided_by = QCoreApplication::translate(
                              "PluginPart", "%1, from the plugin %2 (%3)")
                              .arg(type, name, trust_line(*plugin));
    }
    else
    {
        out.provided_by =
            QCoreApplication::translate("PluginPart", "%1, from the plugin %2")
                .arg(type, name);
    }
    // A running plugin needs no badge, action or note, so return early with
    // only the accessible name filled in.
    out.running = is_running(plugin);
    if (out.running)
    {
        out.accessible_name =
            QCoreApplication::translate("PluginPart", "%1, %2")
                .arg(plain_text(part.target_name), type);
        return out;
    }
    // From here the plugin is not running: mark the part with a badge and a
    // matching accessible name.
    out.badge =
        QCoreApplication::translate("PluginPart", "Plugin not available");
    out.accessible_name = QCoreApplication::translate(
                              "PluginPart", "%1, %2, plugin not available")
                              .arg(plain_text(part.target_name), type);

    // The action and how serious it is follow the plugin's state; the
    // sentence follows what the part is.
    const auto act = [&out](part_action action, part_tone tone)
    {
        out.action = action;
        out.tone = tone;
        out.action_text = part_action_text(action);
    };
    // The groups pick the wording of the note. Several plugin states share
    // one group when the note would say the same thing.
    enum class group
    {
        off,
        decision,
        blocked,
        licence,
        stopped,
        cannot,
        missing
    };
    // Map the plugin state to a group and to the action offered. A missing
    // plugin is the default.
    group which = group::missing;
    if (plugin != nullptr)
    {
        switch (plugin->state)
        {
        case plugin_state::off:
            which = group::off;
            act(part_action::switch_on, part_tone::info);
            break;
        case plugin_state::needs_decision:
            which = group::decision;
            act(part_action::review_and_decide, part_tone::warn);
            break;
        case plugin_state::blocked:
        case plugin_state::needs_isolation:
            which = group::blocked;
            act(part_action::show_in_plugins, part_tone::info);
            break;
        case plugin_state::needs_licence:
            which = group::licence;
            act(part_action::licence, part_tone::warn);
            break;
        case plugin_state::stopped:
            // A stopped plugin that still has fixed replies is less serious
            // than one that has nothing to send.
            which = group::stopped;
            act(part_action::start_again,
                part.fixed_replies ? part_tone::warn : part_tone::danger);
            break;
        case plugin_state::needs_newer_studio:
        case plugin_state::cannot_be_used:
        case plugin_state::not_used:
            which = group::cannot;
            act(part_action::show_in_plugins, part_tone::info);
            break;
        case plugin_state::not_installed:
            which = group::missing;
            act(part_action::show_in_plugins, part_tone::info);
            break;
        case plugin_state::available:
        case plugin_state::starting:
            // These states are not described as unavailable here.
            break;
        }
    }
    else
    {
        // No plugin record at all: the only help is the plugins page.
        act(part_action::show_in_plugins, part_tone::info);
    }

    // The note depends first on the kind of part. Generators and link
    // behaviours have their own wording; fixtures use the group below.
    out.note_shown = true;
    if (part.kind == part_kind::generator)
    {
        out.note = QCoreApplication::translate(
                       "PluginPart", "The generator %1 is not running.")
                       .arg(type);
        return out;
    }
    // A link behaviour that is not running either drops data or passes it
    // on unchanged, and the note says which.
    if (part.kind == part_kind::link_behaviour)
    {
        out.note =
            part.drops_on_timeout
                ? QCoreApplication::translate(
                      "PluginPart",
                      "The link behaviour %1 is not running, so data on this "
                      "link is being dropped.")
                      .arg(type)
                : QCoreApplication::translate(
                      "PluginPart",
                      "The link behaviour %1 is not running, so data on this "
                      "link passes unchanged.")
                      .arg(type);
        return out;
    }
    // A fixture note: why the plugin is not running and what the port does
    // meanwhile.
    switch (which)
    {
    case group::off:
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture comes from the plugin %1, which is "
                       "switched off. The port stays open and sends nothing.")
                       .arg(name);
        break;
    case group::decision:
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture comes from the plugin %1, which is "
                       "waiting for your decision. The port stays open and "
                       "sends nothing.")
                       .arg(name);
        break;
    case group::blocked:
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture comes from the plugin %1, which your "
                       "administrator has blocked. The port stays open and "
                       "sends nothing.")
                       .arg(name);
        break;
    case group::licence:
        // The licence reason comes from the plugin's own licence state.
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture comes from the plugin %1. %2 The port "
                       "stays open and sends nothing.")
                       .arg(name, reason_sentence(plugin->licence_reason,
                                                  plugin->licence_status));
        break;
    case group::stopped:
        // With fixed replies the fixture keeps answering without the plugin.
        out.note =
            part.fixed_replies
                ? QCoreApplication::translate(
                      "PluginPart",
                      "The plugin %1 has stopped. This fixture still sends "
                      "its fixed replies. Anything the plugin worked out "
                      "itself has stopped.")
                      .arg(name)
                : QCoreApplication::translate(
                      "PluginPart",
                      "The plugin %1 has stopped, so this fixture sends "
                      "nothing.")
                      .arg(name);
        break;
    case group::cannot:
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture comes from the plugin %1, which cannot "
                       "run in this version of Studio. The port stays open "
                       "and sends nothing.")
                       .arg(name);
        break;
    case group::missing:
        out.note = QCoreApplication::translate(
                       "PluginPart",
                       "This fixture needs the plugin %1, which is not "
                       "installed on this computer. The port stays open and "
                       "sends nothing.")
                       .arg(name);
        break;
    }
    return out;
}

QHash<vss::endpoint_id, part_description>
describe_port_parts(const std::vector<port_view>& ports,
                    const vss::config::document& setup,
                    const plugin_snapshot& snapshot)
{
    QHash<vss::endpoint_id, part_description> out;
    for (const vss::config::port_entry& entry : setup.ports)
    {
        for (const vss::config::plugin_binding& binding : entry.plugins)
        {
            if (binding.kind != "fixture" && binding.kind != "generator")
                continue;
            const QString name = QString::fromStdString(entry.name);
            const auto port = std::find_if(ports.begin(), ports.end(),
                                           [&name](const port_view& each)
                                           { return each.name == name; });
            if (port == ports.end())
                continue;
            plugin_part part;
            part.plugin_id = QString::fromStdString(binding.id);
            part.plugin_name = QString::fromStdString(binding.name);
            part.target_name = name;
            part.kind = binding.kind == "generator" ? part_kind::generator
                                                    : part_kind::fixture;
            part.type_name = QString::fromStdString(binding.type);
            // The copy that is used, not one that is shadowed.
            const plugin_view* found = nullptr;
            for (const plugin_view& each : snapshot.plugins)
            {
                if (each.id == part.plugin_id &&
                    (found == nullptr ||
                     found->state == plugin_state::not_used))
                    found = &each;
            }
            if (found != nullptr)
            {
                for (const plugin_addition& add : found->adds)
                {
                    if (add.type_id == part.type_name)
                        part.type_name = add.type_name;
                }
            }
            out.insert(port->id, describe_part(part, found));
        }
    }
    return out;
}

// -- the toolbox --------------------------------------------------------------

std::vector<plugin_tile_text>
plugin_toolbox_tiles(const plugin_snapshot& snapshot, bool fixtures_can_run)
{
    // Collect one tile per fixture type a plugin adds, keeping the plugin's
    // name beside it so the tiles can be sorted afterwards.
    struct entry
    {
        QString plugin_name;
        plugin_tile_text tile;
    };
    std::vector<entry> entries;
    for (const plugin_view& plugin : snapshot.plugins)
    {
        // The plugin's state decides whether its tiles can be used and, if
        // not, what the tile says about why.
        QString reason;
        bool enabled = false;
        const QString name = plain_text(plugin.name);
        switch (plugin.state)
        {
        case plugin_state::available:
            enabled = true;
            break;
        case plugin_state::starting:
            reason =
                QCoreApplication::translate("PluginToolbox", "%1 is starting.")
                    .arg(name);
            break;
        case plugin_state::needs_licence:
            reason =
                reason_sentence(plugin.licence_reason, plugin.licence_status);
            break;
        case plugin_state::stopped:
            reason = QCoreApplication::translate(
                         "PluginToolbox",
                         "%1 has stopped. Open Plugins to start it again.")
                         .arg(name);
            break;
        case plugin_state::off:
        case plugin_state::needs_decision:
        case plugin_state::blocked:
        case plugin_state::needs_isolation:
        case plugin_state::not_used:
        case plugin_state::needs_newer_studio:
        case plugin_state::cannot_be_used:
        case plugin_state::not_installed:
            continue;
        }
        for (const plugin_addition& addition : plugin.adds)
        {
            // Only fixtures become toolbox tiles; other additions are placed
            // elsewhere.
            if (addition.kind != plugin_addition_kind::fixture)
                continue;
            plugin_tile_text tile;
            tile.plugin_id = plugin.id;
            tile.type_id = addition.type_id;
            tile.label = plain_text(addition.type_name);
            // A plugin that is available still cannot stand in for a device
            // until the engine can play its timeline.
            const QString no_timeline = QCoreApplication::translate(
                "PluginToolbox",
                "%1 cannot be placed yet. Standing in for a device needs the "
                "engine timeline, which this version of Studio does not "
                "have.");
            const bool usable = enabled && fixtures_can_run;
            const QString why =
                enabled && !fixtures_can_run
                    ? no_timeline.arg(plain_text(addition.type_name))
                    : reason;
            // A tile that cannot be used shows the reason where a
            // description would otherwise be.
            tile.enabled = usable;
            tile.reason = why;
            tile.accessible_name =
                QCoreApplication::translate("PluginToolbox", "%1, plugin")
                    .arg(tile.label);
            QString description =
                QCoreApplication::translate("PluginToolbox",
                                            "Fixture from the plugin %1. %2.")
                    .arg(name, trust_line(plugin));
            // The plugin's own description is appended after the trust line.
            const QString own = plain_text(addition.description);
            if (!own.isEmpty())
                description += QLatin1Char(' ') + own;
            tile.accessible_description = usable ? description : why;
            tile.tooltip =
                usable ? QCoreApplication::translate(
                              "PluginToolbox", "%1\nFrom the plugin %2\n%3")
                              .arg(tile.label, name, trust_line(plugin))
                        : why;
            entries.push_back({name, tile});
        }
    }
    // Order by plugin name, then by tile label, ignoring case. A stable sort
    // keeps equal entries in the order the plugins listed them.
    std::stable_sort(
        entries.begin(), entries.end(),
        [](const entry& left, const entry& right)
        {
            const int by_plugin = QString::compare(
                left.plugin_name, right.plugin_name, Qt::CaseInsensitive);
            if (by_plugin != 0)
                return by_plugin < 0;
            return QString::compare(left.tile.label, right.tile.label,
                                    Qt::CaseInsensitive) < 0;
        });
    std::vector<plugin_tile_text> tiles;
    tiles.reserve(entries.size());
    for (entry& each : entries)
        tiles.push_back(std::move(each.tile));
    return tiles;
}

// -- decoders -----------------------------------------------------------------

std::vector<decoder_choice> decoder_choices(const plugin_snapshot& snapshot)
{
    std::vector<decoder_choice> out;
    for (const plugin_view& plugin : snapshot.plugins)
    {
        QString reason;
        bool enabled = false;
        const QString name = plain_text(plugin.name);
        switch (plugin.state)
        {
        case plugin_state::available:
            enabled = true;
            break;
        case plugin_state::starting:
            reason =
                QCoreApplication::translate("PluginToolbox", "%1 is starting.")
                    .arg(name);
            break;
        case plugin_state::needs_licence:
            reason =
                reason_sentence(plugin.licence_reason, plugin.licence_status);
            break;
        case plugin_state::stopped:
            reason = QCoreApplication::translate(
                         "PluginToolbox",
                         "%1 has stopped. Open Plugins to start it again.")
                         .arg(name);
            break;
        case plugin_state::off:
        case plugin_state::needs_decision:
        case plugin_state::blocked:
        case plugin_state::needs_isolation:
        case plugin_state::not_used:
        case plugin_state::needs_newer_studio:
        case plugin_state::cannot_be_used:
        case plugin_state::not_installed:
            continue;
        }
        for (const plugin_addition& addition : plugin.adds)
        {
            if (!addition.decoder)
                continue;
            decoder_choice choice;
            choice.plugin_id = plugin.id;
            choice.plugin_name = name;
            choice.type_id = addition.type_id;
            choice.type_name = plain_text(addition.type_name);
            bool other_setting = addition.needs_settings;
            for (const plugin_parameter_ask& ask : addition.asks)
            {
                if (ask.place_name && choice.place_setting.isEmpty())
                    choice.place_setting = ask.id;
                else
                    other_setting = true;
            }
            choice.usable = enabled && !other_setting;
            choice.reason = reason;
            if (enabled && other_setting)
                choice.reason = QCoreApplication::translate(
                                    "PluginToolbox",
                                    "%1 needs settings that are set in Plugins.")
                                    .arg(choice.type_name);
            out.push_back(std::move(choice));
        }
    }
    std::stable_sort(
        out.begin(), out.end(),
        [](const decoder_choice& left, const decoder_choice& right)
        {
            const int by_plugin = QString::compare(
                left.plugin_name, right.plugin_name, Qt::CaseInsensitive);
            if (by_plugin != 0)
                return by_plugin < 0;
            return QString::compare(left.type_name, right.type_name,
                                    Qt::CaseInsensitive) < 0;
        });
    return out;
}

// -- the chip in the status bar -----------------------------------------------

traffic_chip_text traffic_chip(const std::vector<plugin_reader>& readers)
{
    traffic_chip_text out;
    if (readers.empty())
        return out;
    out.shown = true;
    out.recording =
        std::any_of(readers.begin(), readers.end(),
                    [](const plugin_reader& each) { return each.recording; });
    out.text =
        out.recording
            ? QCoreApplication::translate("PluginTrafficChip", "Recording")
            : QCoreApplication::translate("PluginTrafficChip", "Reading data");
    if (readers.size() > 1)
    {
        out.accessible_name = QCoreApplication::translate(
            "PluginTrafficChip", "%n plugins are reading data. Open Plugins.",
            nullptr, static_cast<int>(readers.size()));
        return out;
    }
    const plugin_reader& one = readers.front();
    const QString name = plain_text(one.plugin_name);
    if (one.recording)
    {
        out.accessible_name =
            one.port_count == 1
                ? QCoreApplication::translate(
                      "PluginTrafficChip",
                      "%1 is recording data on 1 port. Open Plugins.")
                      .arg(name)
                : QCoreApplication::translate(
                      "PluginTrafficChip",
                      "%1 is recording data on %n ports. Open Plugins.",
                      nullptr, one.port_count)
                      .arg(name);
    }
    else
    {
        out.accessible_name =
            one.port_count == 1
                ? QCoreApplication::translate(
                      "PluginTrafficChip",
                      "%1 is reading data on 1 port. Open Plugins.")
                      .arg(name)
                : QCoreApplication::translate(
                      "PluginTrafficChip",
                      "%1 is reading data on %n ports. Open Plugins.", nullptr,
                      one.port_count)
                      .arg(name);
    }
    return out;
}

// -- notices ------------------------------------------------------------------

std::vector<plugin_notice_text> plugin_notices(const plugin_snapshot& before,
                                               const plugin_snapshot& after,
                                               const QStringList& used)
{
    // Compare two snapshots and raise a notice for each plugin that changed
    // in a way the person should hear about.
    std::vector<plugin_notice_text> out;
    // Plugins are matched by id and folder, because one id can be installed
    // in more than one folder.
    const auto find = [](const plugin_snapshot& snapshot,
                         const plugin_view& like) -> const plugin_view*
    {
        for (const plugin_view& each : snapshot.plugins)
        {
            if (each.id == like.id && each.folder == like.folder)
                return &each;
        }
        return nullptr;
    };
    // A plugin counts as used when the setup lists it or the caller says so.
    const auto is_used = [&used](const plugin_view& plugin)
    { return !plugin.used_in.empty() || used.contains(plugin.id); };
    // States in which the plugin has been started at some point.
    const auto running = [](plugin_state state)
    {
        return state == plugin_state::available ||
               state == plugin_state::starting ||
               state == plugin_state::stopped;
    };

    // Walk the new snapshot and look each plugin up in the old one.
    for (const plugin_view& now : after.plugins)
    {
        const QString name = plain_text(now.name);
        const plugin_view* was = find(before, now);
        plugin_notice_text notice;
        notice.plugin_id = now.id;
        notice.folder = now.folder;
        if (was == nullptr)
        {
            // A plugin found that was not there, which has to be decided.
            if (now.state != plugin_state::needs_decision)
                continue;
            notice.kind = plugin_notice_kind::new_plugin;
            notice.text =
                QCoreApplication::translate(
                    "PluginNotice",
                    "A new plugin was found: %1. It will not run until you "
                    "decide.")
                    .arg(name);
            notice.action = QCoreApplication::translate("PluginNotice",
                                                        "Review and decide");
            notice.action_decides = true;
            out.push_back(notice);
            continue;
        }
        // Nothing changed that matters, so say nothing.
        if (was->state == now.state && was->hash == now.hash)
            continue;
        // Changed after approval: the content hash differs and the plugin
        // was put back to waiting for a decision.
        if (now.state == plugin_state::needs_decision &&
            now.note == plugin_note::decision_changed && was->hash != now.hash)
        {
            notice.kind = plugin_notice_kind::changed;
            notice.warn = true;
            notice.text =
                QCoreApplication::translate(
                    "PluginNotice",
                    "%1 changed since you approved it and has been stopped.")
                    .arg(name);
            notice.action = QCoreApplication::translate("PluginNotice",
                                                        "Review and decide");
            notice.action_decides = true;
            out.push_back(notice);
        }
        else if (now.state == plugin_state::stopped &&
                 was->state != plugin_state::stopped && is_used(now))
        {
            // Only a plugin that the open setup uses is worth a notice.
            notice.kind = plugin_notice_kind::stopped;
            notice.warn = true;
            notice.text =
                QCoreApplication::translate(
                    "PluginNotice",
                    "The plugin %1 has stopped. Parts of this setup that use "
                    "it are not running.")
                    .arg(name);
            notice.action =
                QCoreApplication::translate("PluginNotice", "Plugins");
            notice.second =
                QCoreApplication::translate("PluginNotice", "Start again");
            out.push_back(notice);
        }
        else if (now.state == plugin_state::cannot_be_used &&
                 now.note == plugin_note::withdrawn && running(was->state))
        {
            // The publisher withdrew a plugin that was running.
            notice.kind = plugin_notice_kind::withdrawn;
            notice.warn = true;
            notice.text =
                QCoreApplication::translate(
                    "PluginNotice",
                    "%1 was withdrawn by Virtual Serial Studio and has been "
                    "stopped.")
                    .arg(name);
            notice.action =
                QCoreApplication::translate("PluginNotice", "Plugins");
            out.push_back(notice);
        }
    }
    return out;
}

} // namespace vss::gui::support
