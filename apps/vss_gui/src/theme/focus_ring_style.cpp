//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "theme/focus_ring_style.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOptionFocusRect>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <typeinfo>

namespace vss::gui
{

bool FocusRingStyle::keyboard_ = false;

namespace
{

/// A key that says nothing about how the user is moving around: a modifier or
/// a lock key pressed on its own.
bool is_lone_modifier(int key)
{
    switch (key)
    {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Direction_L:
    case Qt::Key_Direction_R:
    case Qt::Key_unknown:
        return true;
    default:
        return false;
    }
}

bool in_tool_bar(const QWidget* widget)
{
    for (const QWidget* parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget())
    {
        if (qobject_cast<const QToolBar*>(parent) != nullptr)
            return true;
    }
    return false;
}

/// A label whose links can be reached with the keyboard.  It takes focus, but
/// Qt draws no mark for the focused link, so the ring goes round the label.
bool has_keyboard_links(const QWidget* widget)
{
    const auto* label = qobject_cast<const QLabel*>(widget);
    return label != nullptr &&
           (label->textInteractionFlags() & Qt::LinksAccessibleByKeyboard) != 0;
}

/// Where a `PE_FrameFocusRect` ring goes.
struct ring_geometry
{
    QRectF bounds;
    qreal radius = 0.0;
    bool separate = false;
};

/// Works out the ring for a focus rectangle.  A button (push, check box,
/// radio) gets its own widget rectangle, because the style sheet hands the
/// style the rectangle of the contents, which is the text inside the padding.
/// Item views and anything else keep the rectangle the style option names:
/// the current item, or a link's text.  Returns false when the style sheet
/// marks the focus itself.
bool ring_for(const QStyleOption* option, const QWidget* widget,
              ring_geometry& result)
{
    result.radius = Theme::instance().radius(QStringLiteral("control"));
    result.bounds = QRectF(option->rect);
    if (qobject_cast<const QAbstractItemView*>(widget) != nullptr)
        return true;
    if (qobject_cast<const QTabBar*>(widget) != nullptr)
    {
        // With the style sheet on, the current tab marks its own focus (the
        // QTabBar::tab:focus rule).  Without it, draw around the label.
        return qApp->styleSheet().isEmpty();
    }
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget))
    {
        result.bounds = QRectF(button->rect());
        const QString role = button->property("role").toString();
        result.separate =
            role == QLatin1String("primary") || role == QLatin1String("danger");
    }
    return true;
}

} // namespace

FocusRingStyle::FocusRingStyle(QStyle* base)
    : QProxyStyle(base != nullptr
                      ? base
                      : QStyleFactory::create(QStringLiteral("Fusion")))
{
}

void FocusRingStyle::install()
{
    if (installed_ || qApp == nullptr)
        return;
    qApp->installEventFilter(this);
    installed_ = true;
}

void FocusRingStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    // The ring goes round a link label, so its text keeps the room the ring
    // and its gap need from the label's edge.
    if (auto* label = qobject_cast<QLabel*>(widget);
        label != nullptr && has_keyboard_links(label))
    {
        const int room = Theme::instance().space(0);
        if (label->margin() < room)
            label->setMargin(room);
    }
}

bool FocusRingStyle::keyboard_focus()
{
    return keyboard_;
}

void FocusRingStyle::set_keyboard_focus(bool keyboard)
{
    if (keyboard_ == keyboard)
        return;
    keyboard_ = keyboard;
    // The focused control is not told that the ring has appeared or gone.
    if (QWidget* focused = QApplication::focusWidget())
    {
        focused->update();
        if (auto* area = qobject_cast<QAbstractScrollArea*>(focused))
            area->viewport()->update();
    }
}

