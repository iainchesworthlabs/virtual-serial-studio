//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the rest of Studio shows plugins
// (docs/gui-design/docs/screens/10-plugins.md): the toolbox tiles, the notes
// on the ports that a plugin provides, the notices at the top of the window,
// the chip in the status bar and the link in the Licence dialog. Every state
// is made with the fake plugin backend.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_plugin_backend.hpp"
#include "backend/fake_studio_backend.hpp"
#include "panes/inspector_panel.hpp"
#include "shell/plugin_notice.hpp"
#include "shell/shell_window.hpp"
#include "shell/status_bar.hpp"
#include "support/licence_dialog.hpp"
#include "support/licence_text.hpp"
#include "support/plugin_parts.hpp"
#include "topology/toolbox.hpp"
#include "topology/topology_view.hpp"
#include "widgets/notes.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>

#include <vector>

using namespace vss::gui;
using namespace vss::gui::support;

namespace
{

void settle()
{
    for (int round = 0; round < 4; ++round)
        QApplication::processEvents();
}

plugin_view sample_of(const char* id)
{
    for (const plugin_view& each : fake_plugin_samples())
    {
        if (each.id == QString::fromLatin1(id))
            return each;
    }
    return {};
}

plugin_snapshot all_samples()
{
    plugin_snapshot snapshot;
    snapshot.plugins = fake_plugin_samples();
    return snapshot;
}

plugin_part fixture_part(const char* id)
{
    plugin_part part;
    part.plugin_id = QString::fromLatin1(id);
    part.type_name = QStringLiteral("Meter");
    part.target_name = QStringLiteral("meter-1");
    return part;
}

QString licence_sentence()
{
    return reason_sentence(vss::license::reason::no_licence,
                           vss::license::no_licence());
}

} // namespace

// -- toolbox ------------------------------------------------------------------

TEST_CASE("plugin tiles: without the engine timeline a fixture tile is "
          "disabled and says why")
{
    const std::vector<plugin_tile_text> tiles =
        plugin_toolbox_tiles(all_samples(), false);
    REQUIRE(!tiles.empty());
    for (const plugin_tile_text& each : tiles)
    {
        REQUIRE(!each.enabled);
        REQUIRE(!each.reason.isEmpty());
        REQUIRE_EQ(each.tooltip, each.reason);
        REQUIRE_EQ(each.accessible_description, each.reason);
    }
    // The one that would have been enabled names the missing timeline.
    bool timeline = false;
    for (const plugin_tile_text& each : tiles)
        timeline = timeline || each.reason.contains(QStringLiteral("timeline"));
    REQUIRE(timeline);
}

TEST_CASE("decoders: a decoder is offered with its place and a plugin that "
          "cannot run now is listed with the reason")
{
    plugin_snapshot snapshot = all_samples();
    plugin_view decoder = snapshot.plugins.front();
    decoder.state = plugin_state::available;
    decoder.adds.clear();
    plugin_addition one;
    one.kind = plugin_addition_kind::link_behaviour;
    one.type_name = QStringLiteral("Modbus RTU");
    one.type_id = QStringLiteral("modbus-rtu");
    one.decoder = true;
    plugin_parameter_ask place;
    place.id = QStringLiteral("place");
    place.label = QStringLiteral("Place");
    place.place_name = true;
    one.asks.push_back(place);
    decoder.adds.push_back(one);
    plugin_addition other = one;
    other.type_name = QStringLiteral("Needs more");
    other.type_id = QStringLiteral("more");
    plugin_parameter_ask extra;
    extra.id = QStringLiteral("file");
    extra.label = QStringLiteral("File");
    other.asks.push_back(extra);
    decoder.adds.push_back(other);
    snapshot.plugins = {decoder};

    const auto choices = decoder_choices(snapshot);
    REQUIRE_EQ(choices.size(), std::size_t{2});
    REQUIRE_EQ(choices[0].type_name, QStringLiteral("Modbus RTU"));
    REQUIRE(choices[0].usable);
    REQUIRE_EQ(choices[0].place_setting, QStringLiteral("place"));
    // An extra setting cannot be asked here: it is set up in Plugins.
    REQUIRE_EQ(choices[1].type_name, QStringLiteral("Needs more"));
    REQUIRE(!choices[1].usable);
    REQUIRE(choices[1].reason.contains(QStringLiteral("Plugins")));

    snapshot.plugins[0].state = plugin_state::starting;
    const auto starting = decoder_choices(snapshot);
    REQUIRE_EQ(starting.size(), std::size_t{2});
    REQUIRE(!starting[0].usable);
    REQUIRE(starting[0].reason.contains(QStringLiteral("starting")));

    snapshot.plugins[0].state = plugin_state::off;
    REQUIRE(decoder_choices(snapshot).empty());
}

