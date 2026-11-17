//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "shell/dock_panes.hpp"
#include "shell/dock_zones.hpp"
#include "shell/shell_window.hpp"
#include "theme/theme.hpp"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>




namespace
{

using vss::gui::drop_zone;

void clear_settings()
{
    QSettings settings;
    settings.clear();
}

struct Panes
{
    QDockWidget* monitor = nullptr;
    QDockWidget* log = nullptr;
    QDockWidget* json = nullptr;
    QAction* advanced = nullptr;
};

Panes find_panes(vss::gui::ShellWindow& window)
{
    Panes panes;
    panes.monitor =
        window.findChild<QDockWidget*>(QStringLiteral("monitorDock"));
    panes.log = window.findChild<QDockWidget*>(QStringLiteral("logDock"));
    panes.json = window.findChild<QDockWidget*>(QStringLiteral("jsonDock"));
    panes.advanced =
        window.findChild<QAction*>(QStringLiteral("view.advanced"));
    REQUIRE(panes.monitor != nullptr);
    REQUIRE(panes.log != nullptr);
    REQUIRE(panes.json != nullptr);
    REQUIRE(panes.advanced != nullptr);
    return panes;
}

void show_advanced(vss::gui::ShellWindow& window, const Panes& panes)
{
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();
    if (!panes.advanced->isChecked())
        panes.advanced->trigger();
    QApplication::processEvents();
}

QPoint centre_of(const QWidget& window, const QRect& rect)
{
    return window.mapToGlobal(rect.center());
}

QRect rect_in(const QWidget& window, const QDockWidget* dock)
{
    return QRect(dock->mapTo(&window, QPoint(0, 0)), dock->size());
}

void save_shot(QWidget& window, const QString& name)
{
    const QString directory = qEnvironmentVariable("VSS_DOCK_SHOTS");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    REQUIRE(window.grab().save(QDir(directory).filePath(name)));
}

} // namespace

TEST_CASE("dock zones: guides, hit test and preview")
{
    const QRect pane(100, 100, 400, 300);
    const QRect area(0, 0, 1000, 700);
    const int size = 36;
    const int gap = 4;
    const int margin = 8;

    const auto guides = vss::gui::pane_guides(pane, size, gap);
    REQUIRE_EQ(guides.size(), std::size_t{5});
    // The centre guide sits at the middle of the pane.
    REQUIRE_EQ(guides.front().zone, drop_zone::centre);
    REQUIRE((guides.front().rect.center() - pane.center()).manhattanLength() <=
            2);

    REQUIRE_EQ(vss::gui::zone_at(pane.center(), pane, area, size, gap, margin),
               drop_zone::centre);
    for (const auto& guide : guides)
    {
        REQUIRE_EQ(vss::gui::zone_at(guide.rect.center(), pane, area, size,
                                     gap, margin),
                   guide.zone);
    }
    // Over the pane but on no guide, there is no zone.
    REQUIRE_EQ(vss::gui::zone_at(pane.topLeft() + QPoint(5, 5), pane, area,
                                 size, gap, margin),
               drop_zone::none);
    // The guide to the right of the centre is the right zone.
    REQUIRE_EQ(vss::gui::zone_at(pane.center() + QPoint(size + gap, 0), pane,
                                 area, size, gap, margin),
               drop_zone::right);
    REQUIRE_EQ(vss::gui::zone_at(pane.center() - QPoint(0, size + gap), pane,
                                 area, size, gap, margin),
               drop_zone::top);

    // The window edges win over a pane under them.
    const auto edges = vss::gui::window_guides(area, size, margin);
    REQUIRE_EQ(edges.size(), std::size_t{4});
    for (const auto& guide : edges)
    {
        REQUIRE(vss::gui::is_window_zone(guide.zone));
        REQUIRE_EQ(vss::gui::zone_at(guide.rect.center(), pane, area, size,
                                     gap, margin),
                   guide.zone);
        REQUIRE(area.contains(guide.rect));
    }

    // A pane too small for the cross gets the centre guide only.
    const QRect small(0, 0, 60, 60);
    REQUIRE_EQ(vss::gui::pane_guides(small, size, gap).size(), std::size_t{1});
    REQUIRE(vss::gui::pane_guides(QRect(), size, gap).empty());

    // Previews: half the pane, all of it for the centre, a strip at the edge.
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::centre, pane, area), pane);
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::left, pane, area).width(),
               200);
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::right, pane, area).left(),
               300);
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::top, pane, area).height(),
               150);
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::bottom, pane, area).top(),
               250);
    REQUIRE_EQ(vss::gui::preview_rect(drop_zone::window_bottom, pane, area)
                   .bottom(),
               area.bottom());
    REQUIRE(vss::gui::preview_rect(drop_zone::none, pane, area).isNull());
}