bool FocusRingStyle::eventFilter(QObject* watched, QEvent* event)
{
    if (event == nullptr)
        return false;
    switch (event->type())
    {
    case QEvent::KeyPress:
    case QEvent::ShortcutOverride:
        // A shortcut arrives as a ShortcutOverride and is not followed by a
        // key press, so both count.
        if (const auto* key = dynamic_cast<const QKeyEvent*>(event);
            key != nullptr && !is_lone_modifier(key->key()))
            set_keyboard_focus(true);
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        set_keyboard_focus(false);
        break;
    case QEvent::FocusIn:
        // Tab, Backtab and a mnemonic are the keyboard; a click is the mouse.
        // Window activation, popups, menus and Other say nothing about it.
        // QDialog sends its first widget a Tab focus event when it is shown,
        // before the window has the focus; that is not the keyboard, and the
        // widget does not have the focus yet.
        if (const auto* focus = dynamic_cast<const QFocusEvent*>(event);
            focus != nullptr && watched->isWidgetType() &&
            static_cast<const QWidget*>(watched)->hasFocus())
        {
            const Qt::FocusReason reason = focus->reason();
            if (reason == Qt::TabFocusReason ||
                reason == Qt::BacktabFocusReason ||
                reason == Qt::ShortcutFocusReason)
                set_keyboard_focus(true);
            else if (reason == Qt::MouseFocusReason)
                set_keyboard_focus(false);
        }
        break;
    case QEvent::Paint:
        if (paint_over(watched, event))
            return true;
        break;
    case QEvent::ChildPolished:
        style_embedded_line_edit(watched, event);
        break;
    default:
        break;
    }
    return QProxyStyle::eventFilter(watched, event);
}

void FocusRingStyle::style_embedded_line_edit(QObject* watched, QEvent* event)
{
    // The style sheet never styles the line edit inside a spin box or an
    // editable combo box; it styles the box itself, and the box places the
    // edit with no regard for its padding.  A widget with a style sheet of
    // its own is styled, so give the edit one that says nothing and let the
    // application style sheet's rules for it apply.
    if (qobject_cast<QAbstractSpinBox*>(watched) == nullptr &&
        qobject_cast<QComboBox*>(watched) == nullptr)
        return;
    auto* child =
        qobject_cast<QLineEdit*>(static_cast<QChildEvent*>(event)->child());
    if (child != nullptr && child->styleSheet().isEmpty())
    {
        child->setStyleSheet(
            QStringLiteral("/* styled by the application style sheet */"));
    }
}

bool FocusRingStyle::paint_over(QObject* watched, QEvent* event)
{
    if (!keyboard_ || watched == nullptr || !watched->isWidgetType())
        return false;
    auto* widget = static_cast<QWidget*>(watched);
    if (!widget->hasFocus())
        return false;
    // Exactly a QToolButton: a subclass draws its own ring.  A tool button in
    // a tool bar marks its focus with the style sheet.
    const bool slider = qobject_cast<QSlider*>(widget) != nullptr;
    const bool tool_button =
        typeid(*widget) == typeid(QToolButton) && !in_tool_bar(widget);
    if (!slider && !tool_button && !has_keyboard_links(widget))
        return false;

    // QObject::event is public where QWidget's is not; this is the virtual
    // call the notification would have made.
    watched->event(event);
    QPainter painter(widget);
    paint_ring(painter, QRectF(widget->rect()),
               Theme::instance().radius(QStringLiteral("control")));
    return true;
}

void FocusRingStyle::paint_ring(QPainter& painter, const QRectF& bounds,
                                qreal outer_radius, bool separate)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);

    // The stroke is centred half its width inside the bounds, so it fills the
    // outer two pixels of the control and the control's corners stay round.
    const qreal half = kRingWidth / 2.0;
    QPen pen(Theme::instance().colour(colour_role::focus), kRingWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    const qreal centre_radius = std::max<qreal>(0.0, outer_radius - half);
    painter.drawRoundedRect(bounds.adjusted(half, half, -half, -half),
                            centre_radius, centre_radius);

    if (separate)
    {
        // A line in the surface colour just inside the ring, so a fill that
        // matches the ring still shows where the ring ends.
        const qreal gap = kRingWidth + half;
        const qreal gap_radius = std::max<qreal>(0.0, outer_radius - gap);
        painter.setPen(
            QPen(Theme::instance().colour(colour_role::panel), kRingWidth));
        painter.drawRoundedRect(bounds.adjusted(gap, gap, -gap, -gap),
                                gap_radius, gap_radius);
    }
    painter.restore();
}