TEST_CASE("plugin tiles: only what the user switched on shows, enabled only "
          "while it runs, and a disabled tile says why")
{
    const std::vector<plugin_tile_text> tiles =
        plugin_toolbox_tiles(all_samples());
    // Switched off, waiting for a decision, blocked, not compatible, not
    // installed and the rest have no tile. The four that are on have one,
    // ordered by plugin name.
    QStringList labels;
    for (const plugin_tile_text& each : tiles)
        labels << each.label;
    REQUIRE_EQ(labels, (QStringList{QStringLiteral("Crashy device"),
                                    QStringLiteral("Modbus fixture device"),
                                    QStringLiteral("Protocol decoding device"),
                                    QStringLiteral("Replay device")}));

    const auto tile = [&](const QString& label)
    {
        for (const plugin_tile_text& each : tiles)
        {
            if (each.label == label)
                return each;
        }
        return plugin_tile_text{};
    };
    const plugin_tile_text modbus =
        tile(QStringLiteral("Modbus fixture device"));
    REQUIRE(modbus.enabled);
    REQUIRE(modbus.reason.isEmpty());
    REQUIRE_EQ(modbus.accessible_name,
               QStringLiteral("Modbus fixture device, plugin"));
    REQUIRE_EQ(modbus.accessible_description,
               QStringLiteral("Fixture from the plugin Modbus fixture. "
                              "Official."));
    REQUIRE_EQ(modbus.tooltip,
               QStringLiteral("Modbus fixture device\nFrom the plugin Modbus "
                              "fixture\nOfficial"));

    const plugin_tile_text replay = tile(QStringLiteral("Replay device"));
    REQUIRE(!replay.enabled);
    REQUIRE_EQ(replay.reason, QStringLiteral("Replay is starting."));
    REQUIRE_EQ(replay.tooltip, replay.reason);
    REQUIRE_EQ(replay.accessible_description, replay.reason);
    const plugin_tile_text crashy = tile(QStringLiteral("Crashy device"));
    REQUIRE(!crashy.enabled);
    REQUIRE_EQ(crashy.reason,
               QStringLiteral("Crashy has stopped. Open Plugins to start it "
                              "again."));
    const plugin_tile_text decode =
        tile(QStringLiteral("Protocol decoding device"));
    REQUIRE(!decode.enabled);
    REQUIRE_EQ(decode.reason, licence_sentence());

    // A plugin that gives no fixture gives no tile; a hostile name is plain.
    plugin_snapshot odd;
    plugin_view hostile = fake_hostile_plugin();
    hostile.state = plugin_state::available;
    hostile.adds = {
        {plugin_addition_kind::fixture, hostile.name, QStringLiteral("x"), {}}};
    odd.plugins = {hostile};
    const auto hostile_tiles = plugin_toolbox_tiles(odd);
    REQUIRE_EQ(hostile_tiles.size(), std::size_t{1});
    REQUIRE(!hostile_tiles[0].label.contains(QChar(0x202e)));
    REQUIRE(!hostile_tiles[0].tooltip.contains(QChar(0x202e)));
}

