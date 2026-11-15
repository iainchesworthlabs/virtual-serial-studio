//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_THEME_FOCUS_RING_STYLE_HPP
#define VSS_GUI_THEME_FOCUS_RING_STYLE_HPP

#include <QProxyStyle>
#include <QRectF>

class QEvent;
class QPainter;
class QStyle;

namespace vss::gui
{

/// Fusion plus the keyboard-only focus ring the design specifies.
///
/// QSS cannot distinguish keyboard focus from a mouse click, so the ring is
/// painted here instead.  The ring shows while the user is driving the
/// application from the keyboard: a key press (other than a lone modifier)
/// turns it on, and a mouse press or touch turns it off.  Window activation,
/// popups, menus and a dialog taking the focus do not change it, so the ring
/// is still there after Alt+Tab and on the default button of a dialog that
/// was opened with Enter.
///
/// The ring follows the outer shape of the control it marks: the widget's
/// rectangle, with the 2 px stroke inside it so a layout never clips it, and
/// the control's corner radius.  The style sheet hands the style the
/// rectangle of a button's contents (its text, inside the padding), so the
/// style takes the geometry from the widget and not from the style option.
class FocusRingStyle final : public QProxyStyle
{
public:
    /// The ring's stroke width, from `focusRing.width` in the tokens.
    static constexpr qreal kRingWidth = 2.0;

    explicit FocusRingStyle(QStyle* base = nullptr);

    void install();
    /// True while the user is driving the application from the keyboard.
    static bool keyboard_focus();
    /// Draws the ring inside `bounds`, the outer rectangle of the control.
    /// `outer_radius` is the radius of the control's own corners.  A control
    /// with a solid fill the same colour as the ring (the primary button)
    /// passes `separate` to get a thin gap between the ring and the fill.
    static void paint_ring(QPainter& painter, const QRectF& bounds,
                           qreal outer_radius, bool separate = false);

    bool eventFilter(QObject* watched, QEvent* event) override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override;

    QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                           const QSize& size,
                           const QWidget* widget = nullptr) const override;

    QIcon standardIcon(StandardPixmap standard,
                       const QStyleOption* option = nullptr,
                       const QWidget* widget = nullptr) const override;

private:
    static bool draw_chevron(PrimitiveElement element,
                             const QStyleOption* option, QPainter* painter);
    static void set_keyboard_focus(bool keyboard);
    /// Paints the ring over a slider or a plain tool button, which the style
    /// sheet draws completely itself and so never asks the style for a focus
    /// rectangle.  Returns true when it handled the paint event.
    /// Makes the line edit inside a spin box or an editable combo box take
    /// the application style sheet's rules.
    static void style_embedded_line_edit(QObject* watched, QEvent* event);
    static bool paint_over(QObject* watched, QEvent* event);

    static bool keyboard_;
    bool installed_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_THEME_FOCUS_RING_STYLE_HPP
