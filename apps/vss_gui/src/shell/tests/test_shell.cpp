//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/share_wizard.hpp"
#include "dialogs/templates_dialog.hpp"
#include "panes/ports_panel.hpp"
#include "panes/selection_service.hpp"
#include "shell/action_registry.hpp"
#include "shell/command_palette.hpp"
#include "shell/format.hpp"
#include "shell/launcher.hpp"
#include "shell/level_service.hpp"
#include "shell/port_warnings.hpp"
#include "shell/setup_group.hpp"
#include "shell/shell_window.hpp"
#include "shell/single_instance.hpp"
#include "shell/studio_tray.hpp"
#include "shell/undo_service.hpp"
#include "shell/undo_steps.hpp"
#include "simple/simple_page.hpp"
#include "support/account_id.hpp"
#include "support/running_setup.hpp"
#include "support/setup_facts.hpp"

#include "theme/theme.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"
#include "widgets/controls.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <cmath>

namespace
{

void clear_settings()
{
    QSettings settings;
    settings.clear();
}

} // namespace

TEST_CASE("actions: levels, search and shortcut uniqueness")
{
    vss::gui::ActionRegistry registry;
    bool simple_ran = false;
    bool advanced_ran = false;
    registry.add({QStringLiteral("simple.action"),
                  QStringLiteral("Simple"),
                  QStringLiteral("plain"),
                  {},
                  {},
                  vss::gui::action_level::simple,
                  false,
                  [&simple_ran] { simple_ran = true; },
                  {}});
    registry.add({QStringLiteral("advanced.action"),
                  QStringLiteral("Inject a fault"),
                  QStringLiteral("fault parity framing"),
                  {},
                  {},
                  vss::gui::action_level::advanced,
                  false,
                  [&advanced_ran] { advanced_ran = true; },
                  {}});

    REQUIRE(registry.action(QStringLiteral("simple.action")) != nullptr);
    REQUIRE(registry.action(QStringLiteral("advanced.action")) != nullptr);
    REQUIRE(registry.action(QStringLiteral("missing")) == nullptr);
    REQUIRE(registry.shortcuts_unique());

    registry.set_level(vss::gui::action_level::simple);
    REQUIRE(registry.action(QStringLiteral("simple.action"))->isVisible());
    REQUIRE(!registry.action(QStringLiteral("advanced.action"))->isVisible());
    const auto matches = registry.search(QStringLiteral("fault"));
    REQUIRE_EQ(matches.size(), 1);
    REQUIRE_EQ(matches.first()->data().toString(),
               QStringLiteral("advanced.action"));

    registry.set_level(vss::gui::action_level::advanced);
    REQUIRE(registry.action(QStringLiteral("advanced.action"))->isVisible());

    registry.action(QStringLiteral("simple.action"))->trigger();
    registry.action(QStringLiteral("advanced.action"))->trigger();
    REQUIRE(simple_ran);
    REQUIRE(advanced_ran);

    registry.add({QStringLiteral("duplicate.action"),
                  QStringLiteral("Duplicate"),
                  {},
                  {},
                  registry.action(QStringLiteral("simple.action"))->shortcut(),
                  vss::gui::action_level::simple,
                  false,
                  {},
                  {}});
    // Both shortcuts are empty here; an explicit duplicate proves the check.
    registry.action(QStringLiteral("duplicate.action"))
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+9")));
    registry.action(QStringLiteral("simple.action"))
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+9")));
    REQUIRE(!registry.shortcuts_unique());
    REQUIRE_EQ(registry.duplicate_shortcuts().size(), 1);
}

TEST_CASE("palette: groups hidden actions and turns Advanced on")
{
    clear_settings();
    vss::gui::ActionRegistry registry;
    vss::gui::LevelService level;
    bool advanced_ran = false;
    registry.add({QStringLiteral("add"),
                  QStringLiteral("Add port"),
                  QStringLiteral("new"),
                  {},
                  {},
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("fault"),
                  QStringLiteral("Inject a fault"),
                  QStringLiteral("parity framing"),
                  {},
                  {},
                  vss::gui::action_level::advanced,
                  false,
                  [&advanced_ran] { advanced_ran = true; },
                  {}});
    registry.set_level(level.level());

    vss::gui::CommandPalette palette(registry, level);
    palette.set_query(QStringLiteral("fault"));
    REQUIRE_EQ(palette.available_count(), 0);
    REQUIRE_EQ(palette.hidden_count(), 1);
    QAction* selected = palette.selected_action();
    REQUIRE(selected != nullptr);
    REQUIRE_EQ(selected->data().toString(), QStringLiteral("fault"));

    palette.activate_current();
    REQUIRE(level.advanced());
    REQUIRE(advanced_ran);
}