TEST_CASE("plugin tiles: the toolbox has a third group only while there is a "
          "tile, a disabled tile takes focus, and an enabled one is added "
          "with Enter or Space")
{
    topology::Toolbox toolbox;
    toolbox.show();
    settle();
    REQUIRE(!toolbox.has_plugin_group());
    int groups = 0;
    for (int row = 0; row < toolbox.count(); ++row)
        groups +=
            toolbox.item(row)->text() == QStringLiteral("Plugins") ? 1 : 0;
    REQUIRE_EQ(groups, 0);

    std::vector<topology::plugin_tile> tiles;
    for (const plugin_tile_text& each : plugin_toolbox_tiles(all_samples()))
    {
        topology::plugin_tile tile;
        tile.plugin_id = each.plugin_id;
        tile.type_id = each.type_id;
        tile.label = each.label;
        tile.accessible_name = each.accessible_name;
        tile.accessible_description = each.accessible_description;
        tile.tooltip = each.tooltip;
        tile.enabled = each.enabled;
        tiles.push_back(tile);
    }
    toolbox.set_plugin_tiles(tiles);
    settle();
    REQUIRE(toolbox.has_plugin_group());
    // The group label follows the group, after Ports and Bundles.
    QStringList order;
    for (int row = 0; row < toolbox.count(); ++row)
    {
        const QString text = toolbox.item(row)->text();
        if (text == QStringLiteral("Ports") ||
            text == QStringLiteral("Bundles") ||
            text == QStringLiteral("Plugins"))
            order << text;
    }
    REQUIRE_EQ(order,
               (QStringList{QStringLiteral("Ports"), QStringLiteral("Bundles"),
                            QStringLiteral("Plugins")}));
    REQUIRE_EQ(toolbox.plugin_tile_labels().size(), 4);
    REQUIRE(toolbox.plugin_tile_enabled(tiles[1].plugin_id, tiles[1].type_id));
    REQUIRE(!toolbox.plugin_tile_enabled(tiles[0].plugin_id, tiles[0].type_id));

    QStringList added;
    QObject::connect(&toolbox, &topology::Toolbox::plugin_add_requested,
                     [&added](const QString& plugin, const QString& type)
                     { added << plugin + QLatin1Char('/') + type; });
    // Space on an enabled tile adds it.
    toolbox.focus_plugin_tile(tiles[1].plugin_id, tiles[1].type_id);
    REQUIRE(tiles[1].enabled);
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&toolbox, &space);
    REQUIRE_EQ(added.size(), 1);
    // A disabled tile takes focus, so its reason can be read, and adds
    // nothing.
    toolbox.focus_plugin_tile(tiles[0].plugin_id, tiles[0].type_id);
    REQUIRE(!tiles[0].enabled);
    REQUIRE_EQ(
        toolbox.currentItem()->data(Qt::AccessibleDescriptionRole).toString(),
        tiles[0].accessible_description);
    QApplication::sendEvent(&toolbox, &space);
    REQUIRE_EQ(added.size(), 1);
    REQUIRE_EQ(toolbox.currentItem()->toolTip(), tiles[0].tooltip);

    // Switching a plugin off takes its tiles away at once.
    toolbox.set_plugin_tiles({});
    REQUIRE(!toolbox.has_plugin_group());
    REQUIRE_EQ(toolbox.plugin_tile_labels().size(), 0);
}

// -- parts on the canvas ------------------------------------------------------

