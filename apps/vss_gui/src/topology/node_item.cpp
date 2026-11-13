//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/node_item.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "topology/topology_text.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <utility>

namespace vss::gui::topology
{
namespace
{

/// The kind tile: 32 px, and the gap after it.
constexpr qreal kTileSize = 32.0;
constexpr qreal kTileSpace = kTileSize + 10.0;

QStringList badge_texts(const node_data& data)
{
    QStringList badges;
    if (data.owner)
        badges.append(topology_text::owner_badge());
    if (data.dropped)
        badges.append(topology_text::dropped_badge());
    return badges;
}

qreal badge_area(const node_data& data, const QFont& font)
{
    const QFontMetricsF metrics(font);
    qreal total = 0.0;
    const QStringList badges = badge_texts(data);
    for (const QString& badge : badges)
        total += metrics.horizontalAdvance(badge) + 12.0 + 4.0;
    if (badges.isEmpty() && data.unlinked)
        total =
            metrics.horizontalAdvance(topology_text::unlinked_badge()) + 6.0;
    return total;
}

struct node_measure
{
    QSizeF size;
};

node_measure measure(const node_data& data, bool compact,
                     const QFont& name_font, const QFont& detail_font,
                     const QFont& badge_font)
{
    const Theme& theme = Theme::instance();
    const qreal padding_x = theme.space(2);
    const qreal padding_y = theme.space(1);

    QSizeF base(compact ? 120.0 : 168.0, compact ? 48.0 : 56.0);
    if (!compact && data.large)
        base.setHeight(64.0);

    const QFontMetricsF name_metrics(name_font);
    const QFontMetricsF detail_metrics(detail_font);

    const qreal name_width = name_metrics.horizontalAdvance(data.name) +
                             badge_area(data, badge_font);
    const qreal detail_width = detail_metrics.horizontalAdvance(data.detail);
    const qreal needed_width =
        std::max(name_width, detail_width) + padding_x * 2.0 +
        (compact || data.icon.isEmpty() ? 0.0 : kTileSpace);

    const qreal line_gap = 2.0;
    const qreal needed_height = name_metrics.height() + line_gap +
                                detail_metrics.height() + padding_y * 2.0;

    return {QSizeF(std::max(base.width(), needed_width),
                   std::max(base.height(), needed_height))};
}

void draw_chip(QPainter* painter, const QRectF& rect, const QString& text,
               const QColor& fill, const QColor& edge,
               const QColor& text_colour, const QFont& font)
{
    painter->setPen(QPen(edge, 1.0));
    painter->setBrush(fill);
    painter->drawRoundedRect(rect, 4.0, 4.0);
    painter->setPen(text_colour);
    painter->setFont(font);
    painter->drawText(rect, Qt::AlignCenter, text);
}

} // namespace

QSizeF node_size_for(const node_data& data, bool compact,
                     const QFont& name_font, const QFont& detail_font,
                     const QFont& badge_font)
{
    VSS_PROFILE_ZONE("gui.topology.node.measure");
    return measure(data, compact, name_font, detail_font, badge_font).size;
}

NodeItem::NodeItem(node_data data, const QSizeF& size,
                   node_item_callbacks callbacks)
    : data_(std::move(data)), size_(size), callbacks_(std::move(callbacks))
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
    setToolTip(data_.tooltip);
    setZValue(1.0);
}

void NodeItem::redraw()
{
    ++revision_;
    update();
}

void NodeItem::set_state(bool selected, bool focused, bool hovered)
{
    if (selected_ == selected && focused_ == focused && hovered_ == hovered)
        return;
    selected_ = selected;
    focused_ = focused;
    hovered_ = hovered;
    redraw();
}

void NodeItem::set_in_loop(bool value)
{
    if (in_loop_ == value)
        return;
    in_loop_ = value;
    redraw();
}

void NodeItem::set_editing(bool value)
{
    if (editing_ == value)
        return;
    editing_ = value;
    redraw();
}

void NodeItem::set_wiring(bool value, bool source)
{
    if (wiring_ == value && wire_source_ == source)
        return;
    wiring_ = value;
    wire_source_ = source;
    redraw();
}

void NodeItem::set_drop_state(drop_state value)
{
    if (drop_ == value)
        return;
    drop_ = value;
    redraw();
}

bool NodeItem::handles_visible() const noexcept
{
    return editing_ && (wiring_ || hovered_ || selected_ || focused_);
}

QPointF NodeItem::left_handle() const
{
    return {0.0, size_.height() / 2.0};
}

QPointF NodeItem::right_handle() const
{
    return {size_.width(), size_.height() / 2.0};
}

