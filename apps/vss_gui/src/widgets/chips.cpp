//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/chips.hpp"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QSizePolicy>

#include <algorithm>

namespace vss::gui
{
namespace
{

struct chip_palette
{
    QColor fill;
    QColor edge;
    QColor text;
    QColor mark;
    bool hollow = false;
    bool cross = false;
    bool mark_visible = true;
};

chip_palette palette_for(StatusChip::kind kind)
{
    // Chips are tinted capsules with no border: the tint, the mark shape and
    // the text carry the state together.
    const Theme& theme = Theme::instance();
    const QColor none = Qt::transparent;
    switch (kind)
    {
    case StatusChip::kind::open:
    case StatusChip::kind::healthy:
    case StatusChip::kind::connected:
        return {theme.colour(colour_role::okSoft),
                none,
                theme.colour(colour_role::ok),
                theme.colour(colour_role::ok),
                false,
                false,
                true};
    case StatusChip::kind::waiting:
        return {theme.colour(colour_role::amberSoft),
                none,
                theme.colour(colour_role::amber),
                theme.colour(colour_role::amber),
                true,
                false,
                true};
    case StatusChip::kind::studio_only:
    case StatusChip::kind::closed:
        return {theme.colour(colour_role::fill),
                none,
                theme.colour(colour_role::ink2),
                theme.colour(colour_role::ink3),
                true,
                false,
                true};
    case StatusChip::kind::neutral:
        return {theme.colour(colour_role::fill),
                none,
                theme.colour(colour_role::ink2),
                theme.colour(colour_role::ink3),
                false,
                false,
                true};
    case StatusChip::kind::failed:
        return {theme.colour(colour_role::badSoft),
                none,
                theme.colour(colour_role::bad),
                theme.colour(colour_role::bad),
                false,
                true,
                true};
    case StatusChip::kind::fixture:
        return {theme.colour(colour_role::violetSoft),
                none,
                theme.colour(colour_role::violet),
                theme.colour(colour_role::violet),
                false,
                false,
                false};
    }
    return {};
}

int text_width(const QFont& font, const QString& text)
{
    return QFontMetrics(font).horizontalAdvance(text);
}

} // namespace

StatusChip::StatusChip(QWidget* parent) : QLabel(parent)
{
    setFont(Theme::instance().font(text_style::caption));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    repaint_on_theme_change(this);
}

StatusChip::StatusChip(const QString& text, kind value, QWidget* parent)
    : StatusChip(parent)
{
    set_kind(value);
    set_text(text);
}

void StatusChip::set_kind(kind value)
{
    kind_ = value;
    update();
}

void StatusChip::set_text(const QString& text)
{
    setText(text);
    setAccessibleName(text);
    updateGeometry();
    update();
}

void StatusChip::set_small(bool small)
{
    if (small_ == small)
        return;
    small_ = small;
    updateGeometry();
    update();
}

QSize StatusChip::sizeHint() const
{
    const int height = Theme::instance().metric(
        small_ ? QStringLiteral("chipSmall") : QStringLiteral("chip"));
    const chip_palette palette = palette_for(kind_);
    int width = 24 + text_width(font(), text());
    if (palette.mark_visible)
        width += 14;
    return QSize(width, height);
}

QSize StatusChip::minimumSizeHint() const
{
    return sizeHint();
}

void StatusChip::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const chip_palette palette = palette_for(kind_);
    const int height = Theme::instance().metric(
        small_ ? QStringLiteral("chipSmall") : QStringLiteral("chip"));

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect());
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.fill);
    painter.drawRoundedRect(frame, height / 2.0, height / 2.0);

    int x = 12;
    const int centre_y = height / 2;
    if (palette.mark_visible)
    {
        const QRectF mark(x, centre_y - 4, 8, 8);
        painter.setPen(QPen(palette.mark, 1.5));
        painter.setBrush(Qt::NoBrush);
        if (palette.cross)
        {
            painter.drawLine(mark.topLeft(), mark.bottomRight());
            painter.drawLine(mark.topRight(), mark.bottomLeft());
        }
        else if (palette.hollow)
        {
            painter.drawEllipse(mark.adjusted(0.75, 0.75, -0.75, -0.75));
        }
        else
        {
            painter.setBrush(palette.mark);
            painter.drawEllipse(mark);
        }
        x += 14;
    }

    painter.setPen(palette.text);
    painter.setFont(font());
    painter.drawText(QRect(x, 0, width() - x - 12, height),
                     Qt::AlignLeft | Qt::AlignVCenter, text());
}

DirectionBadge::DirectionBadge(QWidget* parent) : QLabel(parent)
{
    setFont(Theme::instance().font(text_style::data));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    set_direction(direction::rx);
    repaint_on_theme_change(this);
}

DirectionBadge::DirectionBadge(direction value, QWidget* parent)
    : DirectionBadge(parent)
{
    set_direction(value);
}

void DirectionBadge::set_direction(direction value)
{
    direction_ = value;
    QString label;
    QString accessible;
    switch (value)
    {
    case direction::rx:
        label = QObject::tr("RX");
        accessible = QObject::tr("received");
        break;
    case direction::tx:
        label = QObject::tr("TX");
        accessible = QObject::tr("sent");
        break;
    case direction::drop:
        label = QObject::tr("DROP");
        accessible = QObject::tr("dropped");
        break;
    }
    QLabel::setText(label);
    setAccessibleName(accessible);
    updateGeometry();
    update();
}

