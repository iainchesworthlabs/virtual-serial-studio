//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/link_item.hpp"

#include "theme/theme.hpp"

#include <vss/core/profile.hpp>

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPen>
#include <QPolygonF>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vss::gui::topology
{
namespace
{

constexpr qreal kLabelGap = 8.0;
constexpr qreal kHitTolerance = 8.0;
constexpr qreal kFocusPadding = 4.0;
constexpr qreal kArrowPadding = 8.0;

QRectF label_rect(const QString& text, const QPointF& anchor, bool above,
                  const QFontMetricsF& metrics)
{
    if (text.isEmpty())
        return {};
    const qreal width = metrics.horizontalAdvance(text);
    const qreal height = metrics.height();
    const QRectF rect(anchor.x() - width / 2.0,
                      above ? anchor.y() - kLabelGap - height
                            : anchor.y() + kLabelGap,
                      width, height);
    return rect.adjusted(-2.0, -1.0, 2.0, 1.0);
}

} // namespace

LinkItem::LinkItem(link_data data, link_item_callbacks callbacks)
    : data_(std::move(data)), callbacks_(std::move(callbacks))
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(data_.accessible_name);
    setZValue(-1.0);
}

void LinkItem::compute_label_rects()
{
    VSS_PROFILE_ZONE("gui.topology.link.label_rects");
    const QFontMetricsF metrics(Theme::instance().font(text_style::caption));
    primary_rect_ =
        label_rect(data_.primary, geometry_.primary_anchor, true, metrics);
    secondary_rect_ =
        label_rect(data_.secondary, geometry_.secondary_anchor, false, metrics);
}

void LinkItem::set_geometry(const topo_link_placement& geometry)
{
    VSS_PROFILE_ZONE("gui.topology.link.set_geometry");
    prepareGeometryChange();
    geometry_ = geometry;
    compute_label_rects();
    update();
}

void LinkItem::set_state(bool selected, bool focused, bool hovered)
{
    if (selected_ == selected && focused_ == focused && hovered_ == hovered)
        return;
    selected_ = selected;
    focused_ = focused;
    hovered_ = hovered;
    update();
}

void LinkItem::set_in_loop(bool value)
{
    if (in_loop_ == value)
        return;
    in_loop_ = value;
    update();
}

QRectF LinkItem::boundingRect() const
{
    // A point or a horizontal line has no area, and QRectF::united() ignores
    // a rectangle without one, so the extent is kept as plain extremes.
    bool any = false;
    qreal left = 0.0;
    qreal top = 0.0;
    qreal right = 0.0;
    qreal bottom = 0.0;
    const auto note_point = [&](const QPointF& point)
    {
        if (!any)
        {
            left = right = point.x();
            top = bottom = point.y();
            any = true;
            return;
        }
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    };
    const auto note = [&note_point](const QRectF& rect)
    {
        if (!rect.isValid())
            return;
        note_point(rect.topLeft());
        note_point(rect.bottomRight());
    };

    if (!geometry_.stroke.isEmpty())
    {
        const QRectF extent = geometry_.stroke.controlPointRect();
        note_point(extent.topLeft());
        note_point(extent.bottomRight());
    }
    for (const QPolygonF& arrow : geometry_.arrows)
        note(arrow.boundingRect());
    note(primary_rect_);
    note(secondary_rect_);

    if (!any)
        return QRectF();
    return QRectF(QPointF(left, top), QPointF(right, bottom))
        .adjusted(-kFocusPadding - kArrowPadding, -kFocusPadding - kArrowPadding,
                  kFocusPadding + kArrowPadding, kFocusPadding + kArrowPadding);
}

QPainterPath LinkItem::shape() const
{
    QPainterPathStroker stroker;
    stroker.setWidth(kHitTolerance);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    QPainterPath path = stroker.createStroke(geometry_.stroke);
    if (primary_rect_.isValid())
        path.addRect(primary_rect_);
    if (secondary_rect_.isValid())
        path.addRect(secondary_rect_);
    return path;
}

void LinkItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                     QWidget* widget)
{
    VSS_PROFILE_ZONE("gui.paint.link");
    Q_UNUSED(option);
    Q_UNUSED(widget);

    const Theme& theme = Theme::instance();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QColor colour = theme.colour(colour_role::ink3);
    if (in_loop_)
        colour = theme.colour(colour_role::bad);
    else if (selected_)
        colour = theme.colour(colour_role::accent);
    else if (hovered_)
        colour = theme.colour(colour_role::ink2);

    QPen pen(colour, (selected_ || in_loop_) ? 3.0 : 2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    if (data_.dashed)
        pen.setStyle(Qt::DashLine);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(geometry_.stroke);

    if (!geometry_.arrows.isEmpty())
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(colour);
        for (const QPolygonF& arrow : geometry_.arrows)
            painter->drawPolygon(arrow);
    }

    const QFont label_font = theme.font(text_style::caption);
    painter->setFont(label_font);
    painter->setPen(theme.colour(colour_role::ink3));
    if (primary_rect_.isValid())
    {
        painter->drawText(primary_rect_, Qt::AlignCenter, data_.primary);
    }
    if (secondary_rect_.isValid())
    {
        painter->drawText(secondary_rect_, Qt::AlignCenter, data_.secondary);
    }

    if (focused_)
    {
        QRectF ring = primary_rect_.isValid() ? primary_rect_ : secondary_rect_;
        if (ring.isValid() && secondary_rect_.isValid())
            ring = ring.united(secondary_rect_);
        if (!ring.isValid())
        {
            const QPointF anchor = geometry_.primary_anchor;
            ring = QRectF(anchor - QPointF(12.0, 12.0), QSizeF(24.0, 24.0));
        }
        painter->setPen(QPen(theme.colour(colour_role::focus), 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(ring.adjusted(-3.0, -2.0, 3.0, 2.0), 3.0, 3.0);
    }
}

void LinkItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (callbacks_.pressed)
            callbacks_.pressed(data_.id);
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
}

void LinkItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    update();
    if (callbacks_.hover_entered)
        callbacks_.hover_entered(data_.id);
    QGraphicsItem::hoverEnterEvent(event);
}

void LinkItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    update();
    if (callbacks_.hover_left)
        callbacks_.hover_left(data_.id);
    QGraphicsItem::hoverLeaveEvent(event);
}

} // namespace vss::gui::topology