QRectF NodeItem::boundingRect() const
{
    return QRectF(-6.0, -6.0, size_.width() + 12.0, size_.height() + 12.0);
}

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                     QWidget* widget)
{
    VSS_PROFILE_ZONE("gui.paint.node");
    Q_UNUSED(option);
    Q_UNUSED(widget);

    const Theme& theme = Theme::instance();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // A node is a rounded card. Taller nodes with an icon are drawn "tiled":
    // an icon tile at the left and the text after it.
    const QRectF body(0.0, 0.0, size_.width(), size_.height());
    const bool tiled = !data_.icon.isEmpty() && size_.height() >= 52.0;
    const qreal radius =
        theme.radius(tiled ? QStringLiteral("card") : QStringLiteral("inner"));

    // A raised card: a soft shadow under it on the sunken canvas.
    if (!data_.device)
    {
        QColor shadow = theme.colour(colour_role::ink);
        shadow.setAlphaF(0.08F);
        painter->setPen(Qt::NoPen);
        painter->setBrush(shadow);
        painter->drawRoundedRect(body.translated(0.0, 1.5), radius, radius);
    }

    // A halo just outside the card marks a selected node, or one in a loop,
    // which gets the error tint instead of the accent.
    if (selected_ || in_loop_)
    {
        const QColor halo = in_loop_ ? theme.colour(colour_role::badSoft)
                                     : theme.colour(colour_role::accentSoft);
        painter->setPen(QPen(halo, 4.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(body.adjusted(-2.5, -2.5, 2.5, 2.5),
                                 radius + 2.5, radius + 2.5);
    }

    // The edge colour says the most urgent thing first: a loop or a refused
    // drop, then selection or an accepted drop, then hover.
    QColor edge = theme.colour(colour_role::line2);
    if (in_loop_ || drop_ == drop_state::refuse)
        edge = theme.colour(colour_role::bad);
    else if (selected_ || drop_ == drop_state::accept)
        edge = theme.colour(colour_role::accent);
    else if (hovered_)
        edge = theme.colour(colour_role::control);

    // A thicker edge for any state that matters. An unlinked node gets a
    // dashed edge, unless one of those states already stands out.
    const bool strong = in_loop_ || drop_ != drop_state::none || selected_;
    QPen body_pen(edge, strong ? 2.0 : 1.0);
    if (data_.unlinked && !selected_ && !strong)
        body_pen.setStyle(Qt::DashLine);
    painter->setPen(body_pen);
    // Device nodes are filled sunken, the rest as a panel. While a drag
    // hovers, the fill shows whether dropping here would be accepted.
    QColor fill =
        theme.colour(data_.device ? colour_role::sunken : colour_role::panel);
    if (drop_ == drop_state::accept)
        fill = theme.colour(colour_role::accentSoft);
    else if (drop_ == drop_state::refuse)
        fill = theme.colour(colour_role::badSoft);
    painter->setBrush(fill);
    painter->drawRoundedRect(body, radius, radius);

    if (handles_visible())
    {
        // The wiring handles: a ring on each side edge, filled on the node a
        // wire starts from.
        constexpr qreal kHandleRadius = 5.0;
        for (const QPointF& centre : {left_handle(), right_handle()})
        {
            painter->setPen(
                QPen(theme.colour(wire_source_ ? colour_role::accent
                                               : colour_role::control),
                     1.5));
            painter->setBrush(theme.colour(wire_source_ ? colour_role::accent
                                                        : colour_role::panel));
            painter->drawEllipse(centre, kHandleRadius, kHandleRadius);
        }
    }

    // The keyboard focus ring is drawn around the card.
    if (focused_)
    {
        painter->setPen(QPen(theme.colour(colour_role::focus), 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(body.adjusted(-4.0, -4.0, 4.0, 4.0),
                                 radius + 4.0, radius + 4.0);
    }

    // Text layout. When there is an icon, its tile is drawn at the left and
    // the text starts after it.
    const qreal padding_x = theme.space(2);
    qreal left_x = padding_x;
    const qreal padding_y = theme.space(1);
    if (tiled)
    {
        const QRectF tile(theme.space(2), (body.height() - kTileSize) / 2.0,
                          kTileSize, kTileSize);
        colour_role soft = colour_role::accentSoft;
        colour_role ink = colour_role::accentText;
        if (data_.tile == node_data::tint::violet)
        {
            soft = colour_role::violetSoft;
            ink = colour_role::violet;
        }
        else if (data_.tile == node_data::tint::ok)
        {
            soft = colour_role::okSoft;
            ink = colour_role::ok;
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme.colour(soft));
        const qreal tile_radius = theme.radius(QStringLiteral("tile"));
        painter->drawRoundedRect(tile, tile_radius, tile_radius);
        const QIcon icon = IconProvider::instance().icon(data_.icon, ink, 18);
        icon.paint(painter, tile.adjusted(7.0, 7.0, -7.0, -7.0).toRect());
        left_x += kTileSpace;
    }
    // Fonts and metrics for the three kinds of text: name, detail line and
    // badges. The detail line is monospaced when data_.detail_mono is set.
    const QFont name_font = theme.font(text_style::label);
    const QFont detail_font = data_.detail_mono
                                  ? theme.font(text_style::data)
                                  : theme.font(text_style::caption);
    const QFont badge_font = theme.font(text_style::caption);

    const QFontMetricsF name_metrics(name_font);
    const QFontMetricsF detail_metrics(detail_font);
    const QFontMetricsF badge_metrics(badge_font);

    const qreal name_height = name_metrics.height();
    const qreal detail_height = detail_metrics.height();
    // The name and detail lines are centred as a block, but never closer to
    // the top than the padding.
    const qreal content_height = name_height + 2.0 + detail_height;
    const qreal content_top =
        std::max(padding_y, (body.height() - content_height) / 2.0);

    // The name is elided so that it never runs under the badges that follow.
    const qreal badges_width = badge_area(data_, badge_font);
    const qreal name_available =
        std::max(0.0, body.width() - left_x - padding_x - badges_width);

    painter->setFont(name_font);
    painter->setPen(theme.colour(colour_role::ink));
    const QString elided_name =
        name_metrics.elidedText(data_.name, Qt::ElideRight, name_available);
    painter->drawText(QRectF(left_x, content_top, name_available, name_height),
                      Qt::AlignLeft | Qt::AlignVCenter, elided_name);

    qreal x = left_x + name_metrics.horizontalAdvance(elided_name) + 6.0;
    // Badges follow the name as small chips and are dropped from the first one
    // that does not fit, rather than being squeezed.
    const QStringList badges = badge_texts(data_);
    const qreal chip_height = std::max(18.0, badge_metrics.height() + 4.0);
    for (const QString& badge : badges)
    {
        const qreal chip_width = badge_metrics.horizontalAdvance(badge) + 12.0;
        if (x + chip_width > body.width() - padding_x)
            break;
        const bool owner = badge == topology_text::owner_badge();
        draw_chip(
            painter,
            QRectF(x, content_top + (name_height - chip_height) / 2.0,
                   chip_width, chip_height),
            badge,
            theme.colour(owner ? colour_role::okSoft : colour_role::amberSoft),
            theme.colour(owner ? colour_role::okLine : colour_role::amberLine),
            theme.colour(owner ? colour_role::ok : colour_role::amber),
            badge_font);
        x += chip_width + 4.0;
    }
    // With no badges, an unlinked node says so in plain text if there is room.
    if (badges.isEmpty() && data_.unlinked)
    {
        const QString note = topology_text::unlinked_badge();
        const qreal note_width = badge_metrics.horizontalAdvance(note);
        if (x + note_width <= body.width() - padding_x)
        {
            painter->setFont(badge_font);
            painter->setPen(theme.colour(colour_role::ink3));
            painter->drawText(QRectF(x, content_top, note_width, name_height),
                              Qt::AlignLeft | Qt::AlignVCenter, note);
        }
    }

    // The detail line goes under the name, elided to the width of the card.
    painter->setFont(detail_font);
    painter->setPen(theme.colour(colour_role::ink3));
    const qreal detail_width = body.width() - left_x - padding_x;
    const QString elided_detail =
        detail_metrics.elidedText(data_.detail, Qt::ElideRight, detail_width);
    painter->drawText(QRectF(left_x, content_top + name_height + 2.0,
                             detail_width, detail_height),
                      Qt::AlignLeft | Qt::AlignVCenter, elided_detail);
}

void NodeItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        pressed_ = true;
        dragging_ = false;
        press_scene_ = event->scenePos();
        press_position_ = pos();
        if (callbacks_.pressed)
            callbacks_.pressed(data_.id);
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
}

void NodeItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    if (!pressed_ || !(event->buttons() & Qt::LeftButton))
    {
        QGraphicsItem::mouseMoveEvent(event);
        return;
    }

    const QPointF delta = event->scenePos() - press_scene_;
    if (!dragging_)
    {
        if (QPointF::dotProduct(delta, delta) <
            static_cast<qreal>(QApplication::startDragDistance()) *
                QApplication::startDragDistance())
        {
            event->accept();
            return;
        }
        dragging_ = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (callbacks_.dragged)
        callbacks_.dragged(data_.id, press_position_ + delta);
    event->accept();
}

void NodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && pressed_)
    {
        const bool was_dragging = dragging_;
        pressed_ = false;
        dragging_ = false;
        if (was_dragging)
        {
            setCursor(Qt::OpenHandCursor);
            if (callbacks_.drag_finished)
                callbacks_.drag_finished(data_.id);
        }
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void NodeItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (callbacks_.double_clicked)
            callbacks_.double_clicked(data_.id);
        event->accept();
        return;
    }
    QGraphicsItem::mouseDoubleClickEvent(event);
}

void NodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    redraw();
    if (callbacks_.hover_entered)
        callbacks_.hover_entered(data_.id);
    QGraphicsItem::hoverEnterEvent(event);
}

void NodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    redraw();
    if (callbacks_.hover_left)
        callbacks_.hover_left(data_.id);
    QGraphicsItem::hoverLeaveEvent(event);
}

} // namespace vss::gui::topology