TEST_CASE("shell: launcher, workspace, level and layout")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    auto* simple = window.findChild<vss::gui::simple::SimplePage*>();
    auto* launcher = window.findChild<vss::gui::Launcher*>();
    auto* ports_dock =
        window.findChild<QDockWidget*>(QStringLiteral("portsDock"));
    auto* inspector_dock =
        window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"));
    auto* monitor_dock =
        window.findChild<QDockWidget*>(QStringLiteral("monitorDock"));
    REQUIRE(simple != nullptr);
    REQUIRE(launcher != nullptr);
    REQUIRE(ports_dock != nullptr);
    REQUIRE(inspector_dock != nullptr);
    REQUIRE(monitor_dock != nullptr);
    // Simple mode is the setups page, with or without ports.
    REQUIRE(simple->isVisible());
    REQUIRE(!launcher->isVisible());
    REQUIRE(!ports_dock->isVisible());
    REQUIRE(!window.windowTitle().isEmpty());

    const auto port = backend.create_virtual_port(
        QStringLiteral("terminal"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();
    REQUIRE(simple->isVisible());
    REQUIRE(!ports_dock->isVisible());

    QAction* advanced =
        window.findChild<QAction*>(QStringLiteral("view.advanced"));
    QAction* mock = window.findChild<QAction*>(QStringLiteral("ports.mock"));
    REQUIRE(advanced != nullptr);
    REQUIRE(mock != nullptr);
    REQUIRE(!mock->isVisible());
    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(advanced->isChecked());
    REQUIRE(mock->isVisible());
    // Advanced mode is the dock grid.
    REQUIRE(!simple->isVisible());
    REQUIRE(ports_dock->isVisible());
    REQUIRE(inspector_dock->isVisible());
    REQUIRE(monitor_dock->isVisible());

    const QByteArray state = window.saveState();
    REQUIRE(!state.isEmpty());

    backend.remove_port(*port);
    QApplication::processEvents();
    // Advanced mode builds on the canvas: an empty setup is the empty canvas.
    REQUIRE(!launcher->isVisible());
    REQUIRE(!simple->isVisible());
    REQUIRE(window.run_selftest());
    // Simple mode is the setups page again, never the launcher.
    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(simple->isVisible());
    REQUIRE(!launcher->isVisible());
    REQUIRE(!ports_dock->isVisible());
    REQUIRE(window.run_selftest());

    window.close();
    QApplication::processEvents();
    QSettings settings;
    REQUIRE(
        !settings.value(QStringLiteral("ui/geometry")).toByteArray().isEmpty());
    REQUIRE(!settings.value(QStringLiteral("ui/windowState"))
                 .toByteArray()
                 .isEmpty());
}

TEST_CASE("levels: the Advanced choice survives a restart")
{
    clear_settings();
    vss::gui::LevelService first;
    REQUIRE(!first.advanced());
    first.set_advanced(true);
    vss::gui::LevelService second;
    REQUIRE(second.advanced());
    second.set_advanced(false);
    vss::gui::LevelService third;
    REQUIRE(!third.advanced());
    clear_settings();
}

TEST_CASE("palette: renders a review screenshot in both themes")
{
    clear_settings();
    vss::gui::ActionRegistry registry;
    vss::gui::LevelService level;
    registry.add({QStringLiteral("add"),
                  QStringLiteral("Add port"),
                  QStringLiteral("new virtual"),
                  QStringLiteral("add"),
                  QKeySequence(QStringLiteral("Ctrl+Shift+N")),
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("connect"),
                  QStringLiteral("Connect ports"),
                  QStringLiteral("link"),
                  QStringLiteral("connect"),
                  QKeySequence(QStringLiteral("Ctrl+L")),
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("fault"),
                  QStringLiteral("Inject a fault"),
                  QStringLiteral("parity framing overrun"),
                  QStringLiteral("fault"),
                  {},
                  vss::gui::action_level::advanced,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("mock"),
                  QStringLiteral("Add mock device"),
                  QStringLiteral("fixture"),
                  QStringLiteral("fixture"),
                  {},
                  vss::gui::action_level::advanced,
                  false,
                  [] {},
                  {}});
    registry.set_level(level.level());

    const QString directory = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("../gui-shots"));
    REQUIRE(QDir().mkpath(directory));

    vss::gui::CommandPalette palette(registry, level);
    palette.resize(760, 520);
    palette.show();
    QApplication::processEvents();
    palette.set_query(QStringLiteral("fault"));
    QApplication::processEvents();
    REQUIRE(palette.grab().save(
        QDir(directory).filePath(QStringLiteral("palette-light.png"))));

    vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::dark);
    QApplication::processEvents();
    REQUIRE(palette.grab().save(
        QDir(directory).filePath(QStringLiteral("palette-dark.png"))));
    vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::light);
    palette.close();
}

TEST_CASE("shell: the search and the level switch are centred in the header")
{
    clear_settings();
    const QFont original = QApplication::font();
    const QString directory = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("../gui-shots"));
    REQUIRE(QDir().mkpath(directory));
    for (const double scale : {1.0, 1.5, 2.0})
    {
        QFont font = original;
        font.setPointSizeF(original.pointSizeF() * scale);
        QApplication::setFont(font);
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        window.resize(1280, 800);
        window.show();
        QApplication::processEvents();

        QMenuBar* bar = window.menuBar();
        QWidget* corner = bar->cornerWidget(Qt::TopRightCorner);
        REQUIRE(corner != nullptr);
        auto* search = corner->findChild<QLineEdit*>();
        auto* level = corner->findChild<vss::gui::SegmentedControl*>();
        REQUIRE(search != nullptr);
        REQUIRE(level != nullptr);
        if (scale == 1.0)
        {
            for (const auto mode : {vss::gui::theme_mode::light,
                                    vss::gui::theme_mode::dark})
            {
                vss::gui::Theme::instance().set_mode(mode);
                QApplication::processEvents();
                const bool dark = mode == vss::gui::theme_mode::dark;
                REQUIRE(bar->grab().save(QDir(directory).filePath(
                    dark ? QStringLiteral("header-dark.png")
                         : QStringLiteral("header-light.png"))));
            }
            vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::light);
        }
        for (QWidget* child : {static_cast<QWidget*>(search),
                               static_cast<QWidget*>(level)})
        {
            const QRect box(child->mapTo(bar, QPoint(0, 0)), child->size());
            REQUIRE(box.top() > 0);
            REQUIRE(box.bottom() < bar->height() - 1);
            REQUIRE(box.height() >= 24);
            // Centred to within a pixel of rounding.
            REQUIRE(std::abs(box.top() - (bar->height() - 1 - box.bottom())) <=
                    1);
            REQUIRE(box.height() < bar->height());
        }
        window.close();
    }
    QApplication::setFont(original);
}

TEST_CASE("shell: every toolbar action has a name and a unique shortcut")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();

    const auto buttons = window.findChildren<QToolButton*>();
    REQUIRE(!buttons.isEmpty());
    for (const QToolButton* button : buttons)
    {
        if (!button->isVisibleTo(&window))
            continue;
        REQUIRE(!button->accessibleName().isEmpty() ||
                !button->text().isEmpty());
        REQUIRE(button->height() >= 24 || button->sizeHint().height() >= 24);
    }

    QStringList shortcuts;
    for (QAction* action : window.findChildren<QAction*>())
    {
        const QString key =
            action->shortcut().toString(QKeySequence::NativeText);
        if (key.isEmpty())
            continue;
        REQUIRE(!shortcuts.contains(key));
        shortcuts << key;
    }
    REQUIRE(shortcuts.size() > 8);
}

TEST_CASE("shell: undo and redo are capability gated")
{
    clear_settings();
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        QAction* undo = window.findChild<QAction*>(QStringLiteral("edit.undo"));
        QAction* redo = window.findChild<QAction*>(QStringLiteral("edit.redo"));
        REQUIRE(undo != nullptr);
        REQUIRE(redo != nullptr);
        REQUIRE(!undo->isEnabled());
        REQUIRE(!redo->isEnabled());
        REQUIRE(!undo->toolTip().isEmpty());
    }

    vss::gui::capabilities caps;
    caps.undo = true;
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(caps);
    vss::gui::ShellWindow window(backend);
    QAction* undo = window.findChild<QAction*>(QStringLiteral("edit.undo"));
    QAction* redo = window.findChild<QAction*>(QStringLiteral("edit.redo"));
    REQUIRE(undo != nullptr);
    // Supported, but there is no change to reverse yet.
    REQUIRE(!undo->isEnabled());
    REQUIRE(!redo->isEnabled());
    REQUIRE_EQ(undo->text(), QStringLiteral("Undo"));
    REQUIRE(undo->toolTip() != QStringLiteral("Not available in this build"));
}

