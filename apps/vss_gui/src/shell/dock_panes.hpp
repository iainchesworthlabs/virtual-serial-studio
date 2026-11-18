//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "shell/dock_zones.hpp"

#include <QFrame>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QVector>
#include <QWidget>

class QDockWidget;
class QMainWindow;
class QMenu;
class QTabBar;
class QToolButton;
class QLabel;

namespace vss::gui
{

/// The title bar of a dock pane: the title, a menu with every move command
/// and a close button.  Dragging it asks the owner to show the drop guides;
/// the owner decides what a drop does.  Every command is also in the menu,
/// so the mouse is never the only way to move a pane.
class DockTitleBar final : public QFrame
{
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock, QWidget* parent = nullptr);

    QToolButton* menu_button() const noexcept { return menu_button_; }
    QToolButton* close_button() const noexcept { return close_button_; }

    /// The menu is filled in by the owner just before it is shown.
    QMenu* menu() const noexcept { return menu_; }

Q_SIGNALS:
    void drag_started(const QPoint& global);
    void drag_moved(const QPoint& global);
    void drag_finished(const QPoint& global);
    void drag_cancelled();
    void menu_requested(QMenu* menu);
    void close_requested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void apply_theme();
    void end_drag();

    QDockWidget* dock_ = nullptr;
    QLabel* title_ = nullptr;
    QToolButton* menu_button_ = nullptr;
    QToolButton* close_button_ = nullptr;
    QMenu* menu_ = nullptr;
    QPoint press_;
    bool pressed_ = false;
    bool dragging_ = false;
};

/// Draws the drop guides and the shaded preview over the main window.  It
/// never takes the mouse.
class DockDropOverlay final : public QWidget
{
    Q_OBJECT

public:
    explicit DockDropOverlay(QWidget* parent);

    /// `panes` are the rectangles of the panes that can be dropped on, in
    /// this widget's coordinates; `area` is the whole dock area.
    void show_guides(const QVector<QRect>& panes, const QRect& area);
    void set_hover(const QRect& pane, drop_zone zone);
    void hide_guides();

    drop_zone hover_zone() const noexcept { return zone_; }
    QRect preview() const noexcept { return preview_; }

    static int guide_size();
    static int guide_gap();
    static int guide_margin();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QRect> panes_;
    QRect area_;
    QRect hover_pane_;
    QRect preview_;
    drop_zone zone_ = drop_zone::none;
};

/// Owns the movable panes of a main window: puts a title bar on each, shows
/// the guides while one is dragged and carries out the drop.  The mechanism
/// is Qt's own `QMainWindow` docking; this class adds the guides and the
/// commands around it.
class DockPanes final : public QObject
{
    Q_OBJECT

public:
    explicit DockPanes(QMainWindow* window);

    /// Gives `dock` a title bar and lets it move, float and close.
    void manage(QDockWidget* dock);
    const QVector<QPointer<QDockWidget>>& docks() const noexcept
    {
        return docks_;
    }

    /// Docks `dock` against an edge of the window.
    void move_to_edge(QDockWidget* dock, Qt::DockWidgetArea area);
    /// Puts `dock` as a tab in the group of `target`.
    void tab_with(QDockWidget* dock, QDockWidget* target);
    /// Puts `dock` beside `target`: `zone` is left, right, top or bottom.
    void split_beside(QDockWidget* dock, QDockWidget* target, drop_zone zone);
    void float_pane(QDockWidget* dock);
    /// Closes `dock` and says so; the View menu entry follows the signal.
    void close_pane(QDockWidget* dock);
    /// Brings the next or previous open pane to the front and focuses it.
    void cycle(int step);

    /// Floating panes that are off every screen are docked again.
    /// Returns the number that were moved.
    int rescue_offscreen();

    /// The part of the window the panes dock into, in window coordinates.
    QRect drop_area() const;

    /// Hides tab bars Qt left behind after a tab group was taken apart.
    void refresh_tab_bars();

    /// True while a pane is being dragged.
    bool dragging() const noexcept { return dragged_ != nullptr; }

    /// The drop under `global` while dragging, for tests.
    drop_zone current_zone() const noexcept;
    DockDropOverlay* overlay() const noexcept { return overlay_; }

    /// Drives a drag without the mouse (tests and the guides picture).
    void begin_drag(QDockWidget* dock, const QPoint& global);
    void update_drag(const QPoint& global);
    void finish_drag(const QPoint& global);
    void cancel_drag();

    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    /// The user closed a pane (its close button, its menu, or the window
    /// system). Hiding a pane for another reason does not send this.
    void pane_closed(QDockWidget* dock);
    /// A pane was moved, tabbed, split or floated by the user.
    void layout_changed();

private:
    bool tab_bar_event(QTabBar* bar, QEvent* event);
    void fill_menu(QDockWidget* dock, QMenu* menu);
    QVector<QDockWidget*> targets_for(QDockWidget* dragged) const;
    QDockWidget* target_at(const QPoint& window_point,
                           QDockWidget* dragged) const;
    QRect dock_rect(const QDockWidget* dock) const;
    void evaluate(const QPoint& global);
    void hide_stale_tab_bars();

    QMainWindow* window_ = nullptr;
    QVector<QPointer<QDockWidget>> docks_;
    DockDropOverlay* overlay_ = nullptr;
    QDockWidget* dragged_ = nullptr;
    QDockWidget* hover_target_ = nullptr;
    QPoint grab_offset_;
    // A drag that began on a tab of Qt's tab bar.
    QPointer<QTabBar> tab_bar_;
    QPointer<QDockWidget> tab_dock_;
    QPoint tab_press_;
    bool tab_dragging_ = false;
};

} // namespace vss::gui