TEST_CASE("dock panes: default layout is the bottom tabs")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);

    REQUIRE(window.dock_panes() != nullptr);
    REQUIRE_EQ(window.dockWidgetArea(panes.monitor),
               Qt::BottomDockWidgetArea);
    REQUIRE_EQ(window.dockWidgetArea(panes.log), Qt::BottomDockWidgetArea);
    REQUIRE_EQ(window.dockWidgetArea(panes.json), Qt::BottomDockWidgetArea);
    // One tab group, the Monitor in front.
    REQUIRE(window.tabifiedDockWidgets(panes.monitor).contains(panes.log));
    REQUIRE(window.tabifiedDockWidgets(panes.monitor).contains(panes.json));
    REQUIRE(panes.monitor->isVisible());

    // All four panes are movable, floatable and closable, with a title bar
    // that has names for assistive technology.
    for (QDockWidget* dock : {panes.monitor, panes.log, panes.json})
    {
        REQUIRE(dock->features().testFlag(QDockWidget::DockWidgetMovable));
        REQUIRE(dock->features().testFlag(QDockWidget::DockWidgetFloatable));
        REQUIRE(dock->features().testFlag(QDockWidget::DockWidgetClosable));
        auto* bar = qobject_cast<vss::gui::DockTitleBar*>(dock->titleBarWidget());
        REQUIRE(bar != nullptr);
        REQUIRE(!bar->accessibleName().isEmpty());
        REQUIRE(!bar->menu_button()->accessibleName().isEmpty());
        REQUIRE(!bar->close_button()->accessibleName().isEmpty());
        REQUIRE(bar->menu_button()->focusPolicy() != Qt::NoFocus);
        REQUIRE(bar->menu_button()->width() >= 24);
        REQUIRE(bar->close_button()->height() >= 24);
    }

    // The View menu entries are checked while the panes are open, including
    // the ones behind a tab.
    QAction* log_action = window.findChild<QAction*>(QStringLiteral("view.log"));
    QAction* json_action =
        window.findChild<QAction*>(QStringLiteral("view.json"));
    QAction* monitor_action =
        window.findChild<QAction*>(QStringLiteral("view.monitor"));
    REQUIRE(log_action->isChecked());
    REQUIRE(json_action->isChecked());
    REQUIRE(monitor_action->isChecked());
    save_shot(window, QStringLiteral("default-light.png"));
}