TEST_CASE("plugin parts: a fixture whose plugin cannot run says so, in the "
          "words of the spec, and offers the action")
{
    struct row
    {
        const char* id;
        part_action action;
        const char* action_text;
        part_tone tone;
        QString note;
    };
    const QString licence_note =
        QStringLiteral(
            "This fixture comes from the plugin Protocol decoding. ") +
        licence_sentence() +
        QStringLiteral(" The port stays open and sends nothing.");
    const std::vector<row> rows = {
        {"org.example.noise", part_action::switch_on, "Switch on",
         part_tone::info,
         QStringLiteral("This fixture comes from the plugin Line noise, which "
                        "is switched off. The port stays open and sends "
                        "nothing.")},
        {"org.hobby.scanner", part_action::review_and_decide,
         "Review and decide", part_tone::warn,
         QStringLiteral("This fixture comes from the plugin Barcode scanner, "
                        "which is waiting for your decision. The port stays "
                        "open and sends nothing.")},
        {"org.example.blocked", part_action::show_in_plugins, "Show in Plugins",
         part_tone::info,
         QStringLiteral("This fixture comes from the plugin Blocked tool, "
                        "which your administrator has blocked. The port stays "
                        "open and sends nothing.")},
        {"com.virtualserialstudio.decode", part_action::licence, "Licence",
         part_tone::warn, licence_note},
        {"org.example.crashy", part_action::start_again, "Start again",
         part_tone::danger,
         QStringLiteral("The plugin Crashy has stopped, so this fixture sends "
                        "nothing.")},
        {"org.example.future", part_action::show_in_plugins, "Show in Plugins",
         part_tone::info,
         QStringLiteral("This fixture comes from the plugin Future plugin, "
                        "which cannot run in this version of Studio. The port "
                        "stays open and sends nothing.")},
        {"org.example.broken", part_action::show_in_plugins, "Show in Plugins",
         part_tone::info,
         QStringLiteral("This fixture comes from the plugin Broken plugin, "
                        "which cannot run in this version of Studio. The port "
                        "stays open and sends nothing.")},
    };
    for (const row& each : rows)
    {
        const plugin_view plugin = sample_of(each.id);
        const part_description text =
            describe_part(fixture_part(each.id), &plugin);
        REQUIRE(!text.running);
        REQUIRE(text.note_shown);
        REQUIRE_EQ(text.note, each.note);
        REQUIRE(text.action == each.action);
        REQUIRE_EQ(text.action_text, QString::fromLatin1(each.action_text));
        REQUIRE(text.tone == each.tone);
        REQUIRE_EQ(text.badge, QStringLiteral("Plugin not available"));
        REQUIRE_EQ(text.accessible_name,
                   QStringLiteral("meter-1, Meter, plugin not available"));
    }

    // A plugin that stopped but whose fixture has fixed replies.
    plugin_part with_replies = fixture_part("org.example.crashy");
    with_replies.fixed_replies = true;
    const plugin_view crashy = sample_of("org.example.crashy");
    const part_description kept = describe_part(with_replies, &crashy);
    REQUIRE_EQ(kept.note,
               QStringLiteral("The plugin Crashy has stopped. This fixture "
                              "still sends its fixed replies. Anything the "
                              "plugin worked out itself has stopped."));
    REQUIRE(kept.tone == part_tone::warn);

    // A plugin that is not installed: the name the setup recorded.
    plugin_part missing = fixture_part("org.example.gone");
    missing.plugin_name = QStringLiteral("Gone tool");
    const part_description gone = describe_part(missing, nullptr);
    REQUIRE_EQ(gone.note,
               QStringLiteral("This fixture needs the plugin Gone tool, which "
                              "is not installed on this computer. The port "
                              "stays open and sends nothing."));
    REQUIRE_EQ(gone.provided_by,
               QStringLiteral("Meter, from the plugin Gone tool"));
    REQUIRE(gone.action == part_action::show_in_plugins);

    // A plugin that runs says nothing.
    const plugin_view modbus = sample_of("com.virtualserialstudio.modbus");
    const part_description fine =
        describe_part(fixture_part("com.virtualserialstudio.modbus"), &modbus);
    REQUIRE(fine.running);
    REQUIRE(!fine.note_shown);
    REQUIRE(fine.badge.isEmpty());
    REQUIRE_EQ(fine.provided_by,
               QStringLiteral("Meter, from the plugin Modbus fixture "
                              "(Official)"));

    // A link behaviour and a generator.
    plugin_part behaviour = fixture_part("org.example.noise");
    behaviour.kind = part_kind::link_behaviour;
    const plugin_view noise = sample_of("org.example.noise");
    REQUIRE_EQ(describe_part(behaviour, &noise).note,
               QStringLiteral("The link behaviour Meter is not running, so "
                              "data on this link passes unchanged."));
    behaviour.drops_on_timeout = true;
    REQUIRE_EQ(describe_part(behaviour, &noise).note,
               QStringLiteral("The link behaviour Meter is not running, so "
                              "data on this link is being dropped."));
    plugin_part generator = fixture_part("org.example.noise");
    generator.kind = part_kind::generator;
    REQUIRE_EQ(describe_part(generator, &noise).note,
               QStringLiteral("The generator Meter is not running."));
}

