//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_TOPOLOGY_NODE_ITEM_HPP
#define VSS_GUI_TOPOLOGY_NODE_ITEM_HPP

#include "rendering/content_revision.hpp"

#include <vss/core/types.hpp>

#include <QFont>
#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <functional>

namespace vss::gui::topology
{

/// What one node draws: a name, one detail line and optional badges.
struct node_data
{
    vss::endpoint_id id = 0;
    QString name;
    /// The mono path, or a short role sentence such as "Real device, COM3".
    QString detail;
    bool detail_mono = true;
    /// A real device uses the sunken fill.
    bool device = false;
    /// A real device or the target of a join uses the taller 64 px box.
    bool large = false;
    bool unlinked = false;
    bool owner = false;
    bool dropped = false;
    QString tooltip;
    /// The kind tile at the card's left (design language rule 8): the icon,
    /// and its tint.  No tile in the compact size.
    enum class tint
    {
        accent,
        violet,
        ok
    };
    QString icon;
    tint tile = tint::accent;
};

/// The node size the design specifies, grown when the text needs more room.
///
/// 168 by 56 is the comfortable size and 120 by 48 the compact one; a real
/// device or the target of a join is 168 by 64.  At a larger text size the box
/// grows instead of clipping, because the specification forbids clipping at
/// 200 percent text.
QSizeF node_size_for(const node_data& data, bool compact,
                     const QFont& name_font, const QFont& detail_font,
                     const QFont& badge_font);

struct node_item_callbacks
{
    std::function<void(vss::endpoint_id)> pressed;
    std::function<void(vss::endpoint_id)> double_clicked;
    std::function<void(vss::endpoint_id)> hover_entered;
    std::function<void(vss::endpoint_id)> hover_left;
    /// The user is dragging the node; `position` is where its top-left corner
    /// would go.  The view moves the node and the links that follow it.
    std::function<void(vss::endpoint_id, const QPointF& position)> dragged;
    /// The mouse button was released after a drag.
    std::function<void(vss::endpoint_id)> drag_finished;
};

/// One port drawn as a card on the topology scene.
class NodeItem final : public QGraphicsItem, public rendering::content_revision
{
public:
    NodeItem(node_data data, const QSizeF& size, node_item_callbacks callbacks);

    vss::endpoint_id id() const noexcept { return data_.id; }
    const node_data& data() const noexcept { return data_; }

    void set_state(bool selected, bool focused, bool hovered);
    bool is_selected() const noexcept { return selected_; }
    bool is_focused() const noexcept { return focused_; }
    bool is_hovered() const noexcept { return hovered_; }

    /// Part of a loop the user asked to see.
    void set_in_loop(bool value);
    bool in_loop() const noexcept { return in_loop_; }

    /// What a wire being dragged over the node would do.
    enum class drop_state
    {
        none,
        accept,
        refuse
    };
    /// Advanced canvas: the node can show its wiring handles.  They show
    /// while it is hovered, selected or focused, or on every node while a
    /// wire is out (`set_wiring`).
    void set_editing(bool value);
    void set_wiring(bool value, bool source);
    void set_drop_state(drop_state value);
    drop_state drop() const noexcept { return drop_; }
    bool handles_visible() const noexcept;
    /// The centres of the left and right handles, in item coordinates.
    QPointF left_handle() const;
    QPointF right_handle() const;

    /// Counts up whenever what `paint` draws changes; a move does not count.
    [[nodiscard]] quint64 revision() const noexcept override
    {
        return revision_;
    }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    /// For every change in what `paint` draws.
    void redraw();

    node_data data_;
    quint64 revision_ = 1;
    QSizeF size_;
    node_item_callbacks callbacks_;
    bool selected_ = false;
    bool focused_ = false;
    bool hovered_ = false;
    bool in_loop_ = false;
    bool editing_ = false;
    bool wiring_ = false;
    bool wire_source_ = false;
    drop_state drop_ = drop_state::none;

    /// Set while the left button is down on the node; the drag starts only
    /// after the pointer has moved past the platform drag distance, so a click
    /// with a slightly unsteady hand still selects.
    bool pressed_ = false;
    bool dragging_ = false;
    QPointF press_scene_;
    QPointF press_position_;
};

} // namespace vss::gui::topology

#endif // VSS_GUI_TOPOLOGY_NODE_ITEM_HPP