bool FocusRingStyle::draw_chevron(PrimitiveElement element,
                                  const QStyleOption* option, QPainter* painter)
{
    // Fusion's own arrows are filled triangles a few pixels wide; the icon
    // set uses a 1.5 px stroked chevron, so draw the same shape here for the
    // combo box and spin box arrows.
    const bool up = element == PE_IndicatorArrowUp ||
                    element == PE_IndicatorSpinUp ||
                    element == PE_IndicatorSpinPlus;
    const bool enabled = (option->state & State_Enabled) != 0;
    const QColor colour =
        enabled ? Theme::instance().colour(colour_role::ink2)
                : Theme::instance().colour(colour_role::disabledText);

    const QPointF centre = QRectF(option->rect).center();
    const qreal half = std::min<qreal>(4.0, option->rect.width() / 2.0);
    const qreal rise = half * 0.55;
    QPainterPath path;
    if (up)
    {
        path.moveTo(centre.x() - half, centre.y() + rise);
        path.lineTo(centre.x(), centre.y() - rise);
        path.lineTo(centre.x() + half, centre.y() + rise);
    }
    else
    {
        path.moveTo(centre.x() - half, centre.y() - rise);
        path.lineTo(centre.x(), centre.y() + rise);
        path.lineTo(centre.x() + half, centre.y() - rise);
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QPen pen(colour, 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    painter->restore();
    return true;
}

QIcon FocusRingStyle::standardIcon(StandardPixmap standard,
                                   const QStyleOption* option,
                                   const QWidget* widget) const
{
    // The dock title buttons are drawn by the style from standard pixmaps.
    // The icon set has the close mark, so use it rather than the platform's.
    if (standard == SP_DockWidgetCloseButton ||
        standard == SP_TitleBarCloseButton)
    {
        return IconProvider::instance().icon(QStringLiteral("remove"),
                                             colour_role::ink2, 16);
    }
    return QProxyStyle::standardIcon(standard, option, widget);
}

QSize FocusRingStyle::sizeFromContents(ContentsType type,
                                       const QStyleOption* option,
                                       const QSize& size,
                                       const QWidget* widget) const
{
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    // Fusion adds its own frame to a spin box's height.  The style sheet's
    // box (border and padding) is the frame, and it already counts it, so
    // with a style sheet the spin box is the size of a line edit.
    if (type == CT_SpinBox && !qApp->styleSheet().isEmpty())
        result.setHeight(size.height());
    return result;
}

void FocusRingStyle::drawPrimitive(PrimitiveElement element,
                                   const QStyleOption* option,
                                   QPainter* painter,
                                   const QWidget* widget) const
{
    if (element == PE_IndicatorArrowDown || element == PE_IndicatorArrowUp ||
        element == PE_IndicatorSpinDown || element == PE_IndicatorSpinUp ||
        element == PE_IndicatorSpinMinus || element == PE_IndicatorSpinPlus)
    {
        if (option != nullptr && painter != nullptr &&
            draw_chevron(element, option, painter))
            return;
    }

    if (element != PE_FrameFocusRect)
    {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }

    if (!keyboard_ || option == nullptr || painter == nullptr)
        return;
    if ((option->state & State_HasFocus) == 0)
        return;

    ring_geometry geometry;
    if (!ring_for(option, widget, geometry))
        return;
    paint_ring(*painter, geometry.bounds, geometry.radius, geometry.separate);
}

} // namespace vss::gui