TEST_CASE("undo: the history reverses and repeats steps in order")
{
    vss::gui::UndoService history;
    int changes = 0;
    int value = 0;
    QObject::connect(&history, &vss::gui::UndoService::changed,
                     [&changes] { ++changes; });
    REQUIRE(!history.can_undo());
    REQUIRE(!history.undo());
    REQUIRE(!history.redo());

    const auto step = [&value](const QString& text, int amount)
    {
        return vss::gui::undo_step{text,
                                   [&value, amount]
                                   {
                                       value -= amount;
                                       return true;
                                   },
                                   [&value, amount]
                                   {
                                       value += amount;
                                       return true;
                                   }};
    };
    value += 1;
    history.push(step(QStringLiteral("one"), 1));
    value += 10;
    history.push(step(QStringLiteral("ten"), 10));
    REQUIRE_EQ(value, 11);
    REQUIRE_EQ(history.undo_text(), QStringLiteral("ten"));

    REQUIRE(history.undo());
    REQUIRE_EQ(value, 1);
    REQUIRE_EQ(history.redo_text(), QStringLiteral("ten"));
    REQUIRE_EQ(history.undo_text(), QStringLiteral("one"));
    REQUIRE(history.redo());
    REQUIRE_EQ(value, 11);

    // A new change drops what could have been redone.
    REQUIRE(history.undo());
    value += 100;
    history.push(step(QStringLiteral("hundred"), 100));
    REQUIRE(!history.can_redo());
    REQUIRE(history.redo_text().isEmpty());
    REQUIRE(changes > 0);
}

TEST_CASE("undo: a step that cannot be applied clears the history")
{
    vss::gui::UndoService history;
    history.push(
        {QStringLiteral("first"), [] { return true; }, [] { return true; }});
    history.push(
        {QStringLiteral("second"), [] { return false; }, [] { return true; }});
    REQUIRE(!history.undo());
    // The step after the failure assumed it, so nothing is left.
    REQUIRE(!history.can_undo());
    REQUIRE(!history.can_redo());

    for (std::size_t count = 0; count < vss::gui::UndoService::kMaxDepth + 5;
         ++count)
        history.push(
            {QStringLiteral("step"), [] { return true; }, [] { return true; }});
    std::size_t undone = 0;
    while (history.undo())
        ++undone;
    REQUIRE_EQ(undone, vss::gui::UndoService::kMaxDepth);
}