TEST_CASE("dock panes: close and open again from the View menu")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);

    QAction* monitor_action =
        window.findChild<QAction*>(QStringLiteral("view.monitor"));
    auto* bar =
        qobject_cast<vss::gui::DockTitleBar*>(panes.monitor->titleBarWidget());
    REQUIRE(bar != nullptr);
    bar->close_button()->click();
    QApplication::processEvents();
    REQUIRE(!panes.monitor->isVisible());
    REQUIRE(!monitor_action->isChecked());

    // Something else updating the window does not bring it back.
    window.resize(1200, 760);
    QApplication::processEvents();
    REQUIRE(!panes.monitor->isVisible());

    monitor_action->trigger();
    QApplication::processEvents();
    REQUIRE(monitor_action->isChecked());
    REQUIRE(panes.monitor->isVisible());

    // Simple mode hides the panes without clearing the check marks.
    panes.advanced->trigger();
    QApplication::processEvents();
    REQUIRE(!panes.monitor->isVisible());
    REQUIRE(monitor_action->isChecked());
    panes.advanced->trigger();
    QApplication::processEvents();
    REQUIRE(panes.monitor->isVisible());

    // The Panes menu is under View and holds the four panes.
    QMenu* panes_menu = window.findChild<QMenu*>(QStringLiteral("panesMenu"));
    REQUIRE(panes_menu != nullptr);
    REQUIRE(panes_menu->actions().contains(monitor_action));
    REQUIRE(monitor_action->isCheckable());
}

TEST_CASE("dock panes: side by side, stacked and floating")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);
    vss::gui::DockPanes* docks = window.dock_panes();

    // Log to the right of the Monitor.
    docks->split_beside(panes.log, panes.monitor, drop_zone::right);
    QApplication::processEvents();
    REQUIRE(panes.monitor->isVisible());
    REQUIRE(panes.log->isVisible());
    REQUIRE(!window.tabifiedDockWidgets(panes.monitor).contains(panes.log));
    REQUIRE(rect_in(window, panes.log).left() >
            rect_in(window, panes.monitor).left());
    REQUIRE_EQ(rect_in(window, panes.log).top(),
               rect_in(window, panes.monitor).top());
    save_shot(window, QStringLiteral("side-by-side-light.png"));

    // Log to the left puts it before the Monitor.
    docks->split_beside(panes.log, panes.monitor, drop_zone::left);
    QApplication::processEvents();
    REQUIRE(rect_in(window, panes.log).left() <
            rect_in(window, panes.monitor).left());

    // Above stacks them.
    docks->split_beside(panes.log, panes.monitor, drop_zone::top);
    QApplication::processEvents();
    REQUIRE(rect_in(window, panes.log).top() <
            rect_in(window, panes.monitor).top());

    // The JSON pane tabs with the Log.
    docks->tab_with(panes.json, panes.log);
    QApplication::processEvents();
    REQUIRE(window.tabifiedDockWidgets(panes.log).contains(panes.json));

    // Float and dock again.
    docks->float_pane(panes.json);
    QApplication::processEvents();
    REQUIRE(panes.json->isFloating());
    docks->move_to_edge(panes.json, Qt::RightDockWidgetArea);
    QApplication::processEvents();
    REQUIRE(!panes.json->isFloating());
    REQUIRE_EQ(window.dockWidgetArea(panes.json), Qt::RightDockWidgetArea);
}

