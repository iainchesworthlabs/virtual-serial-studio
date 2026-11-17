//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/dock_panes.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>


namespace vss::gui
{

namespace
{

QString arrow_name(drop_zone zone)
{
    switch (zone)
    {
    case drop_zone::left:
    case drop_zone::window_left:
        return QStringLiteral("left");
    case drop_zone::right:
    case drop_zone::window_right:
        return QStringLiteral("right");
    case drop_zone::top:
    case drop_zone::window_top:
        return QStringLiteral("top");
    case drop_zone::bottom:
    case drop_zone::window_bottom:
        return QStringLiteral("bottom");
    default:
        return QStringLiteral("centre");
    }
}

Qt::Orientation orientation_of(drop_zone zone)
{
    return (zone == drop_zone::left || zone == drop_zone::right)
               ? Qt::Horizontal
               : Qt::Vertical;
}

bool is_open(const QDockWidget* dock)
{
    return dock != nullptr && !dock->isHidden();
}

} // namespace

// ---------------------------------------------------------------- title bar

DockTitleBar::DockTitleBar(QDockWidget* dock, QWidget* parent)
    : QFrame(parent), dock_(dock)
{
    setObjectName(QStringLiteral("dockTitleBar"));
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(tr("%1 title bar").arg(dock->windowTitle()));
    setAccessibleDescription(
        tr("Drag to move the pane. The menu has every move command."));

    const Theme& theme = Theme::instance();
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(theme.space(3), 0, theme.space(0), 0);
    layout->setSpacing(theme.space(0));

    title_ = new QLabel(dock->windowTitle(), this);
    title_->setObjectName(QStringLiteral("dockTitle"));
    title_->setAccessibleName(dock->windowTitle());
    layout->addWidget(title_, 1);

    menu_button_ = new QToolButton(this);
    menu_button_->setObjectName(QStringLiteral("dockMenuButton"));
    menu_button_->setToolTip(tr("Pane menu"));
    menu_button_->setAccessibleName(
        tr("%1 pane menu").arg(dock->windowTitle()));
    menu_button_->setPopupMode(QToolButton::InstantPopup);
    menu_button_->setFocusPolicy(Qt::StrongFocus);
    menu_ = new QMenu(menu_button_);
    menu_->setObjectName(QStringLiteral("dockMenu"));
    menu_button_->setMenu(menu_);
    connect(menu_, &QMenu::aboutToShow, this,
            [this] { Q_EMIT menu_requested(menu_); });
    layout->addWidget(menu_button_);

    close_button_ = new QToolButton(this);
    close_button_->setObjectName(QStringLiteral("dockCloseButton"));
    close_button_->setToolTip(tr("Close"));
    close_button_->setAccessibleName(tr("Close %1").arg(dock->windowTitle()));
    close_button_->setFocusPolicy(Qt::StrongFocus);
    connect(close_button_, &QToolButton::clicked, this,
            [this] { Q_EMIT close_requested(); });
    layout->addWidget(close_button_);

    connect(dock, &QDockWidget::windowTitleChanged, this,
            [this](const QString& text)
            {
                title_->setText(text);
                title_->setAccessibleName(text);
                setAccessibleName(tr("%1 title bar").arg(text));
                menu_button_->setAccessibleName(tr("%1 pane menu").arg(text));
                close_button_->setAccessibleName(tr("Close %1").arg(text));
            });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_theme(); });
    apply_theme();
}

void DockTitleBar::apply_theme()
{
    const Theme& theme = Theme::instance();
    // The 24 px floor is the minimum target; the buttons are the small size.
    const int button = theme.metric(QStringLiteral("buttonSmall"));
    setFixedHeight(button + theme.space(0));
    for (QToolButton* tool : {menu_button_, close_button_})
        tool->setFixedSize(button, button);
    menu_button_->setIcon(
        IconProvider::instance().icon(QStringLiteral("more"),
                                      colour_role::ink2, 16));
    close_button_->setIcon(
        IconProvider::instance().icon(QStringLiteral("remove"),
                                      colour_role::ink2, 16));
    title_->setFont(theme.font(text_style::label));
    QPalette palette = title_->palette();
    palette.setColor(QPalette::WindowText, theme.colour(colour_role::ink2));
    title_->setPalette(palette);
    update();
}

void DockTitleBar::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::PaletteChange)
        update();
}