TEST_CASE("undo: link steps use exact inverse operations")
{
    vss::gui::FakeStudioBackend backend;
    const auto first = backend.create_auto_port(QStringLiteral("a"), true);
    const auto second = backend.create_auto_port(QStringLiteral("b"), true);
    const auto third = backend.create_auto_port(QStringLiteral("c"), true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    vss::gui::UndoService history;
    const auto linked = backend.connect_ports(*first, *second, {});
    REQUIRE(linked.has_value());
    const auto made = backend.link(*linked);
    REQUIRE(made.has_value());
    history.push(
        vss::gui::connect_step(backend, *made, QStringLiteral("Connect")));

    REQUIRE(history.undo());
    REQUIRE(backend.links().empty());
    REQUIRE(history.redo());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(vss::gui::find_link(backend, *made).has_value());

    // Removing a link and undoing it makes the same link again, with its
    // options.
    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    options.emulate_modem_lines = false;
    const auto joined = backend.join({*first, *second}, *third, options);
    REQUIRE(joined.has_value());
    const auto join_view = backend.link(*joined);
    REQUIRE(join_view.has_value());
    REQUIRE(backend.set_owner(*joined, *second).has_value());
    const auto owned = backend.link(*joined);
    REQUIRE(owned.has_value());

    history.clear();
    REQUIRE(backend.disconnect(*joined).has_value());
    history.push(vss::gui::remove_link_step(backend, *owned,
                                            QStringLiteral("Remove link")));
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(history.undo());
    REQUIRE_EQ(backend.links().size(), std::size_t{2});
    const auto again = vss::gui::find_link(backend, *owned);
    REQUIRE(again.has_value());
    REQUIRE_EQ(again->owner, *second);
    REQUIRE(!again->options.emulate_modem_lines);
    REQUIRE(again->options.policy == vss::merge_policy::owner);

    // An owner change is reversed and repeated on whatever link has these
    // ports now.
    history.clear();
    history.push(vss::gui::owner_step(backend, *again, *second, *first,
                                      QStringLiteral("Change owner")));
    REQUIRE(backend.set_owner(again->id, *first).has_value());
    REQUIRE(history.undo());
    REQUIRE_EQ(vss_test::require_value(vss::gui::find_link(backend, *again)).owner, *second);
    REQUIRE(history.redo());
    REQUIRE_EQ(vss_test::require_value(vss::gui::find_link(backend, *again)).owner, *first);

    // A rename is reversed by naming the port as it was.
    history.clear();
    REQUIRE(backend.rename_port(*first, QStringLiteral("renamed")).has_value());
    history.push(vss::gui::rename_step(backend, *first, QStringLiteral("a"),
                                       QStringLiteral("renamed"),
                                       QStringLiteral("Rename port")));
    REQUIRE(history.undo());
    REQUIRE_EQ(vss_test::require_value(backend.port(*first)).name, QStringLiteral("a"));
    REQUIRE(history.redo());
    REQUIRE_EQ(vss_test::require_value(backend.port(*first)).name,
               QStringLiteral("renamed"));
    REQUIRE(history.undo());
    history.clear();
    history.push(vss::gui::owner_step(backend, *again, *second, *first,
                                      QStringLiteral("Change owner")));
    REQUIRE(backend.set_owner(again->id, *first).has_value());

    // When the link has gone, the step says so instead of guessing.
    REQUIRE(backend.disconnect(
                       vss_test::require_value(vss::gui::find_link(backend, *again)).id)
                .has_value());
    REQUIRE(!history.undo());
    REQUIRE(!history.can_undo());
}

TEST_CASE("shell: the shortcuts dialog is generated from the registry")
{
    vss::gui::ActionRegistry registry;
    registry.add({QStringLiteral("file.new"),
                  QStringLiteral("New"),
                  QStringLiteral("document"),
                  {},
                  QKeySequence::New,
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("file.save"),
                  QStringLiteral("Save"),
                  QStringLiteral("file"),
                  {},
                  QKeySequence::Save,
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});
    registry.add({QStringLiteral("edit.find"),
                  QStringLiteral("Find"),
                  QStringLiteral("search"),
                  {},
                  {},
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {}});

    const auto entries = vss::gui::shortcut_entries(registry);
    REQUIRE_EQ(entries.size(), 2);
    REQUIRE_EQ(entries.at(0).action, QStringLiteral("New"));
    REQUIRE_EQ(entries.at(0).shortcut, QKeySequence::New);
    REQUIRE_EQ(entries.at(1).action, QStringLiteral("Save"));

    vss::gui::support::ShortcutsDialog dialog(entries);
    REQUIRE_EQ(dialog.total_row_count(), entries.size());
    QStringList expected;
    for (const auto& entry : entries)
        expected << entry.action;
    REQUIRE_EQ(dialog.action_texts(), expected);
}

TEST_CASE("shell: the unsaved prompt follows ui/askOnDiscard")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();

    const auto port = backend.create_virtual_port(
        QStringLiteral("terminal"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();
    REQUIRE(window.windowTitle().contains(QLatin1Char('*')));

    // With the setting off, close must not open the dialog.
    window.close();
    QApplication::processEvents();
    REQUIRE(!window.isVisible());
    clear_settings();
}

TEST_CASE("shell: the status bar readiness comes from the shared facts")
{
    clear_settings();
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = true;
    facts.serial_devices = 0;

    {
        vss::gui::FakeStudioBackend backend;
        backend.set_facts(facts);
        vss::gui::ShellWindow window(backend);
        window.show();
        QApplication::processEvents();
        bool ready = false;
        for (QLabel* label : window.findChildren<QLabel*>())
        {
            if (label->text() == QStringLiteral("Setup ready"))
                ready = true;
        }
        REQUIRE(ready);
    }

    facts.com0com_found = false;
    {
        vss::gui::FakeStudioBackend backend;
        backend.set_facts(facts);
        vss::gui::ShellWindow window(backend);
        window.show();
        QApplication::processEvents();
        bool attention = false;
        for (QLabel* label : window.findChildren<QLabel*>())
        {
            if (label->text() == QStringLiteral("Setup needs attention"))
                attention = true;
        }
        REQUIRE(attention);
    }

    // The Studio driver on its own counts as ready.
    facts.driver_found = true;
    facts.driver_version = QStringLiteral("2");
    {
        vss::gui::FakeStudioBackend backend;
        backend.set_facts(facts);
        vss::gui::ShellWindow window(backend);
        window.show();
        QApplication::processEvents();
        bool ready = false;
        for (QLabel* label : window.findChildren<QLabel*>())
        {
            if (label->text() == QStringLiteral("Setup ready"))
                ready = true;
        }
        REQUIRE(ready);
        // The launcher names the driver it found; it reads the host platform.
        if (vss::gui::support::host_platform() == QLatin1String("windows"))
        {
            bool names_driver = false;
            for (QLabel* label : window.findChildren<QLabel*>())
            {
                if (label->text().contains(QStringLiteral(
                        "Virtual Serial Studio driver detected")))
                    names_driver = true;
            }
            REQUIRE(names_driver);
        }
    }
    clear_settings();
}

namespace
{

template <class Dialog>
bool trigger_and_close(vss::gui::ShellWindow& window, const QString& action_id)
{
    auto* action = window.findChild<QAction*>(action_id);
    REQUIRE(action != nullptr);
    REQUIRE(action->isEnabled());
    bool found = false;
    QTimer::singleShot(0, &window,
                       [&found]
                       {
                           for (QWidget* widget :
                                QApplication::topLevelWidgets())
                           {
                               if (auto* dialog = qobject_cast<Dialog*>(widget))
                               {
                                   found = true;
                                   dialog->reject();
                               }
                           }
                       });
    action->trigger();
    return found;
}

} // namespace

TEST_CASE("shell: WP6 actions open the real dialogs")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    backend.set_facts(facts);
    vss::gui::device_view device;
    device.path = QStringLiteral("COM3");
    device.description = QStringLiteral("Serial device");
    backend.set_devices({device});
    for (const char* name : {"alpha", "beta", "gamma"})
    {
        const auto created = backend.create_virtual_port(
            QString::fromLatin1(name), vss::virtual_port_kind::pty, true);
        REQUIRE(created.has_value());
    }
    backend.refresh();

    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    REQUIRE(trigger_and_close<vss::gui::ConnectPortsDialog>(
        window, QStringLiteral("topology.connect")));
    REQUIRE(trigger_and_close<vss::gui::ShareWizard>(
        window, QStringLiteral("topology.share")));
    REQUIRE(trigger_and_close<vss::gui::TemplatesDialog>(
        window, QStringLiteral("file.templates")));

    auto* advanced =
        window.findChild<QAction*>(QStringLiteral("view.advanced"));
    auto* share_toolbar =
        window.findChild<QAction*>(QStringLiteral("topology.shareToolbar"));
    auto* network = window.findChild<QAction*>(QStringLiteral("ports.network"));
    REQUIRE(advanced != nullptr);
    REQUIRE(share_toolbar != nullptr);
    REQUIRE(network != nullptr);
    REQUIRE(!share_toolbar->isVisible());
    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(share_toolbar->isVisible());
    REQUIRE(trigger_and_close<vss::gui::NetworkEndpointDialog>(
        window, QStringLiteral("ports.network")));

    // The toolbar-only duplicate must not double an entry in the palette.
    vss::gui::ActionRegistry registry;
    registry.add({QStringLiteral("menu.share"),
                  QStringLiteral("Share a device"),
                  QStringLiteral("share"),
                  {},
                  {},
                  vss::gui::action_level::simple,
                  false,
                  [] {},
                  {},
                  true});
    registry.add({QStringLiteral("toolbar.share"),
                  QStringLiteral("Share a device"),
                  QStringLiteral("share"),
                  {},
                  {},
                  vss::gui::action_level::advanced,
                  false,
                  [] {},
                  {},
                  false});
    REQUIRE_EQ(registry.search(QStringLiteral("share")).size(), 1);
    REQUIRE_EQ(
        registry.search(QStringLiteral("share")).front()->data().toString(),
        QStringLiteral("menu.share"));
    REQUIRE(registry.action(QStringLiteral("toolbar.share")) != nullptr);

    window.close();
    QApplication::processEvents();
}

namespace
{

/// What the next confirmation dialog said, and which button was pressed.
struct confirm_seen
{
    bool shown = false;
    QString title;
    QString text;
};

/// Presses the button called `answer` in the next ConfirmDialog that opens,
/// looking again until one does.
void answer_next_confirm(const QString& answer, confirm_seen* seen,
                         int remaining_ms = 30000)
{
    QTimer::singleShot(
        10,
        [answer, seen, remaining_ms]
        {
            vss::gui::ConfirmDialog* dialog = nullptr;
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                if (auto* found =
                        qobject_cast<vss::gui::ConfirmDialog*>(widget);
                    found != nullptr && found->isVisible())
                    dialog = found;
            }
            if (dialog == nullptr)
            {
                if (remaining_ms > 10)
                    answer_next_confirm(answer, seen, remaining_ms - 10);
                return;
            }
            seen->shown = true;
            seen->title = dialog->windowTitle();
            for (QLabel* label : dialog->findChildren<QLabel*>())
                seen->text += label->text() + QLatin1Char('\n');
            for (QPushButton* button : dialog->findChildren<QPushButton*>())
            {
                if (button->text() == answer)
                {
                    button->click();
                    return;
                }
            }
            dialog->reject();
        });
}

vss::endpoint_id held_driver_port(vss::gui::FakeStudioBackend& backend,
                                  const QString& name, unsigned com_number)
{
    vss::gui::capabilities caps;
    caps.peer_open = true;
    caps.com_numbers = true;
    caps.release_port = true;
    backend.set_capabilities(caps);
    const auto id = backend.create_driver_port(name, true, com_number);
    REQUIRE(id.has_value());
    backend.set_peer_state(*id, vss::gui::peer_state::open);
    return *id;
}

} // namespace

