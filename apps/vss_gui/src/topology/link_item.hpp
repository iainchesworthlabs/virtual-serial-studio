//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_TOPOLOGY_LINK_ITEM_HPP
#define VSS_GUI_TOPOLOGY_LINK_ITEM_HPP

#include "topology/topology_layout.hpp"

#include "rendering/content_revision.hpp"

#include <vss/core/types.hpp>

#include <QGraphicsItem>
#include <QPainterPath>
#include <QRectF>
#include <QString>

#include <functional>

namespace vss::gui::topology
{

/// What one link draws.
struct link_data
{
    vss::link_id id = 0;
    link_shape shape = link_shape::two_way;
    QString primary;
    QString secondary;
    QString accessible_name;
    bool dashed = false;
    bool arrow = false;
};

struct link_item_callbacks
{
    std::function<void(vss::link_id)> pressed;
    std::function<void(vss::link_id)> hover_entered;
    std::function<void(vss::link_id)> hover_left;
};

/// One link drawn as a curve, a fan of curves from a junction in the middle of
/// the gap, a bus on a rounded bar, or a dashed tap.
///
/// The item has an 8 px hit tolerance, as the design asks, and the label text
/// is part of the shape so clicking a label selects the link.
class LinkItem final : public QGraphicsItem, public rendering::content_revision
{
public:
    LinkItem(link_data data, link_item_callbacks callbacks);

    vss::link_id id() const noexcept { return data_.id; }
    const link_data& data() const noexcept { return data_; }

    void set_geometry(const topo_link_placement& geometry);
    const topo_link_placement& geometry() const noexcept { return geometry_; }

    void set_state(bool selected, bool focused, bool hovered);
    bool is_selected() const noexcept { return selected_; }
    bool is_focused() const noexcept { return focused_; }
    bool is_hovered() const noexcept { return hovered_; }

    void set_in_loop(bool value);
    bool in_loop() const noexcept { return in_loop_; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void compute_label_rects();
    /// For every change in what `paint` draws.
    void redraw();

    link_data data_;
    link_item_callbacks callbacks_;
    topo_link_placement geometry_;
    QRectF primary_rect_;
    QRectF secondary_rect_;
    bool selected_ = false;
    bool focused_ = false;
    bool hovered_ = false;
    bool in_loop_ = false;
};

} // namespace vss::gui::topology

#endif // VSS_GUI_TOPOLOGY_LINK_ITEM_HPP
