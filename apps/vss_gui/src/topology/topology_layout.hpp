//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_TOPOLOGY_LAYOUT_HPP
#define VSS_GUI_TOPOLOGY_LAYOUT_HPP

#include "backend/view_types.hpp"

#include <vss/core/types.hpp>

#include <QHash>
#include <QLineF>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QVector>

namespace vss::gui::topology
{

/// One node fed to the layout: the port id and the size the text needs.
struct topo_node_input
{
    vss::endpoint_id id = 0;
    QSizeF size{168.0, 56.0};
};

/// One link fed to the layout.
struct topo_link_input
{
    vss::link_id id = 0;
    link_shape shape = link_shape::two_way;
    QVector<vss::endpoint_id> sources;
    QVector<vss::endpoint_id> targets;
    /// Draw a filled arrow head at each target end (one-way and monitor).
    bool arrow = false;
};

/// Gaps and margins in scene units.  The design fixes the minimum gap at
/// 48 px; the horizontal gap is wider so the link labels fit between the
/// columns (8 px clear of a line or a node).
struct topology_metrics
{
    qreal horizontal_gap = 96.0;
    qreal vertical_gap = 48.0;
    qreal margin = 16.0;
    /// How far a link keeps from any node it is not attached to: enough for
    /// a 12 px label and the 8 px the design keeps clear above or below it.
    qreal clearance = 32.0;
    /// The least space left between two nodes when the user drops one.
    qreal drop_gap = 16.0;
    /// How links are drawn: smooth curves or right angles.  It changes the
    /// strokes only; the route a link follows is the same.
    link_style style = link_style::curved;
};

struct topo_node_placement
{
    vss::endpoint_id id = 0;
    QRectF rect;
};

/// The pre-computed geometry of one link.  The item adds the label rectangles
/// from these anchors, because their size depends on the font.
///
/// `segments` and `bar` are the route: the straight lines the link follows,
/// with the node corners it goes around.  They are not drawn.  `stroke` is what
/// is drawn: smooth curves along the route, level where they meet a port.
struct topo_link_placement
{
    vss::link_id id = 0;
    QVector<QLineF> segments;
    /// The vertical bar of a bus; a null line when there is none.
    QLineF bar;
    QPainterPath stroke;
    QVector<QPolygonF> arrows;
    QPointF primary_anchor;
    QPointF secondary_anchor;
};

struct topology_layout
{
    /// Nodes in reading order: left column first, then top to bottom.  This
    /// is also the Tab order of the view.
    QVector<topo_node_placement> nodes;
    QVector<topo_link_placement> links;
    QRectF content;

    QRectF node_rect(vss::endpoint_id id) const;
    const topo_node_placement* find_node(vss::endpoint_id id) const;
    const topo_link_placement* find_link(vss::link_id id) const;
};

/// Places the nodes in deterministic columns: sources on the left, targets on
/// the right, each column centred vertically, with at least
/// `metrics.vertical_gap` between two nodes and at least
/// `metrics.horizontal_gap` between two columns.  Split and join links get a
/// mid-gap bar; every other link is a straight line.  The result depends only
/// on the input order of the ports, so two runs on the same setup are equal.
topology_layout compute_topology_layout(const QVector<topo_node_input>& nodes,
                                        const QVector<topo_link_input>& links,
                                        const topology_metrics& metrics = {});

/// Geometry for every link from the current node rectangles.  Split and join
/// links get their mid-gap bar; every other shape is a straight line.  A line
/// (or a bar) that would run under a node it is not attached to is routed
/// around the node instead, keeping `metrics.clearance` from it; a split or a
/// join that cannot keep its bar clear falls back to one routed line per
/// pair.  The view calls this again after a drag or a nudge, when only the
/// node positions changed.
QVector<topo_link_placement>
compute_link_geometry(const QVector<topo_link_input>& links,
                      const QHash<vss::endpoint_id, QRectF>& rects,
                      const topology_metrics& metrics = {});

/// True when the segment passes through any obstacle grown by `clearance`.
bool segment_blocked(const QLineF& line, const QVector<QRectF>& obstacles,
                     qreal clearance);

/// The shortest polyline from `from` to `to` that keeps `clearance` away from
/// every obstacle.  The first and last points are `from` and `to`; a clear
/// direct line returns just those two.  When no route exists (an end point is
/// inside an obstacle) the direct line is returned.
QVector<QPointF> route_around(const QPointF& from, const QPointF& to,
                              const QVector<QRectF>& obstacles,
                              qreal clearance);

/// Which side of its anchor a new node goes on.
enum class anchor_side
{
    none,
    /// The new node is a target of the anchor, so it goes in the next column.
    right_of,
    /// The new node is a source of the anchor, so it goes in the previous one.
    left_of,
};

/// The top-left corner for a new node of `size` in free space: next to
/// `anchor` on the given side, at the nearest height where it overlaps no node
/// in `occupied` (with `metrics.vertical_gap` between them) and, when
/// possible, where the straight link to the anchor crosses no other node.
/// With `anchor_side::none` the node goes in a fresh column after the content.
QPointF find_free_position(const QSizeF& size, const QRectF& anchor,
                           anchor_side side, const QVector<QRectF>& occupied,
                           const topology_metrics& metrics = {});

/// Moves `rect` the shortest way that leaves at least `gap` between it and
/// every rectangle in `occupied`.  A rectangle that is already clear is
/// returned unchanged.
QRectF push_clear(const QRectF& rect, const QVector<QRectF>& occupied,
                  qreal gap);

/// Finds a directed cycle in the link graph.  The returned path starts and
/// ends at the same port, for example `a, b, c, a`; an acyclic graph returns
/// an empty vector.  Used by the "Show the loop" feedback.
QVector<vss::endpoint_id> find_cycle(const QVector<topo_link_input>& links,
                                     const QVector<vss::endpoint_id>& node_ids);

/// The links that connect two ports of the cycle, so they can be highlighted.
QVector<vss::link_id> cycle_links(const QVector<topo_link_input>& links,
                                  const QVector<vss::endpoint_id>& cycle);

} // namespace vss::gui::topology

#endif // VSS_GUI_TOPOLOGY_LAYOUT_HPP