TEST_CASE("warnings: only a port a program is known to hold is warned about")
{
    using namespace vss::gui;
    port_view held;
    held.id = 1;
    held.name = QStringLiteral("meter");
    held.path = QStringLiteral("COM7");
    held.open = true;
    held.peer = peer_state::open;
    port_view waiting = held;
    waiting.id = 2;
    waiting.path = QStringLiteral("COM8");
    waiting.peer = peer_state::not_open;
    port_view unknown = held;
    unknown.id = 3;
    unknown.path = QStringLiteral("COM9");
    unknown.peer = peer_state::unknown;
    port_view closed = held;
    closed.id = 4;
    closed.path = QStringLiteral("COM10");
    closed.open = false;

    const auto in_use = ports_in_use({held, waiting, unknown, closed});
    REQUIRE_EQ(in_use.size(), std::size_t{1});
    REQUIRE_EQ(in_use.front().id, vss::endpoint_id{1});

    REQUIRE(removal_warning(waiting).isEmpty());
    REQUIRE(removal_warning(unknown).isEmpty());
    REQUIRE(removal_warning(closed).isEmpty());
    REQUIRE(removal_warning(held).contains(
        QStringLiteral("A program has COM7 open")));

    REQUIRE(exit_warning({}).isEmpty());
    REQUIRE(new_setup_warning({}).isEmpty());
    REQUIRE(exit_warning(in_use).contains(
        QStringLiteral("A program has COM7 open")));
    port_view second = held;
    second.id = 5;
    second.path = QStringLiteral("COM11");
    const QString both = exit_warning({held, second});
    REQUIRE(both.contains(QStringLiteral("COM7, COM11")));
    REQUIRE(both.contains(QStringLiteral("may stop or show an error")));
    REQUIRE(new_setup_warning({held, second})
                .contains(QStringLiteral("COM7, COM11")));
}

TEST_CASE("shell: closing the window warns while a program holds a port")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    vss::gui::FakeStudioBackend backend;
    held_driver_port(backend, QStringLiteral("meter"), 7);
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();

    // Keep Studio open: the window stays and the port is untouched.
    confirm_seen kept;
    answer_next_confirm(QStringLiteral("Keep Studio open"), &kept);
    window.close();
    QApplication::processEvents();
    REQUIRE(kept.shown);
    REQUIRE_EQ(kept.title, QStringLiteral("Close Studio?"));
    REQUIRE(kept.text.contains(QStringLiteral("A program has COM7 open")));
    REQUIRE(window.isVisible());
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});

    // Close Studio: the window closes.
    confirm_seen closed;
    answer_next_confirm(QStringLiteral("Close Studio"), &closed);
    window.close();
    QApplication::processEvents();
    REQUIRE(closed.shown);
    REQUIRE(!window.isVisible());
    clear_settings();
}

TEST_CASE("shell: closing the window asks nothing when no program holds a port")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    vss::gui::FakeStudioBackend backend;
    vss::gui::capabilities caps;
    caps.peer_open = true;
    backend.set_capabilities(caps);
    const auto id =
        backend.create_driver_port(QStringLiteral("meter"), true, 7);
    REQUIRE(id.has_value());
    backend.set_peer_state(*id, vss::gui::peer_state::not_open);
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();
    REQUIRE(window.confirm_exit());
    window.close();
    QApplication::processEvents();
    REQUIRE(!window.isVisible());
    clear_settings();
}

TEST_CASE("shell: removing a port a program holds says so first")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    vss::gui::FakeStudioBackend backend;
    const auto id = held_driver_port(backend, QStringLiteral("meter"), 7);
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();

    auto* selection = window.findChild<vss::gui::SelectionService*>();
    auto* remove = window.findChild<QAction*>(QStringLiteral("ports.remove"));
    REQUIRE(selection != nullptr);
    REQUIRE(remove != nullptr);
    selection->select_port(id);
    QApplication::processEvents();

    confirm_seen refused;
    answer_next_confirm(QStringLiteral("Cancel"), &refused);
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(refused.shown);
    REQUIRE(refused.text.contains(QStringLiteral("A program has COM7 open")));
    REQUIRE(refused.text.contains(QStringLiteral("may stop or show an error")));
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});

    confirm_seen agreed;
    answer_next_confirm(QStringLiteral("Remove port"), &agreed);
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(agreed.shown);
    REQUIRE(backend.ports().empty());

    // A port nobody holds is removed with the usual question only.
    const auto quiet =
        backend.create_driver_port(QStringLiteral("quiet"), true, 8);
    REQUIRE(quiet.has_value());
    backend.set_peer_state(*quiet, vss::gui::peer_state::not_open);
    selection->select_port(*quiet);
    QApplication::processEvents();
    confirm_seen plain;
    answer_next_confirm(QStringLiteral("Remove port"), &plain);
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(plain.shown);
    REQUIRE(!plain.text.contains(QStringLiteral("A program has")));
    REQUIRE(backend.ports().empty());
    window.close();
    clear_settings();
}

TEST_CASE("shell: a new setup warns while a program holds a port")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    vss::gui::FakeStudioBackend backend;
    held_driver_port(backend, QStringLiteral("meter"), 7);
    vss::gui::ShellWindow window(backend);
    window.show();
    QApplication::processEvents();

    auto* file_new = window.findChild<QAction*>(QStringLiteral("file.new"));
    REQUIRE(file_new != nullptr);
    confirm_seen refused;
    answer_next_confirm(QStringLiteral("Cancel"), &refused);
    file_new->trigger();
    QApplication::processEvents();
    REQUIRE(refused.shown);
    REQUIRE(refused.text.contains(QStringLiteral("removes every port")));
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});

    confirm_seen agreed;
    answer_next_confirm(QStringLiteral("Start new"), &agreed);
    file_new->trigger();
    QApplication::processEvents();
    REQUIRE(agreed.shown);
    REQUIRE(backend.ports().empty());
    window.close();
    clear_settings();
}

namespace
{

/// Presses `button` in the next confirmation that opens and records what it
/// said.  It looks again until the dialog is up, so a slow machine does not
/// leave the test waiting in the dialog's event loop.
struct remove_probe
{
    QString title;
    QString body;
    QStringList buttons;
    bool seen = false;
};

void answer_next_remove_confirm(remove_probe* probe, const QString& button,
                                int remaining_ms = 30000)
{
    QTimer::singleShot(
        10,
        [probe, button, remaining_ms]
        {
            auto* dialog = qobject_cast<vss::gui::ConfirmDialog*>(
                QApplication::activeModalWidget());
            if (dialog == nullptr)
            {
                if (remaining_ms > 10)
                    answer_next_remove_confirm(probe, button,
                                               remaining_ms - 10);
                return;
            }
            probe->seen = true;
            probe->title = dialog->windowTitle();
            for (auto* label : dialog->findChildren<QLabel*>())
                probe->body += label->text() + QLatin1Char(' ');
            QPushButton* chosen = nullptr;
            for (auto* push : dialog->findChildren<QPushButton*>())
            {
                probe->buttons << push->text();
                if (push->text() == button)
                    chosen = push;
            }
            if (chosen != nullptr)
                chosen->click();
            else
                dialog->reject();
        });
}

struct setup_fixture
{
    vss::gui::FakeStudioBackend backend;
    vss::endpoint_id first = 0;
    vss::endpoint_id second = 0;
    vss::endpoint_id lone = 0;