TEST_CASE("dock panes: dragging shows the guides and a drop lands the pane")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);
    vss::gui::DockPanes* docks = window.dock_panes();

    // Tab the Log out of the group first so the Monitor is not shared.
    docks->begin_drag(panes.log, window.mapToGlobal(QPoint(10, 10)));
    REQUIRE(docks->dragging());
    REQUIRE(docks->overlay()->isVisible());

    // Over the right guide of the Monitor, the preview is its right half.
    const QRect monitor = rect_in(window, panes.monitor);
    const int size = vss::gui::DockDropOverlay::guide_size();
    const int gap = vss::gui::DockDropOverlay::guide_gap();
    const auto guides = vss::gui::pane_guides(monitor, size, gap);
    QRect right_guide;
    for (const auto& guide : guides)
    {
        if (guide.zone == drop_zone::right)
            right_guide = guide.rect;
    }
    REQUIRE(right_guide.isValid());
    docks->update_drag(centre_of(window, right_guide));
    REQUIRE_EQ(docks->current_zone(), drop_zone::right);
    REQUIRE_EQ(docks->overlay()->preview(),
               vss::gui::preview_rect(drop_zone::right, monitor,
                                      docks->drop_area()));
    save_shot(window, QStringLiteral("guides-light.png"));
    docks->finish_drag(centre_of(window, right_guide));
    QApplication::processEvents();
    REQUIRE(!docks->dragging());
    REQUIRE(!docks->overlay()->isVisible());
    REQUIRE(panes.log->isVisible());
    REQUIRE(rect_in(window, panes.log).left() >
            rect_in(window, panes.monitor).left());

    // Escape-style cancel changes nothing.
    docks->begin_drag(panes.log, window.mapToGlobal(QPoint(10, 10)));
    docks->cancel_drag();
    REQUIRE(!docks->dragging());
    REQUIRE(!docks->overlay()->isVisible());

    // Dropping outside the window floats the pane there.
    docks->begin_drag(panes.json, window.mapToGlobal(QPoint(10, 10)));
    docks->finish_drag(QPoint(-400, -400));
    QApplication::processEvents();
    REQUIRE(panes.json->isFloating());

    // Dropping on a window edge guide docks it at that edge.
    docks->begin_drag(panes.json, QPoint(-400, -400));
    const auto edges = vss::gui::window_guides(
        docks->drop_area(), size, vss::gui::DockDropOverlay::guide_margin());
    QRect left_edge;
    for (const auto& guide : edges)
    {
        if (guide.zone == drop_zone::window_left)
            left_edge = guide.rect;
    }
    docks->update_drag(centre_of(window, left_edge));
    REQUIRE(left_edge.isValid());
    REQUIRE_EQ(docks->current_zone(), drop_zone::window_left);
    docks->finish_drag(centre_of(window, left_edge));
    QApplication::processEvents();
    REQUIRE(!panes.json->isFloating());
    REQUIRE_EQ(window.dockWidgetArea(panes.json), Qt::LeftDockWidgetArea);
}

TEST_CASE("dock panes: the pane menu has every move command")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);

    auto* bar =
        qobject_cast<vss::gui::DockTitleBar*>(panes.monitor->titleBarWidget());
    REQUIRE(bar != nullptr);
    bar->menu()->aboutToShow();
    QStringList texts;
    for (QAction* action : bar->menu()->actions())
        texts.push_back(action->text());
    REQUIRE(texts.contains(QStringLiteral("Float")));
    REQUIRE(texts.contains(QStringLiteral("Move to the window edge")));
    REQUIRE(texts.contains(QStringLiteral("Close")));
    // The other panes behind the tab are not on screen, so the Monitor has no
    // neighbour to go beside until the Log is split out.
    window.dock_panes()->split_beside(panes.log, panes.monitor,
                                      drop_zone::right);
    QApplication::processEvents();
    bar->menu()->aboutToShow();
    texts.clear();
    for (QAction* action : bar->menu()->actions())
    {
        texts.push_back(action->text());
        if (action->menu() != nullptr)
        {
            for (QAction* sub : action->menu()->actions())
                texts.push_back(sub->text());
        }
    }
    REQUIRE(texts.contains(QStringLiteral("Beside Log")));
    REQUIRE(texts.contains(QStringLiteral("Tab with Log")));
    REQUIRE(texts.contains(QStringLiteral("Right of Log")));

    // Next pane moves focus between the open panes.
    QAction* next = window.findChild<QAction*>(QStringLiteral("view.nextPane"));
    REQUIRE(next != nullptr);
    next->trigger();
    QApplication::processEvents();
}