QSize DirectionBadge::sizeHint() const
{
    return QSize(text_width(font(), text()) + 10, 18);
}

QSize DirectionBadge::minimumSizeHint() const
{
    return sizeHint();
}

void DirectionBadge::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    // RX is the accent tint and TX the violet tint, so the two directions
    // are told apart by colour as well as by their text.
    QColor fill = theme.colour(colour_role::accentSoft);
    QColor ink = theme.colour(colour_role::accentText);
    switch (direction_)
    {
    case direction::rx:
        break;
    case direction::tx:
        fill = theme.colour(colour_role::violetSoft);
        ink = theme.colour(colour_role::violet);
        break;
    case direction::drop:
        fill = theme.colour(colour_role::badSoft);
        ink = theme.colour(colour_role::bad);
        break;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()), 5, 5);
    painter.setPen(ink);
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text());
}

Tag::Tag(QWidget* parent) : QLabel(parent)
{
    setFont(Theme::instance().font(text_style::caption));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    repaint_on_theme_change(this);
}

Tag::Tag(const QString& text, variant value, QWidget* parent) : Tag(parent)
{
    set_variant(value);
    set_text(text);
}

void Tag::set_variant(variant value)
{
    variant_ = value;
    update();
}

void Tag::set_text(const QString& text)
{
    QLabel::setText(text);
    setAccessibleName(text);
    updateGeometry();
    update();
}

QSize Tag::sizeHint() const
{
    return QSize(text_width(font(), text()) + 12, 20);
}

QSize Tag::minimumSizeHint() const
{
    return sizeHint();
}

void Tag::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QColor fill = theme.colour(colour_role::accentSoft);
    QColor edge = theme.colour(colour_role::accentLine);
    QColor ink = theme.colour(colour_role::accentText);
    Qt::PenStyle style = Qt::SolidLine;
    switch (variant_)
    {
    case variant::s:
        break;
    case variant::a:
        fill = theme.colour(colour_role::amberSoft);
        edge = theme.colour(colour_role::amberLine);
        ink = theme.colour(colour_role::noteWarnText);
        break;
    case variant::n:
        fill = Qt::transparent;
        edge = theme.colour(colour_role::line2);
        ink = theme.colour(colour_role::ink2);
        style = Qt::DashLine;
        break;
    case variant::ok:
        fill = theme.colour(colour_role::okSoft);
        edge = theme.colour(colour_role::okLine);
        ink = theme.colour(colour_role::noteOkText);
        break;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(edge, 1.0);
    pen.setStyle(style);
    // Only the dashed tag keeps an outline; the others are tints.
    painter.setPen(variant_ == variant::n ? pen : QPen(Qt::NoPen));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4,
                            4);
    painter.setPen(ink);
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text());
}

Badge::Badge(const QString& text, QWidget* parent) : QLabel(parent)
{
    setFont(Theme::instance().font(text_style::caption));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    set_text(text);
    repaint_on_theme_change(this);
}

void Badge::set_text(const QString& text)
{
    const QString upper = text.toUpper();
    QLabel::setText(upper);
    setAccessibleName(upper);
    updateGeometry();
    update();
}

QSize Badge::sizeHint() const
{
    return QSize(text_width(font(), text()) + 10, 20);
}

QSize Badge::minimumSizeHint() const
{
    return sizeHint();
}

void Badge::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.colour(colour_role::amberSoft));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4,
                            4);
    painter.setPen(theme.colour(colour_role::noteWarnText));
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text());
}

StateDot::StateDot(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    repaint_on_theme_change(this);
}

void StateDot::set_kind(kind value)
{
    kind_ = value;
    update();
}

void StateDot::set_colour(colour_role role)
{
    colour_ = role;
    update();
}

QSize StateDot::sizeHint() const
{
    return QSize(8, 8);
}

QSize StateDot::minimumSizeHint() const
{
    return sizeHint();
}

void StateDot::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor colour = Theme::instance().colour(colour_);
    const QRectF dot = QRectF(0.5, 0.5, 7, 7);
    painter.setPen(QPen(colour, 1.5));
    painter.setBrush(Qt::NoBrush);
    if (kind_ == kind::filled)
    {
        painter.setBrush(colour);
        painter.drawEllipse(dot);
    }
    else if (kind_ == kind::hollow)
    {
        painter.drawEllipse(dot);
    }
    else
    {
        painter.drawLine(dot.topLeft(), dot.bottomRight());
        painter.drawLine(dot.topRight(), dot.bottomLeft());
    }
}

KeyCap::KeyCap(const QString& text, QWidget* parent) : QLabel(parent)
{
    setFont(Theme::instance().font(text_style::data));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    set_text(text);
    repaint_on_theme_change(this);
}

void KeyCap::set_text(const QString& text)
{
    QLabel::setText(text);
    setAccessibleName(text);
    updateGeometry();
    update();
}

QSize KeyCap::sizeHint() const
{
    const QFontMetrics metrics(font());
    return QSize(metrics.horizontalAdvance(text()) + 8, metrics.height() + 4);
}

QSize KeyCap::minimumSizeHint() const
{
    return sizeHint();
}

void KeyCap::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme.colour(colour_role::line2), 1.0));
    painter.setBrush(theme.colour(colour_role::sunken));
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -1.0);
    painter.drawRoundedRect(frame, 3, 3);
    painter.setPen(theme.colour(colour_role::ink2));
    painter.setFont(font());
    painter.drawText(frame, Qt::AlignCenter, text());
}

} // namespace vss::gui