    setup_fixture()
    {
        first = *backend.create_auto_port(QStringLiteral("alpha"), true);
        second = *backend.create_auto_port(QStringLiteral("beta"), true);
        lone = *backend.create_auto_port(QStringLiteral("gamma"), true);
        REQUIRE(backend.connect_ports(first, second, {}).has_value());
        backend.refresh();
    }
};

} // namespace

TEST_CASE("setup group: a pair, a share and a lone port")
{
    setup_fixture fixture;
    const auto ports = fixture.backend.ports();
    const auto links = fixture.backend.links();

    const auto pair = vss::gui::find_setup(ports, links, fixture.second);
    REQUIRE_EQ(pair.ports.size(), 2);
    REQUIRE(pair.ports.contains(fixture.first));
    REQUIRE(pair.ports.contains(fixture.second));
    REQUIRE_EQ(pair.links.size(), 1);
    REQUIRE(pair.has_company());
    const QString names = vss::gui::setup_port_names(pair, ports);
    REQUIRE(names.startsWith(QStringLiteral("alpha")));
    REQUIRE(names.contains(QStringLiteral(" and beta")));

    const auto lone = vss::gui::find_setup(ports, links, fixture.lone);
    REQUIRE_EQ(lone.ports.size(), 1);
    REQUIRE(lone.links.isEmpty());
    REQUIRE(!lone.has_company());

    REQUIRE(vss::gui::find_setup(ports, links, 9999).empty());
    const auto by_link =
        vss::gui::find_setup_of_link(ports, links, links.front().id);
    REQUIRE_EQ(by_link.ports.size(), 2);
    REQUIRE(vss::gui::find_setup_of_link(ports, links, 9999).empty());

    // A share is the device and every program port, joined by one link.
    const auto shared = fixture.backend.share_serial(
        QStringLiteral("serial://COM3?baud=9600"), 3);
    REQUIRE(shared.has_value());
    const auto group =
        vss::gui::find_setup(fixture.backend.ports(), fixture.backend.links(),
                             shared->listeners.at(1));
    REQUIRE_EQ(group.ports.size(), 4);
    REQUIRE(group.ports.contains(shared->source));
    REQUIRE_EQ(group.links.size(), 1);
    // The pair and the lone port are not part of it.
    REQUIRE(!group.ports.contains(fixture.first));
    REQUIRE(!group.ports.contains(fixture.lone));
}

TEST_CASE("shell: Remove setup names what closes and removes the whole pair")
{
    clear_settings();
    setup_fixture fixture;
    vss::gui::ShellWindow window(fixture.backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    auto* action =
        window.findChild<QAction*>(QStringLiteral("topology.removeSetup"));
    REQUIRE(action != nullptr);
    REQUIRE(action->isEnabled());

    // Cancel keeps everything and still names the ports.
    remove_probe cancelled;
    answer_next_remove_confirm(&cancelled, QStringLiteral("Cancel"));
    action->trigger();
    QApplication::processEvents();
    REQUIRE(cancelled.seen);
    REQUIRE_EQ(cancelled.title, QStringLiteral("Remove this setup?"));
    REQUIRE(cancelled.body.contains(QStringLiteral("2 ports and 1 link")));
    REQUIRE(cancelled.body.contains(QStringLiteral("alpha")));
    REQUIRE(cancelled.body.contains(QStringLiteral(" and beta")));
    REQUIRE(!cancelled.body.contains(QStringLiteral("gamma")));
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{3});

    remove_probe confirmed;
    answer_next_remove_confirm(&confirmed, QStringLiteral("Remove setup"));
    action->trigger();
    QApplication::processEvents();
    REQUIRE(confirmed.seen);
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{1});
    REQUIRE_EQ(fixture.backend.ports().front().name, QStringLiteral("gamma"));
    REQUIRE(fixture.backend.links().empty());
}

TEST_CASE("shell: Delete on a port in a setup offers the whole setup")
{
    clear_settings();
    setup_fixture fixture;
    vss::gui::ShellWindow window(fixture.backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    auto* remove = window.findChild<QAction*>(QStringLiteral("edit.remove"));
    REQUIRE(remove != nullptr);
    REQUIRE_EQ(remove->shortcut(), QKeySequence(Qt::Key_Delete));

    remove_probe offered;
    answer_next_remove_confirm(&offered, QStringLiteral("Remove only alpha"));
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(offered.seen);
    REQUIRE_EQ(offered.title, QStringLiteral("Remove alpha?"));
    REQUIRE(offered.buttons.contains(QStringLiteral("Remove the whole setup")));
    REQUIRE(offered.buttons.contains(QStringLiteral("Cancel")));
    REQUIRE(offered.body.contains(QStringLiteral("beta")));
    REQUIRE_EQ(fixture.backend.ports().size(), std::size_t{2});
    REQUIRE(!fixture.backend.port(fixture.first).has_value());
}

TEST_CASE("shell: Delete then the whole setup closes a share in one go")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    const auto shared =
        backend.share_serial(QStringLiteral("serial://COM3?baud=9600"), 3);
    REQUIRE(shared.has_value());
    const auto lone = backend.create_auto_port(QStringLiteral("gamma"), true);
    REQUIRE(lone.has_value());
    backend.refresh();

    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    // Delete on the device asks about the whole setup.
    auto* ports_panel = window.findChild<vss::gui::PortsPanel*>();
    REQUIRE(ports_panel != nullptr);
    ports_panel->select_port(shared->source);
    QApplication::processEvents();
    auto* remove = window.findChild<QAction*>(QStringLiteral("edit.remove"));
    REQUIRE(remove != nullptr);
    remove_probe probe;
    answer_next_remove_confirm(&probe,
                               QStringLiteral("Remove the whole setup"));
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(probe.seen);
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});
    REQUIRE_EQ(backend.ports().front().name, QStringLiteral("gamma"));
}

TEST_CASE("shell: removing one program of a share keeps the others sharing")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    const auto shared =
        backend.share_serial(QStringLiteral("serial://COM3?baud=9600"), 3);
    REQUIRE(shared.has_value());
    backend.refresh();

    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    // Select the middle program port through the Ports dock.
    auto* ports_panel = window.findChild<vss::gui::PortsPanel*>();
    REQUIRE(ports_panel != nullptr);
    ports_panel->select_port(shared->listeners.at(1));
    QApplication::processEvents();

    auto* remove = window.findChild<QAction*>(QStringLiteral("edit.remove"));
    REQUIRE(remove != nullptr);
    const QString name =
        vss_test::require_value(backend.port(shared->listeners.at(1))).name;
    remove_probe probe;
    answer_next_remove_confirm(&probe,
                               QStringLiteral("Remove only %1").arg(name));
    remove->trigger();
    QApplication::processEvents();
    REQUIRE(probe.seen);
    REQUIRE(probe.body.contains(QStringLiteral("one of 3 programs")));
    REQUIRE(!backend.port(shared->listeners.at(1)).has_value());
    REQUIRE_EQ(backend.ports().size(), std::size_t{3});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().targets.size(), 2);
}