TEST_CASE("dock panes: the layout is saved, restored and reset")
{
    clear_settings();
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        const Panes panes = find_panes(window);
        show_advanced(window, panes);
        window.dock_panes()->split_beside(panes.log, panes.monitor,
                                          drop_zone::right);
        QAction* json_action =
            window.findChild<QAction*>(QStringLiteral("view.json"));
        json_action->setChecked(false);
        QApplication::processEvents();
        REQUIRE(!panes.json->isVisible());
        REQUIRE(!window.layout_state().isEmpty());
        window.close();
        QApplication::processEvents();
    }

    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        const Panes panes = find_panes(window);
        show_advanced(window, panes);
        // The saved layout came back: Log beside the Monitor, JSON closed.
        REQUIRE(panes.log->isVisible());
        REQUIRE(!window.tabifiedDockWidgets(panes.monitor).contains(panes.log));
        REQUIRE(rect_in(window, panes.log).left() >
                rect_in(window, panes.monitor).left());
        REQUIRE(!panes.json->isVisible());

        // Reset puts the three back in one tab group, Monitor in front.
        QAction* reset =
            window.findChild<QAction*>(QStringLiteral("view.resetLayout"));
        REQUIRE(reset != nullptr);
        reset->trigger();
        QApplication::processEvents();
        REQUIRE(window.tabifiedDockWidgets(panes.monitor).contains(panes.log));
        REQUIRE(panes.monitor->isVisible());
        window.close();
    }

    // A damaged or old state is refused and the default layout stays.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/windowState"),
                          QByteArray("not a layout"));
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        const Panes panes = find_panes(window);
        show_advanced(window, panes);
        REQUIRE(!window.apply_layout_state(QByteArray("junk")));
        REQUIRE(!window.apply_layout_state(QByteArray()));
        REQUIRE(window.tabifiedDockWidgets(panes.monitor).contains(panes.log));
        REQUIRE(panes.monitor->isVisible());
        // A state saved without the version is refused too.
        REQUIRE(!window.apply_layout_state(window.saveState(0)));
        window.close();
    }
}

TEST_CASE("dock panes: a floating pane saved off screen is docked again")
{
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);
    window.dock_panes()->float_pane(panes.json);
    QApplication::processEvents();
    REQUIRE(panes.json->isFloating());
    panes.json->move(-50000, -50000);
    QApplication::processEvents();
    REQUIRE_EQ(window.dock_panes()->rescue_offscreen(), 1);
    REQUIRE(!panes.json->isFloating());
}

TEST_CASE("dock panes: pictures in both themes")
{
    if (qEnvironmentVariable("VSS_DOCK_SHOTS").isEmpty())
        VSS_SKIP("VSS_DOCK_SHOTS is not set");
    clear_settings();
    vss::gui::FakeStudioBackend backend;
    vss::gui::ShellWindow window(backend);
    const Panes panes = find_panes(window);
    show_advanced(window, panes);
    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        vss::gui::Theme::instance().set_mode(mode);
        QApplication::processEvents();
        const QString suffix = mode == vss::gui::theme_mode::light
                                   ? QStringLiteral("light")
                                   : QStringLiteral("dark");
        window.reset_layout();
        QApplication::processEvents();
        save_shot(window, QStringLiteral("default-%1.png").arg(suffix));
        vss::gui::DockPanes* docks = window.dock_panes();
        docks->split_beside(panes.log, panes.monitor, drop_zone::right);
        QApplication::processEvents();
        // No tab bar from a group that no longer exists is left showing.
        for (QTabBar* bar : window.findChildren<QTabBar*>())
        {
            if (bar->isVisible() && bar->count() == 3)
                REQUIRE(false);
        }
        save_shot(window, QStringLiteral("side-by-side-%1.png").arg(suffix));
        docks->begin_drag(panes.log, window.mapToGlobal(QPoint(10, 10)));
        const QRect monitor = rect_in(window, panes.monitor);
        const auto guides = vss::gui::pane_guides(
            monitor, vss::gui::DockDropOverlay::guide_size(),
            vss::gui::DockDropOverlay::guide_gap());
        docks->update_drag(centre_of(window, guides[2].rect));
        QApplication::processEvents();
        save_shot(window, QStringLiteral("guides-%1.png").arg(suffix));
        docks->cancel_drag();
    }
    vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::system);
}