TEST_CASE("plugin parts: the setup's ports are matched to their plugins, and "
          "the canvas and the Inspector show the note")
{
    FakeStudioBackend backend;
    const auto port = backend.create_virtual_port(
        QStringLiteral("meter-1"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());

    vss::config::document setup;
    vss::config::port_entry entry;
    entry.name = "meter-1";
    vss::config::plugin_binding binding;
    binding.id = "org.example.noise";
    binding.kind = "fixture";
    binding.type = "noise-device";
    binding.name = "Line noise";
    entry.plugins.push_back(binding);
    setup.ports.push_back(entry);
    // A port with no plugin binding is not mentioned.
    vss::config::port_entry plain;
    plain.name = "other";
    setup.ports.push_back(plain);

    plugin_snapshot snapshot = all_samples();
    const auto parts = describe_port_parts(backend.ports(), setup, snapshot);
    REQUIRE_EQ(parts.size(), 1);
    REQUIRE(parts.contains(*port));
    REQUIRE(!parts.value(*port).running);
    REQUIRE_EQ(parts.value(*port).badge,
               QStringLiteral("Plugin not available"));

    // The canvas: the node carries the badge and the accessible name.
    topology::TopologyView view;
    view.resize(800, 500);
    view.show();
    view.set_topology(backend.ports(), backend.links());
    settle();
    REQUIRE_EQ(view.node_badges(*port).size() > 0 ? 1 : 0, 1);
    view.set_plugin_parts(parts);
    settle();
    REQUIRE(view.node_badges(*port).contains(
        QStringLiteral("Plugin not available")));
    REQUIRE_EQ(view.node_accessible_name(*port),
               QStringLiteral("meter-1, noise-device, plugin not available"));
    view.set_plugin_parts({});
    settle();
    REQUIRE(!view.node_badges(*port).contains(
        QStringLiteral("Plugin not available")));
    REQUIRE_EQ(view.node_accessible_name(*port), QStringLiteral("meter-1"));

    // The Inspector: "Provided by", the state, the note, the settings line and
    // the actions.
    InspectorPanel inspector(backend);
    inspector.resize(360, 700);
    inspector.show();
    inspector.set_selected_port(*port);
    settle();
    REQUIRE(!inspector.plugin_block()->isVisibleTo(&inspector));
    inspector.set_plugin_parts(parts);
    settle();
    REQUIRE(inspector.plugin_block()->isVisibleTo(&inspector));
    REQUIRE_EQ(
        inspector.findChild<QLabel*>(QStringLiteral("inspectorPluginProvided"))
            ->text(),
        QStringLiteral("noise-device, from the plugin Line noise "
                       "(Signed by Example Instruments)"));
    REQUIRE_EQ(inspector.plugin_note()->text(),
               QStringLiteral("This fixture comes from the plugin Line noise, "
                              "which is switched off. The port stays open and "
                              "sends nothing."));
    REQUIRE_EQ(inspector.plugin_note()->action_button()->text(),
               QStringLiteral("Switch on"));
    REQUIRE_EQ(
        inspector.findChild<QLabel*>(QStringLiteral("inspectorPluginKept"))
            ->text(),
        QStringLiteral("These settings are kept. They apply again when "
                       "the plugin is available."));
    QString asked;
    int action = -1;
    QObject::connect(&inspector, &InspectorPanel::plugin_action_requested,
                     [&](const QString& id, int what)
                     {
                         asked = id;
                         action = what;
                     });
    inspector.plugin_note()->action_button()->click();
    REQUIRE_EQ(asked, QStringLiteral("org.example.noise"));
    REQUIRE_EQ(action, static_cast<int>(part_action::switch_on));
    inspector.plugin_show_button()->click();
    REQUIRE_EQ(action, static_cast<int>(part_action::show_in_plugins));
    // Plain text only.
    REQUIRE_EQ(inspector.plugin_note()->text().contains(QLatin1Char('<')),
               false);
}

// -- the chip in the status bar -----------------------------------------------

TEST_CASE("plugin chip: Recording or Reading data, named for a screen reader, "
          "and there only while a plugin reads")
{
    REQUIRE(!traffic_chip({}).shown);
    plugin_reader reading;
    reading.plugin_name = QStringLiteral("Wire tap");
    reading.port_count = 1;
    const traffic_chip_text one = traffic_chip({reading});
    REQUIRE(one.shown);
    REQUIRE_EQ(one.text, QStringLiteral("Reading data"));
    REQUIRE_EQ(one.accessible_name,
               QStringLiteral("Wire tap is reading data on 1 port. Open "
                              "Plugins."));
    reading.port_count = 3;
    REQUIRE_EQ(traffic_chip({reading}).accessible_name,
               QStringLiteral("Wire tap is reading data on 3 ports. Open "
                              "Plugins."));
    reading.recording = true;
    const traffic_chip_text recording = traffic_chip({reading});
    REQUIRE_EQ(recording.text, QStringLiteral("Recording"));
    REQUIRE(recording.recording);
    REQUIRE_EQ(recording.accessible_name,
               QStringLiteral("Wire tap is recording data on 3 ports. Open "
                              "Plugins."));
    plugin_reader other = reading;
    other.plugin_name = QStringLiteral("Logger");
    REQUIRE_EQ(traffic_chip({reading, other}).accessible_name,
               QStringLiteral("2 plugins are reading data. Open Plugins."));

    // In the window: the chip shows while a plugin reads, opens Plugins, and
    // goes when the reading stops. It cannot be dismissed.
    FakeStudioBackend backend;
    capabilities caps;
    caps.plugins = true;
    backend.set_capabilities(caps);
    backend.fake_plugins().set_plugins(fake_plugin_samples());
    backend.fake_plugins().flush();
    ShellWindow window(backend);
    window.show();
    settle();
    auto* bar = window.findChild<StatusBar*>();
    REQUIRE(bar != nullptr);
    REQUIRE(!bar->plugin_chip()->isVisibleTo(&window));
    plugin_snapshot snapshot = *backend.fake_plugins().snapshot();
    snapshot.readers = {reading};
    backend.fake_plugins().set_snapshot(snapshot);
    backend.fake_plugins().flush();
    settle();
    REQUIRE(bar->plugin_chip()->isVisibleTo(&window));
    REQUIRE_EQ(bar->plugin_chip()->chip()->text(), QStringLiteral("Recording"));
    REQUIRE_EQ(bar->plugin_chip()->accessibleName(),
               QStringLiteral("Wire tap is recording data on 3 ports. Open "
                              "Plugins."));
    REQUIRE(bar->plugin_chip()->minimumSizeHint().height() >= 24);
    snapshot.readers.clear();
    backend.fake_plugins().set_snapshot(snapshot);
    backend.fake_plugins().flush();
    settle();
    REQUIRE(!bar->plugin_chip()->isVisibleTo(&window));

    // On a bar of its own, pressing the chip asks for Plugins. (In the window
    // that opens a modal dialog.)
    StatusBar alone(backend);
    alone.set_plugin_backend(backend.plugin_backend());
    alone.show();
    snapshot.readers = {reading};
    backend.fake_plugins().set_snapshot(snapshot);
    backend.fake_plugins().flush();
    settle();
    REQUIRE(alone.plugin_chip()->isVisibleTo(&alone));
    int pressed = 0;
    QObject::connect(&alone, &StatusBar::plugins_requested, &alone,
                     [&pressed] { ++pressed; });
    alone.plugin_chip()->click();
    REQUIRE_EQ(pressed, 1);
}

// -- notices ------------------------------------------------------------------

TEST_CASE("plugin notices: what follows from a change, in the words of the "
          "spec")
{
    plugin_snapshot before;
    before.plugins = {sample_of("com.virtualserialstudio.modbus"),
                      sample_of("org.example.replay"),
                      sample_of("org.example.crashy")};
    before.plugins[2].state = plugin_state::available;
    plugin_snapshot after = before;
    // A new plugin, a plugin whose files changed, one that stopped while the
    // setup used it, one that was withdrawn while it ran.
    plugin_view fresh = sample_of("org.hobby.scanner");
    after.plugins.push_back(fresh);
    after.plugins[0].state = plugin_state::cannot_be_used;
    after.plugins[0].note = plugin_note::withdrawn;
    after.plugins[1].state = plugin_state::needs_decision;
    after.plugins[1].note = plugin_note::decision_changed;
    after.plugins[1].hash = QString(128, QLatin1Char('f'));
    after.plugins[2].state = plugin_state::stopped;

    const auto notices =
        plugin_notices(before, after, {QStringLiteral("org.example.crashy")});
    REQUIRE_EQ(notices.size(), std::size_t{4});
    const auto find = [&](plugin_notice_kind kind)
    {
        for (const plugin_notice_text& each : notices)
        {
            if (each.kind == kind)
                return each;
        }
        return plugin_notice_text{};
    };
    REQUIRE_EQ(find(plugin_notice_kind::new_plugin).text,
               QStringLiteral("A new plugin was found: Barcode scanner. It "
                              "will not run until you decide."));
    REQUIRE(!find(plugin_notice_kind::new_plugin).warn);
    REQUIRE_EQ(find(plugin_notice_kind::new_plugin).action,
               QStringLiteral("Review and decide"));
    REQUIRE_EQ(find(plugin_notice_kind::changed).text,
               QStringLiteral("Replay changed since you approved it and has "
                              "been stopped."));
    REQUIRE(find(plugin_notice_kind::changed).warn);
    REQUIRE_EQ(find(plugin_notice_kind::stopped).text,
               QStringLiteral("The plugin Crashy has stopped. Parts of this "
                              "setup that use it are not running."));
    REQUIRE_EQ(find(plugin_notice_kind::stopped).second,
               QStringLiteral("Start again"));
    REQUIRE_EQ(find(plugin_notice_kind::stopped).action,
               QStringLiteral("Plugins"));
    REQUIRE_EQ(find(plugin_notice_kind::withdrawn).text,
               QStringLiteral("Modbus fixture was withdrawn by Virtual Serial "
                              "Studio and has been stopped."));

    // A plugin the setup does not use that stops is not worth a notice, and a
    // change that did not alter anything says nothing.
    REQUIRE(plugin_notices(before, before, {}).empty());
    REQUIRE(plugin_notices(before, after, {}).size() == 3);
}

TEST_CASE("plugin notice: one at a time, never modal, never taking focus, "
          "plain text, and the first snapshot says nothing")
{
    FakePluginBackend host;
    host.set_plugins(fake_plugin_samples());
    host.flush();
    QWidget parent;
    PluginNotice notice(&parent);
    parent.resize(800, 100);
    parent.show();
    notice.set_backend(&host);
    // What was there when the window opened is the baseline.
    host.set_plugins(fake_plugin_samples());
    host.flush();
    settle();
    REQUIRE(!notice.shown().has_value());
    REQUIRE(!notice.isVisible());

    // Two new plugins at once: one shows, the other waits.
    std::vector<plugin_view> more = fake_plugin_samples();
    plugin_view first = sample_of("org.hobby.scanner");
    first.id = QStringLiteral("org.new.one");
    first.folder = QStringLiteral("C:/x/one");
    first.name = QStringLiteral("One <b>bold</b>");
    plugin_view second = first;
    second.id = QStringLiteral("org.new.two");
    second.folder = QStringLiteral("C:/x/two");
    second.name = QStringLiteral("Two");
    more.push_back(first);
    more.push_back(second);
    QWidget* before_focus = QApplication::focusWidget();
    host.set_plugins(more);
    host.flush();
    settle();
    REQUIRE(notice.isVisible());
    REQUIRE(notice.shown().has_value());
    REQUIRE_EQ(notice.text(),
               QStringLiteral("A new plugin was found: One <b>bold</b>. It "
                              "will not run until you decide."));
    REQUIRE_EQ(notice.waiting(), 1);
    // It never takes the focus, and what the plugin is called is not markup.
    REQUIRE(QApplication::focusWidget() == before_focus);
    REQUIRE(!notice.hasFocus());
    REQUIRE(notice.get_kind() == NoteBanner::kind::info);
    REQUIRE_EQ(notice.action_button()->text(),
               QStringLiteral("Review and decide"));
    REQUIRE(!notice.second_button()->isVisibleTo(&parent));
    REQUIRE_EQ(notice.dismiss_button()->text(), QStringLiteral("Dismiss"));

    QString decided;
    QObject::connect(&notice, &PluginNotice::decision_requested, &notice,
                     [&decided](const QString& id, const QString& folder)
                     { decided = id + QLatin1Char('|') + folder; });
    notice.action_button()->click();
    REQUIRE_EQ(decided, QStringLiteral("org.new.one|C:/x/one"));

    // Dismiss shows the next; dismissing that hides the banner.
    notice.dismiss_button()->click();
    REQUIRE(notice.isVisible());
    REQUIRE_EQ(notice.text(),
               QStringLiteral("A new plugin was found: Two. It will not run "
                              "until you decide."));
    notice.dismiss_button()->click();
    REQUIRE(!notice.isVisible());

    // A plugin that stopped: Plugins and Start again.
    std::vector<plugin_view> stopped = more;
    for (plugin_view& each : stopped)
    {
        if (each.id == QStringLiteral("org.example.replay"))
        {
            each.state = plugin_state::stopped;
            each.used_in = {
                {QStringLiteral("Replay on COM3"), QStringLiteral("port:3")}};
        }
    }
    host.set_plugins(stopped);
    host.flush();
    settle();
    REQUIRE(notice.isVisible());
    REQUIRE(notice.get_kind() == NoteBanner::kind::warn);
    REQUIRE(notice.second_button()->isVisibleTo(&parent));
    QString again;
    QObject::connect(&notice, &PluginNotice::start_again_requested, &notice,
                     [&again](const QString& id) { again = id; });
    notice.second_button()->click();
    REQUIRE_EQ(again, QStringLiteral("org.example.replay"));
}

// -- the Licence dialog -------------------------------------------------------

TEST_CASE("licence dialog: the row of the official plugins links to Plugins")
{
    FakeStudioBackend backend;
    LicenceDialog dialog(backend);
    dialog.show();
    settle();
    REQUIRE(dialog.plugins_link() != nullptr);
    REQUIRE(dialog.plugins_link()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.plugins_link()->text(), QStringLiteral("Plugins"));
    REQUIRE_EQ(dialog.plugins_link()->accessibleName(),
               QStringLiteral("Plugins"));
    REQUIRE(dialog.plugins_link()->height() >= 24);
    int asked = 0;
    QObject::connect(&dialog, &LicenceDialog::plugins_requested, &dialog,
                     [&asked] { ++asked; });
    dialog.plugins_link()->click();
    REQUIRE_EQ(asked, 1);
}

// -- the window ---------------------------------------------------------------

TEST_CASE("plugin surfaces: the window shows the toolbox group, and without a "
          "host shows none of it")
{
    for (const bool with_host : {false, true})
    {
        FakeStudioBackend backend;
        capabilities caps;
        caps.plugins = with_host;
        backend.set_capabilities(caps);
        backend.fake_plugins().set_plugins(fake_plugin_samples());
        backend.fake_plugins().flush();
        ShellWindow window(backend);
        window.show();
        settle();
        auto* toolbox = window.findChild<topology::Toolbox*>(
            QStringLiteral("canvasToolbox"));
        REQUIRE(toolbox != nullptr);
        auto* bar = window.findChild<StatusBar*>();
        REQUIRE(bar != nullptr);
        REQUIRE_EQ(toolbox->has_plugin_group(), with_host);
        REQUIRE(!bar->plugin_chip()->isVisibleTo(&window));
        REQUIRE(window.plugin_notice() != nullptr);
        REQUIRE(!window.plugin_notice()->isVisibleTo(&window));
        if (with_host)
        {
            // A decision that switches the Modbus plugin off takes its tile
            // away at once, and on brings it back.
            REQUIRE_EQ(toolbox->plugin_tile_labels().size(), 4);
            backend.fake_plugins().change(
                QStringLiteral("com.virtualserialstudio.modbus"),
                [](plugin_view& each) { each.state = plugin_state::off; });
            backend.fake_plugins().flush();
            settle();
            REQUIRE_EQ(toolbox->plugin_tile_labels().size(), 3);
            backend.fake_plugins().change(
                QStringLiteral("com.virtualserialstudio.modbus"),
                [](plugin_view& each)
                { each.state = plugin_state::available; });
            backend.fake_plugins().flush();
            settle();
            REQUIRE_EQ(toolbox->plugin_tile_labels().size(), 4);
        }
    }
}