TEST_CASE("shell: Add a program port extends the share of the selection")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    const auto shared =
        backend.share_serial(QStringLiteral("serial://COM3?baud=9600"), 2);
    REQUIRE(shared.has_value());
    const auto lone = backend.create_auto_port(QStringLiteral("gamma"), true);
    REQUIRE(lone.has_value());
    backend.refresh();

    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    auto* add =
        window.findChild<QAction*>(QStringLiteral("topology.addProgram"));
    REQUIRE(add != nullptr);
    auto* ports_panel = window.findChild<vss::gui::PortsPanel*>();
    REQUIRE(ports_panel != nullptr);

    // A port that is not in a share cannot take another program.
    ports_panel->select_port(*lone);
    QApplication::processEvents();
    REQUIRE(!add->isEnabled());

    ports_panel->select_port(shared->listeners.at(0));
    QApplication::processEvents();
    REQUIRE(add->isEnabled());
    add->trigger();
    QApplication::processEvents();
    REQUIRE_EQ(backend.ports().size(), std::size_t{5});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().targets.size(), 3);
}

TEST_CASE("shell: Remove setup is on the toolbar and the Edit menu")
{
    clear_settings();
    setup_fixture fixture;
    vss::gui::ShellWindow window(fixture.backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    auto* action =
        window.findChild<QAction*>(QStringLiteral("topology.removeSetup"));
    REQUIRE(action != nullptr);
    bool on_toolbar = false;
    for (auto* toolbar : window.findChildren<QToolBar*>())
        on_toolbar = on_toolbar || toolbar->actions().contains(action);
    REQUIRE(on_toolbar);
    bool in_edit_menu = false;
    for (auto* menu : window.findChildren<QMenu*>())
    {
        if (menu->title().remove(QLatin1Char('&')) == QStringLiteral("Edit"))
            in_edit_menu = menu->actions().contains(action);
    }
    REQUIRE(in_edit_menu);
}

namespace
{

/// The banner that offers to make lost ports again, or null.
vss::gui::NoteBanner* find_lost_banner(QWidget& window)
{
    const auto banners = window.findChildren<vss::gui::NoteBanner*>();
    for (auto* banner : banners)
    {
        if (banner->text().startsWith(
                QStringLiteral("Some COM ports were lost")))
            return banner;
    }
    return nullptr;
}

} // namespace

TEST_CASE("shell: a banner offers to recreate ports the driver lost")
{
    clear_settings();
    vss::gui::capabilities caps;
    caps.recreate_ports = true;
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(caps);
    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();

    const auto port = backend.create_virtual_port(
        QStringLiteral("com"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* banner = find_lost_banner(window);
    REQUIRE(banner != nullptr);
    REQUIRE(!banner->isVisible());

    // The helper restarted: the port is lost and the banner says so.
    backend.set_lost_ports({*port});
    QApplication::processEvents();
    REQUIRE(banner->isVisible());
    REQUIRE(banner->action_button() != nullptr);
    REQUIRE_EQ(banner->action_button()->text(),
               QStringLiteral("Recreate ports"));

    banner->action_button()->click();
    REQUIRE(vss_test::wait_until([&] { return !banner->isVisible(); }, 5000));
    REQUIRE(backend.lost_ports().empty());
    REQUIRE(banner->action_button()->isEnabled());
}

TEST_CASE("shell: the banner stays after a failed recreate and says why")
{
    clear_settings();
    vss::gui::capabilities caps;
    caps.recreate_ports = true;
    vss::gui::FakeStudioBackend backend;
    backend.set_capabilities(caps);
    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();

    const auto port = backend.create_virtual_port(
        QStringLiteral("com"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    backend.set_lost_ports({*port});
    QApplication::processEvents();
    auto* banner = find_lost_banner(window);
    REQUIRE(banner != nullptr);
    REQUIRE(banner->isVisible());

    backend.fail_next(vss::make_error_code(vss::errc::device_busy));
    banner->action_button()->click();
    REQUIRE(vss_test::wait_until(
        [&] { return banner->action_button()->isEnabled(); }, 5000));
    // Nothing was made, so the banner is still there for another try.
    REQUIRE(banner->isVisible());
    REQUIRE_EQ(backend.lost_ports().size(), std::size_t{1});
}

TEST_CASE("shell: without the capability a lost port shows no banner")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    const auto port = backend.create_virtual_port(
        QStringLiteral("com"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    backend.set_lost_ports({*port});
    QApplication::processEvents();
    auto* banner = find_lost_banner(window);
    REQUIRE(banner != nullptr);
    REQUIRE(!banner->isVisible());
}

// -- running in the background ----------------------------------------------

namespace
{

QString running_file(const QTemporaryDir& directory)
{
    return directory.filePath(QStringLiteral("running-setup.json"));
}

void write_running(const QString& path, bool stopped)
{
    vss::gui::FakeStudioBackend before;
    REQUIRE(before
                .create_virtual_port(QStringLiteral("console"),
                                     vss::virtual_port_kind::loopback, true)
                .has_value());
    REQUIRE(before
                .create_virtual_port(QStringLiteral("logger"),
                                     vss::virtual_port_kind::loopback, true)
                .has_value());
    REQUIRE(vss::gui::support::save_running_setup(
        path, before.snapshot(),
        {QStringLiteral("C:/setups/lab.json"), true, stopped}));
}

} // namespace

TEST_CASE("background: the ports that ran before a restart start again")
{
    clear_settings();
    QTemporaryDir directory;
    write_running(running_file(directory), false);

    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.enable_background(running_file(directory), false);
    QApplication::processEvents();
    REQUIRE(window.background_enabled());
    REQUIRE(backend.ports().size() == 2);
    // The saved setup it came from and its unsaved mark come back too.
    REQUIRE(window.windowTitle().startsWith(QStringLiteral("lab.json*")));
    REQUIRE(!window.resume_banner()->isVisible());
    REQUIRE(window.needed_in_background());
    clear_settings();
}

TEST_CASE("background: a hidden start with nothing to run has no reason to "
          "stay")
{
    clear_settings();
    QTemporaryDir directory;

    // No running setup at all.
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        window.enable_background(running_file(directory), false);
        QApplication::processEvents();
        REQUIRE(!window.needed_in_background());
    }

    // The ports were stopped with a Quit, so nothing starts.
    write_running(running_file(directory), true);
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        window.enable_background(running_file(directory), false);
        QApplication::processEvents();
        REQUIRE(backend.ports().empty());
        REQUIRE(!window.needed_in_background());
    }
    clear_settings();
}

TEST_CASE("background: a hidden start stays when the ports could not start")
{
    clear_settings();
    QTemporaryDir directory;
    write_running(running_file(directory), false);

    vss::gui::FakeStudioBackend backend;
    backend.fail_next(vss::make_error_code(vss::errc::already_exists));
    vss::gui::ShellWindow window(backend);
    // Not shown: this is what `vss_gui --background` does at sign-in.
    window.enable_background(running_file(directory), false);
    QApplication::processEvents();
    REQUIRE(backend.ports().empty());
    // Quitting now would take the notification with it, and nobody would
    // see the banner that offers the ports again.
    REQUIRE(window.needed_in_background());

    // Start again from the banner works, and then the ports are the reason.
    REQUIRE(window.resume_banner()->action_button() != nullptr);
    window.resume_banner()->action_button()->click();
    QApplication::processEvents();
    REQUIRE(backend.ports().size() == 2);
    REQUIRE(window.needed_in_background());
    clear_settings();
}

TEST_CASE("background: after a Quit Studio offers to start the ports again")
{
    clear_settings();
    QTemporaryDir directory;
    write_running(running_file(directory), true);

    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.show();
    window.enable_background(running_file(directory), false);
    QApplication::processEvents();
    REQUIRE(backend.ports().empty());
    auto* banner = window.resume_banner();
    REQUIRE(banner->isVisible());
    REQUIRE(banner->findChildren<QLabel*>().size() > 0);
    bool said = false;
    for (auto* label : banner->findChildren<QLabel*>())
        said = said || label->text().contains(QStringLiteral("2 ports"));
    REQUIRE(said);

    REQUIRE(banner->action_button() != nullptr);
    banner->action_button()->click();
    QApplication::processEvents();
    REQUIRE(backend.ports().size() == 2);
    REQUIRE(!banner->isVisible());
    clear_settings();
}

TEST_CASE("background: a setup that cannot start is offered again")
{
    clear_settings();
    QTemporaryDir directory;
    write_running(running_file(directory), false);

    vss::gui::FakeStudioBackend backend;
    backend.fail_next(vss::make_error_code(vss::errc::already_exists));
    vss::gui::ShellWindow window(backend);
    window.show();
    window.enable_background(running_file(directory), false);
    QApplication::processEvents();
    REQUIRE(backend.ports().empty());
    REQUIRE(window.resume_banner()->isVisible());
    clear_settings();
}

TEST_CASE("background: what runs is kept, and a Quit is recorded")
{
    clear_settings();
    QTemporaryDir directory;
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    window.enable_background(running_file(directory), false);
    REQUIRE(backend
                .create_virtual_port(QStringLiteral("console"),
                                     vss::virtual_port_kind::loopback, true)
                .has_value());
    backend.refresh();
    QApplication::processEvents();
    window.save_running_now();
    auto setup = vss::gui::support::load_running_setup(running_file(directory));
    REQUIRE(setup.has_value());
    REQUIRE(setup->ports == 1);
    REQUIRE(!setup->state.stopped);

    window.save_running_now(true);
    setup = vss::gui::support::load_running_setup(running_file(directory));
    REQUIRE(setup.has_value());
    REQUIRE(setup->state.stopped);
    clear_settings();
}

TEST_CASE("background: Quit asks before it stops the ports")
{
    clear_settings();
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
    QTemporaryDir directory;
    vss::gui::FakeStudioBackend backend;
    REQUIRE(backend
                .create_virtual_port(QStringLiteral("console"),
                                     vss::virtual_port_kind::loopback, true)
                .has_value());
    vss::gui::ShellWindow window(backend);
    window.show();
    window.enable_background(running_file(directory), false);
    QApplication::processEvents();

    remove_probe probe;
    answer_next_remove_confirm(&probe, QStringLiteral("Keep running"));
    window.quit_studio();
    REQUIRE(probe.seen);
    REQUIRE((probe.title + probe.body)
                .contains(QStringLiteral("Stop the ports and quit")));
    REQUIRE(probe.body.contains(QStringLiteral("1 ports")));
    REQUIRE(backend.ports().size() == 1);
    REQUIRE(window.isVisible());
    clear_settings();
}

TEST_CASE("background: without background running, closing still quits")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    REQUIRE(!window.background_enabled());
    REQUIRE(window.tray() == nullptr);
    REQUIRE(!window.resume_banner()->isVisible());
}

TEST_CASE("background: the one-Studio name follows the account")
{
    // The system's own id for the account, not a name from the environment,
    // which whoever starts Studio can change.
    REQUIRE(!vss::gui::support::account_id().isEmpty());
    const QString name = vss::gui::SingleInstance::default_name();
    REQUIRE(name.startsWith(QStringLiteral("virtual-serial-studio-")));

    const bool had_user = qEnvironmentVariableIsSet("USER");
    const bool had_username = qEnvironmentVariableIsSet("USERNAME");
    const QByteArray user = qgetenv("USER");
    const QByteArray username = qgetenv("USERNAME");
    qputenv("USER", "somebody-else");
    qputenv("USERNAME", "somebody-else");
    const QString renamed = vss::gui::SingleInstance::default_name();
    qunsetenv("USER");
    qunsetenv("USERNAME");
    const QString unnamed = vss::gui::SingleInstance::default_name();
    if (had_user)
        qputenv("USER", user);
    if (had_username)
        qputenv("USERNAME", username);

    REQUIRE(renamed == name);
    REQUIRE(unnamed == name);
}

TEST_CASE("background: the tray icon says what runs")
{
    vss::gui::StudioTray tray;
    tray.set_state(3, 2, false);
    REQUIRE(tray.status_text() == QStringLiteral("3 ports, 2 links running"));
    REQUIRE(!tray.warn());
    tray.set_state(3, 2, true);
    REQUIRE(tray.warn());
    tray.set_sign_in(false, true);
    QAction* sign_in = nullptr;
    for (QAction* action : tray.menu()->actions())
        if (action->objectName() == QStringLiteral("traySignIn"))
            sign_in = action;
    REQUIRE(sign_in != nullptr);
    REQUIRE(!sign_in->isEnabled());
    REQUIRE(!sign_in->isChecked());
    REQUIRE(sign_in->toolTip() ==
            QStringLiteral("Not available in this build"));
}

TEST_CASE("shell: a transfer rate is formatted for any double")
{
    // Rates far above the range of std::uint64_t used to be converted out of
    // range (UBSan); negative and not-a-number rates read as zero.
    REQUIRE_EQ(vss::gui::format_rate(-5.0), QStringLiteral("0 B/s"));
    REQUIRE_EQ(vss::gui::format_rate(std::nan("")), QStringLiteral("0 B/s"));
    REQUIRE_EQ(vss::gui::format_rate(512.0), QStringLiteral("512 B/s"));
    REQUIRE(vss::gui::format_rate(2048.0).endsWith(QStringLiteral("/s")));
    REQUIRE(vss::gui::format_rate(7.7e20).endsWith(QStringLiteral("/s")));
    REQUIRE(vss::gui::format_rate(1.0e300).endsWith(QStringLiteral("/s")));
}