TEST_CASE("dock panes: Ports, Inspector and Send move, close and reset too")
{
    clear_settings();
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        const Panes panes = find_panes(window);
        show_advanced(window, panes);
        auto* ports = window.findChild<QDockWidget*>(QStringLiteral("portsDock"));
        auto* inspector =
            window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"));
        auto* send = window.findChild<QDockWidget*>(QStringLiteral("sendDock"));
        REQUIRE(ports != nullptr);
        REQUIRE(inspector != nullptr);
        REQUIRE(send != nullptr);
        vss::gui::DockPanes* docks = window.dock_panes();
        for (QDockWidget* dock : {ports, inspector, send})
        {
            REQUIRE(dock->features().testFlag(QDockWidget::DockWidgetMovable));
            auto* bar =
                qobject_cast<vss::gui::DockTitleBar*>(dock->titleBarWidget());
            REQUIRE(bar != nullptr);
            REQUIRE(!bar->accessibleName().isEmpty());
            REQUIRE(docks->docks().contains(dock));
        }
        // The View menu lists them, checked.
        for (const char* name : {"view.ports", "view.inspector", "view.send"})
        {
            QAction* action =
                window.findChild<QAction*>(QString::fromLatin1(name));
            REQUIRE(action != nullptr);
            REQUIRE(action->isChecked());
        }

        // Close Ports from its title bar; it stays closed, then comes back.
        auto* bar = qobject_cast<vss::gui::DockTitleBar*>(ports->titleBarWidget());
        bar->close_button()->click();
        QApplication::processEvents();
        QAction* ports_action =
            window.findChild<QAction*>(QStringLiteral("view.ports"));
        REQUIRE(!ports->isVisible());
        REQUIRE(!ports_action->isChecked());
        window.resize(1200, 760);
        QApplication::processEvents();
        REQUIRE(!ports->isVisible());
        ports_action->trigger();
        QApplication::processEvents();
        REQUIRE(ports->isVisible());

        // Send goes to the left of the Monitor; the compact window no longer
        // tabs it with the Inspector.
        docks->split_beside(send, panes.monitor, drop_zone::left);
        QApplication::processEvents();
        REQUIRE(!window.tabifiedDockWidgets(inspector).contains(send));
        window.resize(980, 660);
        QApplication::processEvents();
        window.resize(1280, 800);
        QApplication::processEvents();
        REQUIRE(!window.tabifiedDockWidgets(inspector).contains(send));
        REQUIRE(rect_in(window, send).bottom() > rect_in(window, ports).bottom() -
                                                     10);

        // Inspector tabs with Ports.
        docks->tab_with(inspector, ports);
        QApplication::processEvents();
        REQUIRE(window.tabifiedDockWidgets(ports).contains(inspector));
        save_shot(window, QStringLiteral("all-panes-light.png"));
        window.close();
    }
    {
        // The arrangement comes back, and Reset layout restores the default.
        vss::gui::FakeStudioBackend backend;
        vss::gui::ShellWindow window(backend);
        const Panes panes = find_panes(window);
        show_advanced(window, panes);
        auto* ports = window.findChild<QDockWidget*>(QStringLiteral("portsDock"));
        auto* inspector =
            window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"));
        auto* send = window.findChild<QDockWidget*>(QStringLiteral("sendDock"));
        REQUIRE(window.tabifiedDockWidgets(ports).contains(inspector));
        QAction* reset =
            window.findChild<QAction*>(QStringLiteral("view.resetLayout"));
        reset->trigger();
        QApplication::processEvents();
        REQUIRE(!window.tabifiedDockWidgets(ports).contains(inspector));
        REQUIRE_EQ(window.dockWidgetArea(ports), Qt::LeftDockWidgetArea);
        REQUIRE_EQ(window.dockWidgetArea(inspector), Qt::RightDockWidgetArea);
        REQUIRE_EQ(window.dockWidgetArea(send), Qt::RightDockWidgetArea);
        REQUIRE(ports->isVisible());
        REQUIRE(inspector->isVisible());
        REQUIRE(send->isVisible());
        REQUIRE(rect_in(window, send).top() > rect_in(window, inspector).top());
        window.close();
    }
}