void DockTitleBar::paintEvent(QPaintEvent*)
{
    // A grip of dots at the start says the bar can be dragged.
    QPainter painter(this);
    const Theme& theme = Theme::instance();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.colour(colour_role::ink3));
    const int x = theme.space(1);
    const int y = height() / 2;
    for (int column = 0; column < 2; ++column)
    {
        for (int row = -1; row <= 1; ++row)
            painter.drawEllipse(QPointF(x + column * 4, y + row * 4), 1.0,
                                1.0);
    }
}

void DockTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        pressed_ = true;
        dragging_ = false;
        press_ = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (!pressed_)
    {
        QFrame::mouseMoveEvent(event);
        return;
    }
    const QPoint global = event->globalPosition().toPoint();
    if (!dragging_)
    {
        if ((global - press_).manhattanLength() <
            QApplication::startDragDistance())
            return;
        dragging_ = true;
        grabKeyboard();
        Q_EMIT drag_started(press_);
    }
    Q_EMIT drag_moved(global);
    event->accept();
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && pressed_)
    {
        const bool was_dragging = dragging_;
        end_drag();
        if (was_dragging)
            Q_EMIT drag_finished(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void DockTitleBar::keyPressEvent(QKeyEvent* event)
{
    if (dragging_ && event->key() == Qt::Key_Escape)
    {
        end_drag();
        Q_EMIT drag_cancelled();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void DockTitleBar::end_drag()
{
    if (dragging_)
        releaseKeyboard();
    pressed_ = false;
    dragging_ = false;
}

// ------------------------------------------------------------------ overlay

DockDropOverlay::DockDropOverlay(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("dockDropOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(tr("Drop guides"));
    setAccessibleDescription(
        tr("Shows where the pane will go. Use the pane menu to move a pane "
           "without the mouse."));
    hide();
}

int DockDropOverlay::guide_size()
{
    return Theme::instance().metric(QStringLiteral("buttonSmall")) + 8;
}

int DockDropOverlay::guide_gap()
{
    return Theme::instance().space(0);
}

int DockDropOverlay::guide_margin()
{
    return Theme::instance().space(1);
}

void DockDropOverlay::show_guides(const QVector<QRect>& panes,
                                  const QRect& area)
{
    panes_ = panes;
    area_ = area;
    hover_pane_ = {};
    preview_ = {};
    zone_ = drop_zone::none;
    setGeometry(parentWidget()->rect());
    raise();
    show();
    update();
}

void DockDropOverlay::set_hover(const QRect& pane, drop_zone zone)
{
    hover_pane_ = pane;
    zone_ = zone;
    preview_ = preview_rect(zone, pane, area_);
    update();
}

void DockDropOverlay::hide_guides()
{
    panes_.clear();
    preview_ = {};
    zone_ = drop_zone::none;
    hide();
}

void DockDropOverlay::paintEvent(QPaintEvent*)
{
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (preview_.isValid())
    {
        QColor fill = theme.colour(colour_role::accent);
        fill.setAlpha(60);
        painter.setPen(QPen(theme.colour(colour_role::accent), 2));
        painter.setBrush(fill);
        const int radius = theme.radius(QStringLiteral("pane"));
        painter.drawRoundedRect(QRectF(preview_).adjusted(2, 2, -2, -2),
                                radius, radius);
    }

    const int size = guide_size();
    const auto draw = [&](const drop_guide& guide, bool hot)
    {
        const QRectF box(guide.rect);
        painter.setPen(QPen(theme.colour(hot ? colour_role::accent
                                             : colour_role::line),
                            hot ? 2 : 1));
        painter.setBrush(theme.colour(hot ? colour_role::accentSoft
                                          : colour_role::panel));
        painter.drawRoundedRect(box, theme.radius(QStringLiteral("control")),
                                theme.radius(QStringLiteral("control")));
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.colour(hot ? colour_role::accentStrong
                                          : colour_role::ink2));
        const QPointF centre = box.center();
        const qreal unit = size / 4.0;
        if (guide.zone == drop_zone::centre)
        {
            painter.drawRoundedRect(
                QRectF(centre.x() - unit, centre.y() - unit, unit * 2,
                       unit * 2),
                2, 2);
            return;
        }
        // The pane glyph: the half the pane would take is filled.
        const QRectF frame(centre.x() - unit * 1.2, centre.y() - unit * 1.2,
                           unit * 2.4, unit * 2.4);
        painter.setPen(QPen(painter.brush().color(), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.colour(hot ? colour_role::accentStrong
                                          : colour_role::ink2));
        const QString name = arrow_name(guide.zone);
        QRectF half = frame;
        if (name == QLatin1String("left"))
            half.setWidth(frame.width() / 2);
        else if (name == QLatin1String("right"))
            half.setLeft(frame.center().x());
        else if (name == QLatin1String("top"))
            half.setHeight(frame.height() / 2);
        else
            half.setTop(frame.center().y());
        painter.drawRect(half);
    };

    // Guides show over every pane that can be dropped on, so the choice is
    // visible before the cursor reaches one; only the pane under the cursor
    // highlights a guide.
    for (const QRect& pane : std::as_const(panes_))
    {
        const bool under_cursor = pane == hover_pane_;
        painter.setOpacity(under_cursor || zone_ == drop_zone::none ? 1.0
                                                                    : 0.6);
        for (const drop_guide& guide : pane_guides(pane, size, guide_gap()))
            draw(guide, under_cursor && guide.zone == zone_);
    }
    painter.setOpacity(1.0);
    for (const drop_guide& guide : window_guides(area_, size, guide_margin()))
        draw(guide, guide.zone == zone_);
}

// --------------------------------------------------------------- controller

DockPanes::DockPanes(QMainWindow* window) : QObject(window), window_(window)
{
    overlay_ = new DockDropOverlay(window);
    // Qt's own tab bars drag a tab by themselves; watching them lets a tab
    // drag show the same guides as a title bar drag.
    qApp->installEventFilter(this);
}

void DockPanes::manage(QDockWidget* dock)
{
    if (dock == nullptr || docks_.contains(dock))
        return;
    docks_.push_back(dock);
    dock->setFeatures(QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    auto* bar = new DockTitleBar(dock, dock);
    dock->installEventFilter(this);
    connect(bar, &DockTitleBar::close_requested, this,
            [this, dock] { close_pane(dock); });
    dock->setTitleBarWidget(bar);
    connect(bar, &DockTitleBar::menu_requested, this,
            [this, dock](QMenu* menu) { fill_menu(dock, menu); });
    connect(bar, &DockTitleBar::drag_started, this,
            [this, dock](const QPoint& global) { begin_drag(dock, global); });
    connect(bar, &DockTitleBar::drag_moved, this,
            [this](const QPoint& global) { update_drag(global); });
    connect(bar, &DockTitleBar::drag_finished, this,
            [this](const QPoint& global) { finish_drag(global); });
    connect(bar, &DockTitleBar::drag_cancelled, this,
            [this] { cancel_drag(); });
}

void DockPanes::fill_menu(QDockWidget* dock, QMenu* menu)
{
    menu->clear();
    QAction* floating = menu->addAction(
        dock->isFloating() ? tr("Dock") : tr("Float"));
    connect(floating, &QAction::triggered, dock,
            [this, dock]
            {
                if (dock->isFloating())
                    move_to_edge(dock, Qt::BottomDockWidgetArea);
                else
                    float_pane(dock);
            });
    QMenu* edges = menu->addMenu(tr("Move to the window edge"));
    const struct
    {
        QString text;
        Qt::DockWidgetArea area;
    } entries[] = {{tr("Left"), Qt::LeftDockWidgetArea},
                   {tr("Right"), Qt::RightDockWidgetArea},
                   {tr("Top"), Qt::TopDockWidgetArea},
                   {tr("Bottom"), Qt::BottomDockWidgetArea}};
    for (const auto& entry : entries)
    {
        QAction* action = edges->addAction(entry.text);
        const auto area = entry.area;
        connect(action, &QAction::triggered, dock,
                [this, dock, area] { move_to_edge(dock, area); });
    }
    bool any = false;
    for (QDockWidget* other : targets_for(dock))
    {
        if (!any)
            menu->addSeparator();
        any = true;
        QMenu* sub = menu->addMenu(tr("Beside %1").arg(other->windowTitle()));
        QAction* tab = sub->addAction(tr("Tab with %1").arg(other->windowTitle()));
        connect(tab, &QAction::triggered, dock,
                [this, dock, other] { tab_with(dock, other); });
        sub->addSeparator();
        const struct
        {
            QString text;
            drop_zone zone;
        } sides[] = {{tr("Left of %1"), drop_zone::left},
                     {tr("Right of %1"), drop_zone::right},
                     {tr("Above %1"), drop_zone::top},
                     {tr("Below %1"), drop_zone::bottom}};
        for (const auto& side : sides)
        {
            QAction* action = sub->addAction(side.text.arg(other->windowTitle()));
            const auto zone = side.zone;
            connect(action, &QAction::triggered, dock,
                    [this, dock, other, zone]
                    { split_beside(dock, other, zone); });
        }
    }
    menu->addSeparator();
    QAction* close = menu->addAction(tr("Close"));
    connect(close, &QAction::triggered, dock,
            [this, dock] { close_pane(dock); });
}

QVector<QDockWidget*> DockPanes::targets_for(QDockWidget* dragged) const
{
    QVector<QDockWidget*> result;
    for (const auto& dock : docks_)
    {
        if (dock == nullptr || dock == dragged || dock->isFloating() ||
            !dock->isVisible())
            continue;
        result.push_back(dock);
    }
    return result;
}

QRect DockPanes::dock_rect(const QDockWidget* dock) const
{
    return QRect(dock->mapTo(window_, QPoint(0, 0)), dock->size());
}

QRect DockPanes::drop_area() const
{
    // The dock area is the window without the menu, tool and status bars.
    QRect area = window_->rect();
    if (QWidget* menu = window_->menuWidget())
        area.setTop(menu->geometry().bottom() + 1);
    if (QWidget* status = window_->findChild<QWidget*>(
            QStringLiteral("statusBar")))
    {
        if (status->isVisible())
            area.setBottom(status->geometry().top() - 1);
    }
    return area;
}

QDockWidget* DockPanes::target_at(const QPoint& window_point,
                                  QDockWidget* dragged) const
{
    for (QDockWidget* dock : targets_for(dragged))
    {
        if (dock_rect(dock).contains(window_point))
            return dock;
    }
    return nullptr;
}

void DockPanes::begin_drag(QDockWidget* dock, const QPoint& global)
{
    if (dock == nullptr)
        return;
    dragged_ = dock;
    hover_target_ = nullptr;
    grab_offset_ = global - dock->mapToGlobal(QPoint(0, 0));
    QVector<QRect> panes;
    for (QDockWidget* target : targets_for(dock))
        panes.push_back(dock_rect(target));
    overlay_->show_guides(panes, drop_area());
    evaluate(global);
}

void DockPanes::evaluate(const QPoint& global)
{
    const QPoint point = window_->mapFromGlobal(global);
    const QRect area = drop_area();
    QDockWidget* target = target_at(point, dragged_);
    hover_target_ = target;
    const QRect pane = target != nullptr ? dock_rect(target) : QRect();
    const int size = DockDropOverlay::guide_size();
    const drop_zone zone =
        zone_at(point, pane, area, size, DockDropOverlay::guide_gap(),
                DockDropOverlay::guide_margin());
    overlay_->set_hover(pane, zone);
}

void DockPanes::update_drag(const QPoint& global)
{
    if (dragged_ == nullptr)
        return;
    // A floating pane follows the cursor.
    if (dragged_->isFloating())
        dragged_->move(global - grab_offset_);
    evaluate(global);
}

drop_zone DockPanes::current_zone() const noexcept
{
    return overlay_ != nullptr ? overlay_->hover_zone() : drop_zone::none;
}

void DockPanes::cancel_drag()
{
    dragged_ = nullptr;
    hover_target_ = nullptr;
    overlay_->hide_guides();
}

void DockPanes::finish_drag(const QPoint& global)
{
    QDockWidget* dock = dragged_;
    if (dock == nullptr)
        return;
    evaluate(global);
    const drop_zone zone = overlay_->hover_zone();
    QDockWidget* target = hover_target_;
    const QPoint window_point = window_->mapFromGlobal(global);
    const bool inside = window_->rect().contains(window_point);
    cancel_drag();
    if (zone == drop_zone::none)
    {
        // Let go outside the window and the pane floats there; let go over
        // the window with no guide and nothing changes.
        if (!inside)
        {
            if (!dock->isFloating())
                dock->setFloating(true);
            dock->move(global - grab_offset_);
        }
        return;
    }
    if (is_window_zone(zone))
    {
        switch (zone)
        {
        case drop_zone::window_left:
            move_to_edge(dock, Qt::LeftDockWidgetArea);
            break;
        case drop_zone::window_right:
            move_to_edge(dock, Qt::RightDockWidgetArea);
            break;
        case drop_zone::window_top:
            move_to_edge(dock, Qt::TopDockWidgetArea);
            break;
        default:
            move_to_edge(dock, Qt::BottomDockWidgetArea);
            break;
        }
        return;
    }
    if (target == nullptr)
        return;
    if (zone == drop_zone::centre)
        tab_with(dock, target);
    else
        split_beside(dock, target, zone);
}

void DockPanes::move_to_edge(QDockWidget* dock, Qt::DockWidgetArea area)
{
    if (dock->isFloating())
        dock->setFloating(false);
    window_->removeDockWidget(dock);
    window_->addDockWidget(area, dock);
    dock->show();
    dock->raise();
    refresh_tab_bars();
    Q_EMIT layout_changed();
}

void DockPanes::tab_with(QDockWidget* dock, QDockWidget* target)
{
    if (dock == target)
        return;
    if (dock->isFloating())
        dock->setFloating(false);
    window_->removeDockWidget(dock);
    window_->tabifyDockWidget(target, dock);
    dock->show();
    dock->raise();
    refresh_tab_bars();
    Q_EMIT layout_changed();
}

void DockPanes::split_beside(QDockWidget* dock, QDockWidget* target,
                             drop_zone zone)
{
    if (dock == target)
        return;
    const Qt::Orientation orientation = orientation_of(zone);
    if (dock->isFloating())
        dock->setFloating(false);
    const bool before = zone == drop_zone::left || zone == drop_zone::top;
    const Qt::DockWidgetArea area = window_->dockWidgetArea(target);

    // Qt puts a dock split from a neighbour that was ever in a tab group into
    // that group, so the neighbour and the rest of its group are taken out
    // and put back as plain items: the neighbour first, the dock beside it,
    // then the rest of the group tabbed with the neighbour as before.
    QList<QDockWidget*> group = window_->tabifiedDockWidgets(target);
    group.removeAll(dock);
    QList<QDockWidget*> open_siblings;
    for (QDockWidget* sibling : std::as_const(group))
    {
        if (!sibling->isHidden())
            open_siblings.push_back(sibling);
        window_->removeDockWidget(sibling);
    }
    window_->removeDockWidget(dock);
    window_->removeDockWidget(target);
    // Qt has no "before", so the dock goes in first when it is to be before.
    QDockWidget* first = before ? dock : target;
    QDockWidget* second = before ? target : dock;
    window_->addDockWidget(area, first);
    window_->splitDockWidget(first, second, orientation);
    for (QDockWidget* sibling : std::as_const(group))
    {
        window_->tabifyDockWidget(target, sibling);
        sibling->setVisible(open_siblings.contains(sibling));
    }
    dock->show();
    target->show();
    target->raise();
    dock->raise();
    const int total = orientation == Qt::Horizontal
                          ? std::max(target->width() + dock->width(), 2)
                          : std::max(target->height() + dock->height(), 2);
    window_->resizeDocks({first, second}, {total / 2, total - total / 2},
                         orientation);
    refresh_tab_bars();
    Q_EMIT layout_changed();
}

void DockPanes::refresh_tab_bars()
{
    // Taking a tab group apart can leave Qt's old tab bar showing over the
    // new layout. It is hidden once the layout has settled.
    QTimer::singleShot(0, this, [this] { hide_stale_tab_bars(); });
}

void DockPanes::hide_stale_tab_bars()
{
    // The tab groups that exist now, each as its sorted list of titles.
    QList<QStringList> groups;
    for (QDockWidget* dock : window_->findChildren<QDockWidget*>())
    {
        if (dock->isHidden())
            continue;
        QStringList titles{dock->windowTitle()};
        for (const QDockWidget* other : window_->tabifiedDockWidgets(dock))
        {
            if (!other->isHidden())
                titles.push_back(other->windowTitle());
        }
        if (titles.size() < 2)
            continue;
        titles.sort();
        groups.push_back(titles);
    }
    for (QTabBar* bar : window_->findChildren<QTabBar*>(
             QString(), Qt::FindDirectChildrenOnly))
    {
        if (!bar->isVisible() || bar->count() == 0)
            continue;
        QStringList titles;
        for (int index = 0; index < bar->count(); ++index)
            titles.push_back(bar->tabText(index));
        titles.sort();
        if (!groups.contains(titles))
            bar->hide();
    }
}

void DockPanes::close_pane(QDockWidget* dock)
{
    dock->hide();
    Q_EMIT pane_closed(dock);
}

bool DockPanes::tab_bar_event(QTabBar* bar, QEvent* event)
{
    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        tab_dock_ = nullptr;
        tab_dragging_ = false;
        if (mouse->button() != Qt::LeftButton)
            return false;
        const int index = bar->tabAt(mouse->position().toPoint());
        if (index < 0)
            return false;
        // The tab's data is the dock widget it stands for.
        const quintptr id = bar->tabData(index).value<quintptr>();
        for (const auto& dock : docks_)
        {
            if (reinterpret_cast<quintptr>(dock.data()) == id)
                tab_dock_ = dock;
        }
        tab_bar_ = bar;
        tab_press_ = mouse->globalPosition().toPoint();
        return false;
    }
    case QEvent::MouseMove:
    {
        if (tab_dock_ == nullptr || tab_bar_ != bar)
            return false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint global = mouse->globalPosition().toPoint();
        if (!tab_dragging_)
        {
            if ((global - tab_press_).manhattanLength() <
                QApplication::startDragDistance())
                return false;
            tab_dragging_ = true;
            begin_drag(tab_dock_, tab_press_);
        }
        update_drag(global);
        return true;
    }
    case QEvent::MouseButtonRelease:
    {
        if (!tab_dragging_ || tab_bar_ != bar)
        {
            tab_dock_ = nullptr;
            return false;
        }
        auto* mouse = static_cast<QMouseEvent*>(event);
        tab_dragging_ = false;
        tab_dock_ = nullptr;
        finish_drag(mouse->globalPosition().toPoint());
        return true;
    }
    default:
        return false;
    }
}

bool DockPanes::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* bar = qobject_cast<QTabBar*>(watched);
        bar != nullptr && bar->parent() == window_)
    {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress ||
            type == QEvent::MouseMove || type == QEvent::MouseButtonRelease)
        {
            if (tab_bar_event(bar, event))
                return true;
        }
    }
    if (tab_dragging_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        tab_dragging_ = false;
        tab_dock_ = nullptr;
        cancel_drag();
        return true;
    }
    // Closing a floating pane from the window system.
    if (event->type() == QEvent::Close)
    {
        if (auto* dock = qobject_cast<QDockWidget*>(watched);
            dock != nullptr && docks_.contains(dock))
            Q_EMIT pane_closed(dock);
    }
    return QObject::eventFilter(watched, event);
}

void DockPanes::float_pane(QDockWidget* dock)
{
    if (dock->isFloating())
        return;
    const QRect rect(dock->mapToGlobal(QPoint(0, 0)), dock->size());
    dock->setFloating(true);
    dock->setGeometry(rect.translated(QPoint(24, 24)));
    dock->show();
    Q_EMIT layout_changed();
}

void DockPanes::cycle(int step)
{
    QVector<QDockWidget*> open;
    for (const auto& dock : docks_)
    {
        if (is_open(dock))
            open.push_back(dock);
    }
    if (open.isEmpty())
        return;
    qsizetype current = -1;
    for (qsizetype index = 0; index < open.size(); ++index)
    {
        if (open[index]->isAncestorOf(QApplication::focusWidget()) ||
            open[index] == QApplication::focusWidget())
            current = index;
    }
    const qsizetype count = open.size();
    const qsizetype next = ((current + step) % count + count) % count;
    QDockWidget* dock = open[next];
    dock->show();
    dock->raise();
    if (QWidget* body = dock->widget())
        body->setFocus(Qt::TabFocusReason);
}

int DockPanes::rescue_offscreen()
{
    int moved = 0;
    for (const auto& dock : docks_)
    {
        if (dock == nullptr || !dock->isFloating())
            continue;
        const QRect frame = dock->frameGeometry();
        bool seen = false;
        for (const QScreen* screen : QGuiApplication::screens())
        {
            const QRect common = screen->availableGeometry().intersected(frame);
            if (common.width() >= 48 && common.height() >= 24)
                seen = true;
        }
        if (!seen)
        {
            dock->setFloating(false);
            window_->addDockWidget(Qt::BottomDockWidgetArea, dock);
            ++moved;
        }
    }
    return moved;
}

} // namespace vss::gui
