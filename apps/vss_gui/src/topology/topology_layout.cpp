//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/topology_layout.hpp"

#include "topology/link_curve.hpp"

#include <vss/core/profile.hpp>

#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace vss::gui::topology
{
namespace
{

constexpr qreal kBusStub = 24.0;
constexpr qreal kBusCorner = 12.0;
constexpr qreal kArrowLength = 8.0;
constexpr qreal kArrowHalfWidth = 4.5;

QPointF right_centre(const QRectF& rect)
{
    return QPointF(rect.right(), rect.center().y());
}

QPointF left_centre(const QRectF& rect)
{
    return QPointF(rect.left(), rect.center().y());
}

QPolygonF arrow_head(const QPointF& tip, const QPointF& from)
{
    const qreal length = QLineF(from, tip).length();
    if (length < 1e-6)
        return {};

    const QPointF unit((tip.x() - from.x()) / length,
                       (tip.y() - from.y()) / length);
    const QPointF base = tip - unit * kArrowLength;
    const QPointF perpendicular(-unit.y(), unit.x());

    QPolygonF arrow;
    arrow << tip << base + perpendicular * kArrowHalfWidth
          << base - perpendicular * kArrowHalfWidth;
    return arrow;
}

void note_point(QRectF& bounds, const QPointF& point)
{
    // QRectF::united() ignores an empty rectangle, so a point has to widen the
    // bounds by hand or the bar of a bus left of the first column is lost.
    if (bounds.isNull())
    {
        bounds = QRectF(point, QSizeF(0.001, 0.001));
        return;
    }
    bounds.setLeft(std::min(bounds.left(), point.x()));
    bounds.setTop(std::min(bounds.top(), point.y()));
    bounds.setRight(std::max(bounds.right(), point.x()));
    bounds.setBottom(std::max(bounds.bottom(), point.y()));
}

void note_rect(QRectF& bounds, const QRectF& rect)
{
    bounds = bounds.isNull() ? rect : bounds.united(rect);
}

} // namespace

QRectF topology_layout::node_rect(vss::endpoint_id id) const
{
    const topo_node_placement* placement = find_node(id);
    return placement == nullptr ? QRectF() : placement->rect;
}

const topo_node_placement* topology_layout::find_node(vss::endpoint_id id) const
{
    for (const auto& node : nodes)
    {
        if (node.id == id)
            return &node;
    }
    return nullptr;
}

const topo_link_placement* topology_layout::find_link(vss::link_id id) const
{
    for (const auto& link : links)
    {
        if (link.id == id)
            return &link;
    }
    return nullptr;
}

topology_layout compute_topology_layout(const QVector<topo_node_input>& nodes,
                                        const QVector<topo_link_input>& links,
                                        const topology_metrics& metrics)
{
    VSS_PROFILE_ZONE("gui.topology.layout.compute");
    VSS_PROFILE_PLOT("gui.topology.nodes", nodes.size());
    VSS_PROFILE_PLOT("gui.topology.links", links.size());
    topology_layout result;
    if (nodes.isEmpty())
        return result;

    QVector<topo_node_input> ordered_nodes = nodes;
    std::sort(ordered_nodes.begin(), ordered_nodes.end(),
              [](const topo_node_input& left, const topo_node_input& right)
              { return left.id < right.id; });

    QVector<topo_link_input> ordered_links = links;
    std::sort(ordered_links.begin(), ordered_links.end(),
              [](const topo_link_input& left, const topo_link_input& right)
              { return left.id < right.id; });

    QHash<vss::endpoint_id, QSizeF> sizes;
    QHash<vss::endpoint_id, int> rank;
    QHash<vss::endpoint_id, QVector<vss::endpoint_id>> neighbours;
    for (const auto& node : ordered_nodes)
    {
        sizes.insert(node.id, node.size);
        rank.insert(node.id, 0);
    }

    for (const auto& link : ordered_links)
    {
        for (const vss::endpoint_id source : link.sources)
        {
            if (!rank.contains(source))
                continue;
            for (const vss::endpoint_id target : link.targets)
            {
                if (!rank.contains(target))
                    continue;
                neighbours[source].push_back(target);
                neighbours[target].push_back(source);
            }
        }
    }
    for (auto it = neighbours.begin(); it != neighbours.end(); ++it)
    {
        std::sort(it->begin(), it->end());
        it->erase(std::unique(it->begin(), it->end()), it->end());
    }

    // Longest-path ranks.  The engine refuses cycles, so a handful of passes
    // is enough; the clamp keeps a malformed document deterministic instead
    // of unbounded.
    const int node_count = static_cast<int>(ordered_nodes.size());
    for (int pass = 0; pass < node_count; ++pass)
    {
        bool changed = false;
        for (const auto& link : ordered_links)
        {
            for (const vss::endpoint_id source : link.sources)
            {
                if (!rank.contains(source))
                    continue;
                for (const vss::endpoint_id target : link.targets)
                {
                    if (!rank.contains(target))
                        continue;
                    const int candidate =
                        std::min(node_count - 1, rank.value(source) + 1);
                    if (candidate > rank.value(target))
                    {
                        rank[target] = candidate;
                        changed = true;
                    }
                }
            }
        }
        if (!changed)
            break;
    }

    // A port with no link is not a source or a target: give it its own column
    // after the graph so it cannot push a real source off the centre line.
    if (!ordered_links.isEmpty())
    {
        QSet<vss::endpoint_id> linked;
        for (const auto& link : ordered_links)
        {
            for (const vss::endpoint_id id : link.sources)
                linked.insert(id);
            for (const vss::endpoint_id id : link.targets)
                linked.insert(id);
        }
        int highest = 0;
        for (auto it = rank.constBegin(); it != rank.constEnd(); ++it)
            highest = std::max(highest, it.value());
        for (const auto& node : ordered_nodes)
        {
            if (!linked.contains(node.id))
                rank[node.id] = highest + 1;
        }
    }

    QMap<int, QVector<vss::endpoint_id>> by_rank;
    for (const auto& node : ordered_nodes)
        by_rank[rank.value(node.id)].push_back(node.id);

    QVector<QVector<vss::endpoint_id>> columns;
    columns.reserve(by_rank.size());
    for (auto it = by_rank.begin(); it != by_rank.end(); ++it)
        columns.push_back(*it);

    // One barycentre pass in each direction: peers in the neighbouring column
    // decide the vertical order.  Ties fall back to the port id, so the result
    // is stable for a given setup.
    using column_set = QVector<QVector<vss::endpoint_id>>;
    const auto order_column =
        [&neighbours](column_set& set, int column, int reference)
    {
        QHash<vss::endpoint_id, double> position;
        const auto& reference_ids = set.at(reference);
        for (int index = 0; index < reference_ids.size(); ++index)
            position.insert(reference_ids.at(index),
                            static_cast<double>(index));

        auto& ids = set[column];
        QHash<vss::endpoint_id, double> fallback;
        for (int index = 0; index < ids.size(); ++index)
            fallback.insert(ids.at(index), static_cast<double>(index));

        QHash<vss::endpoint_id, double> key;
        for (const vss::endpoint_id id : ids)
        {
            double sum = 0.0;
            int count = 0;
            for (const vss::endpoint_id peer : neighbours.value(id))
            {
                const auto found = position.constFind(peer);
                if (found == position.constEnd())
                    continue;
                sum += found.value();
                ++count;
            }
            key.insert(id, count == 0 ? fallback.value(id) : sum / count);
        }

        std::stable_sort(ids.begin(), ids.end(),
                         [&key](vss::endpoint_id left, vss::endpoint_id right)
                         {
                             if (key.value(left) < key.value(right))
                                 return true;
                             if (key.value(right) < key.value(left))
                                 return false;
                             return left < right;
                         });
    };

    const auto sweep = [&order_column](column_set& set)
    {
        for (int column = 1; column < set.size(); ++column)
            order_column(set, column, column - 1);
        for (int column = static_cast<int>(set.size()) - 2; column >= 0;
             --column)
            order_column(set, column, column + 1);
    };
    sweep(columns);

    QVector<qreal> column_width(columns.size(), 0.0);
    QVector<qreal> column_height(columns.size(), 0.0);
    for (int column = 0; column < columns.size(); ++column)
    {
        qreal width = 0.0;
        qreal height =
            metrics.vertical_gap * static_cast<qreal>(std::max<qsizetype>(
                                       0, columns.at(column).size() - 1));
        for (const vss::endpoint_id id : columns.at(column))
        {
            const QSizeF size = sizes.value(id);
            width = std::max(width, size.width());
            height += size.height();
        }
        column_width[column] = width;
        column_height[column] = height;
    }

    const qreal total_height =
        *std::max_element(column_height.begin(), column_height.end());

    const auto place = [&](const column_set& set)
    {
        QHash<vss::endpoint_id, QRectF> placed;
        qreal x = metrics.margin;
        for (int column = 0; column < set.size(); ++column)
        {
            qreal y = metrics.margin +
                      (total_height - column_height.at(column)) / 2.0;
            for (const vss::endpoint_id id : set.at(column))
            {
                const QSizeF size = sizes.value(id);
                placed.insert(
                    id,
                    QRectF(x + (column_width.at(column) - size.width()) / 2.0,
                           y, size.width(), size.height()));
                y += size.height() + metrics.vertical_gap;
            }
            x += column_width.at(column) + metrics.horizontal_gap;
        }
        return placed;
    };

    // The barycentre sweeps leave some crossings behind.  Count the crossings
    // of the straight lines between the ports and keep any order that has
    // fewer: more sweeps, then swaps of two neighbours in a column.  Only a
    // strictly better order replaces the current one, so a layout that is
    // already free of crossings is left as the sweeps made it.
    QVector<std::pair<vss::endpoint_id, vss::endpoint_id>> edges;
    for (const auto& link : ordered_links)
    {
        for (const vss::endpoint_id source : link.sources)
        {
            for (const vss::endpoint_id target : link.targets)
            {
                const auto edge = std::make_pair(source, target);
                if (rank.contains(source) && rank.contains(target) &&
                    !edges.contains(edge))
                    edges.append(edge);
            }
        }
    }
    const auto crossings =
        [&edges](const QHash<vss::endpoint_id, QRectF>& placed)
    {
        QVector<QLineF> lines;
        lines.reserve(edges.size());
        for (const auto& edge : edges)
        {
            lines.append(QLineF(right_centre(placed.value(edge.first)),
                                left_centre(placed.value(edge.second))));
        }
        int count = 0;
        for (qsizetype left = 0; left < edges.size(); ++left)
        {
            for (qsizetype right = left + 1; right < edges.size(); ++right)
            {
                if (edges.at(left).first == edges.at(right).first ||
                    edges.at(left).second == edges.at(right).second)
                    continue;
                if (lines.at(left).intersects(lines.at(right), nullptr) ==
                    QLineF::BoundedIntersection)
                    ++count;
            }
        }
        return count;
    };
    constexpr qsizetype kMaxOptimisedEdges = 150;
    constexpr qsizetype kMaxOptimisedNodes = 60;
    if (columns.size() >= 2 && edges.size() >= 2 &&
        edges.size() <= kMaxOptimisedEdges &&
        ordered_nodes.size() <= kMaxOptimisedNodes)
    {
        VSS_PROFILE_ZONE("gui.topology.layout.reduce_crossings");
        column_set best = columns;
        int best_count = crossings(place(best));
        column_set trial = columns;
        for (int round = 0; round < 6 && best_count > 0; ++round)
        {
            sweep(trial);
            const int count = crossings(place(trial));
            if (count < best_count)
            {
                best = trial;
                best_count = count;
            }
        }
        bool improved = true;
        for (int pass = 0; pass < 8 && improved && best_count > 0; ++pass)
        {
            improved = false;
            for (auto& column : best)
            {
                for (qsizetype index = 0; index + 1 < column.size(); ++index)
                {
                    std::swap(column[index], column[index + 1]);
                    const int count = crossings(place(best));
                    if (count < best_count)
                    {
                        best_count = count;
                        improved = true;
                    }
                    else
                    {
                        std::swap(column[index], column[index + 1]);
                    }
                }
            }
        }
        columns = best;
    }

    const QHash<vss::endpoint_id, QRectF> rects = place(columns);
    for (const auto& column : columns)
    {
        for (const vss::endpoint_id id : column)
            result.nodes.append({id, rects.value(id)});
    }

    result.links = compute_link_geometry(ordered_links, rects, metrics);

    QRectF bounds;
    for (const auto& node : result.nodes)
        note_rect(bounds, node.rect);
    for (const auto& link : result.links)
    {
        for (const QPolygonF& curve : link.stroke.toSubpathPolygons())
        {
            for (const QPointF& point : curve)
                note_point(bounds, point);
        }
        for (const QPolygonF& arrow : link.arrows)
        {
            for (const QPointF& point : arrow)
                note_point(bounds, point);
        }
    }

    result.content = bounds.isNull()
                         ? QRectF()
                         : bounds.adjusted(-metrics.margin, -metrics.margin,
                                           metrics.margin, metrics.margin);
    return result;
}

namespace
{

/// A routed line leaves a node by the clearance plus this much before it
/// turns, so it never grazes the node it starts from.
constexpr qreal kExitMargin = 4.0;
/// Rectangles are shrunk by this much before a segment test, so a line that
/// runs exactly along the edge of a grown obstacle is not counted as inside.
constexpr qreal kEdgeTolerance = 0.5;

/// Liang-Barsky: does the segment touch the (closed) rectangle?
bool segment_touches(const QLineF& line, const QRectF& rect)
{
    if (!rect.isValid())
        return false;
    const qreal dx = line.dx();
    const qreal dy = line.dy();
    qreal t0 = 0.0;
    qreal t1 = 1.0;
    const auto clip = [&t0, &t1](qreal p, qreal q)
    {
        if (std::abs(p) < 1e-12)
            return q >= 0.0;
        const qreal ratio = q / p;
        if (p < 0.0)
        {
            if (ratio > t1)
                return false;
            t0 = std::max(t0, ratio);
        }
        else
        {
            if (ratio < t0)
                return false;
            t1 = std::min(t1, ratio);
        }
        return true;
    };
    return clip(-dx, line.x1() - rect.left()) &&
           clip(dx, rect.right() - line.x1()) &&
           clip(-dy, line.y1() - rect.top()) &&
           clip(dy, rect.bottom() - line.y1());
}

QRectF zone_of(const QRectF& rect, qreal clearance)
{
    return rect.adjusted(-clearance, -clearance, clearance, clearance)
        .adjusted(kEdgeTolerance, kEdgeTolerance, -kEdgeTolerance,
                  -kEdgeTolerance);
}

/// Drops the middle point of three that lie on one line.
QVector<QPointF> simplified(const QVector<QPointF>& path)
{
    QVector<QPointF> result;
    for (const QPointF& point : path)
    {
        if (!result.isEmpty() && QLineF(result.last(), point).length() < 1e-6)
            continue;
        while (result.size() >= 2)
        {
            const QPointF& a = result.at(result.size() - 2);
            const QPointF& b = result.at(result.size() - 1);
            const qreal cross = (b.x() - a.x()) * (point.y() - a.y()) -
                                (b.y() - a.y()) * (point.x() - a.x());
            const qreal dot = (b.x() - a.x()) * (point.x() - b.x()) +
                              (b.y() - a.y()) * (point.y() - b.y());
            if (std::abs(cross) < 1e-6 && dot > 0.0)
                result.removeLast();
            else
                break;
        }
        result.append(point);
    }
    return result;
}

/// The obstacles in a fixed order, so a tie between two routes is always
/// broken the same way whatever order a hash iterates in.
QVector<QRectF> sorted_rects(QVector<QRectF> rects)
{
    std::sort(rects.begin(), rects.end(),
              [](const QRectF& left, const QRectF& right)
              {
                  if (left.left() < right.left())
                      return true;
                  if (right.left() < left.left())
                      return false;
                  return left.top() < right.top();
              });
    return rects;
}

QVector<QRectF> rects_except(const QHash<vss::endpoint_id, QRectF>& rects,
                             const QVector<vss::endpoint_id>& excluded)
{
    QVector<QRectF> result;
    for (auto it = rects.constBegin(); it != rects.constEnd(); ++it)
    {
        if (!excluded.contains(it.key()))
            result.append(it.value());
    }
    return sorted_rects(result);
}

/// The polyline between a source and a target: the straight line when it is
/// clear, otherwise a route around every node, the two ends included.
QVector<QPointF> shortest_edge_path(const QRectF& source, const QRectF& target,
                                    const QVector<QRectF>& others,
                                    qreal clearance, link_style style)
{
    const QPointF start = right_centre(source);
    const QPointF finish = left_centre(target);
    // The link is drawn as a curve or as right angles, so what is drawn is
    // what has to be clear.
    const QPainterPath direct =
        style == link_style::curved
            ? s_curve(start, finish)
            : elbow_curve(start, finish, (start.x() + finish.x()) / 2.0);
    if (finish.x() > start.x() &&
        !curve_blocked(direct, others, std::max(0.0, clearance - kCurveSlack)))
    {
        return {start, finish};
    }

    QVector<QRectF> obstacles = others;
    obstacles.append(source);
    obstacles.append(target);
    obstacles = sorted_rects(obstacles);

    const qreal stub = clearance + kExitMargin;
    QVector<QPointF> path;
    path.append(start);
    const QVector<QPointF> middle =
        route_around(start + QPointF(stub, 0.0), finish - QPointF(stub, 0.0),
                     obstacles, clearance);
    for (const QPointF& point : middle)
        path.append(point);
    path.append(finish);
    return simplified(path);
}

/// The line a link would take if it ran straight between two of its ports.
/// Routes for a link are compared on how many of these they cross.
struct proxy_edge
{
    vss::endpoint_id source = 0;
    vss::endpoint_id target = 0;
    QLineF line;
};

/// What crossing one other link costs a route, in pixels of extra length: a
/// route may be this much longer to avoid one crossing.
constexpr qreal kCrossingCost = 120.0;

/// How many of the nodes on the straight line get a route of their own over
/// and under them; more would cost a route search each on every drag.
constexpr int kViaNodes = 3;

qreal polyline_length(const QVector<QPointF>& path)
{
    qreal length = 0.0;
    for (qsizetype index = 1; index < path.size(); ++index)
        length += QLineF(path.at(index - 1), path.at(index)).length();
    return length;
}

/// How many links that share no node with this one the route crosses.
int proxy_crossings(const QVector<QPointF>& path, vss::endpoint_id source,
                    vss::endpoint_id target, const QVector<proxy_edge>& proxies)
{
    int count = 0;
    for (const proxy_edge& proxy : proxies)
    {
        if (proxy.source == source || proxy.source == target ||
            proxy.target == source || proxy.target == target)
            continue;
        for (qsizetype index = 1; index < path.size(); ++index)
        {
            if (QLineF(path.at(index - 1), path.at(index))
                    .intersects(proxy.line, nullptr) ==
                QLineF::BoundedIntersection)
            {
                ++count;
                break;
            }
        }
    }
    return count;
}

/// True when every segment between the two end stubs keeps clear.
bool route_clear(const QVector<QPointF>& path, const QVector<QRectF>& obstacles,
                 qreal clearance)
{
    for (qsizetype index = 1; index + 2 < path.size(); ++index)
    {
        if (segment_blocked(QLineF(path.at(index), path.at(index + 1)),
                            obstacles, clearance))
            return false;
    }
    return true;
}

/// The route of one link.  The shortest route round the nodes in the way is
/// the start; when nodes are in the way it is weighed against a route over
/// the top of them and a route under them, and the one that crosses the fewest
/// other links wins unless it is a lot longer.
QVector<QPointF> edge_path(vss::endpoint_id source_id,
                           vss::endpoint_id target_id, const QRectF& source,
                           const QRectF& target, const QVector<QRectF>& others,
                           qreal clearance, const QVector<proxy_edge>& proxies,
                           link_style style)
{
    const QVector<QPointF> shortest =
        shortest_edge_path(source, target, others, clearance, style);
    if (shortest.size() <= 2)
        return shortest;

    const QPointF start = right_centre(source);
    const QPointF finish = left_centre(target);
    const qreal left = std::min(start.x(), finish.x());
    const qreal right = std::max(start.x(), finish.x());
    bool found = false;
    qreal top = 0.0;
    qreal bottom = 0.0;
    for (const QRectF& rect : others)
    {
        if (rect.right() <= left || rect.left() >= right)
            continue;
        top = found ? std::min(top, rect.top()) : rect.top();
        bottom = found ? std::max(bottom, rect.bottom()) : rect.bottom();
        found = true;
    }

    QVector<QVector<QPointF>> candidates{shortest};
    if (found)
    {
        QVector<QRectF> obstacles = others;
        obstacles.append(source);
        obstacles.append(target);
        obstacles = sorted_rects(obstacles);

        const qreal stub = clearance + kExitMargin;
        const qreal x = (left + right) / 2.0;
        // Over and under everything between the ports, then over and under
        // each node that sits on the straight line (the first few of them).
        QVector<QPointF> vias{QPointF(x, top - stub),
                              QPointF(x, bottom + stub)};
        int in_the_way = 0;
        for (const QRectF& rect : others)
        {
            if (in_the_way >= kViaNodes ||
                !segment_blocked(QLineF(start, finish), {rect}, clearance))
                continue;
            ++in_the_way;
            vias.append(QPointF(rect.center().x(), rect.top() - stub));
            vias.append(QPointF(rect.center().x(), rect.bottom() + stub));
        }
        for (const QPointF& via : vias)
        {
            QVector<QPointF> path{start};
            for (const QPointF& point : route_around(start + QPointF(stub, 0.0),
                                                     via, obstacles, clearance))
                path.append(point);
            for (const QPointF& point : route_around(
                     via, finish - QPointF(stub, 0.0), obstacles, clearance))
                path.append(point);
            path.append(finish);
            path = simplified(path);
            if (route_clear(path, obstacles, clearance))
                candidates.append(path);
        }
    }

    qsizetype best = 0;
    qreal best_cost = std::numeric_limits<qreal>::infinity();
    for (qsizetype index = 0; index < candidates.size(); ++index)
    {
        const QVector<QPointF>& path = candidates.at(index);
        const qreal cost = polyline_length(path) +
                           kCrossingCost * proxy_crossings(path, source_id,
                                                           target_id, proxies);
        if (cost < best_cost - 1e-6)
        {
            best = index;
            best_cost = cost;
        }
    }
    return candidates.at(best);
}

/// The arrow head at the end of a curve, pointing the way the curve arrives.
QPolygonF arrow_at_end(const QPainterPath& curve)
{
    const QPointF tip = curve.currentPosition();
    return arrow_head(tip, tip - end_direction(curve));
}

/// How far the second link between the same two ports bows away from the
/// first, and each further one beyond that.
constexpr qreal kLaneStep = 22.0;

/// `route` with every point but the two ports moved `distance` to one side of
/// the way the route runs there, the points next to a port by half as much.
QVector<QPointF> offset_route(const QVector<QPointF>& route, qreal distance)
{
    QVector<QPointF> moved = route;
    for (qsizetype index = 1; index + 1 < route.size(); ++index)
    {
        const QLineF way(route.at(index - 1), route.at(index + 1));
        if (way.length() < 1e-6)
            continue;
        const bool next_to_port = index == 1 || index + 2 == route.size();
        const qreal reach = next_to_port ? distance / 2.0 : distance;
        moved[index] =
            route.at(index) +
            QPointF(way.dy() / way.length(), -way.dx() / way.length()) * reach;
    }
    return moved;
}

/// How far the link in `lane` bows away from the first link between its ports:
/// none for the first, then to one side and the other, further each time.
qreal lane_bow(int lane)
{
    return static_cast<qreal>((lane + 1) / 2) * kLaneStep *
           (lane % 2 != 0 ? 1.0 : -1.0);
}

/// The curve of a link that runs straight between two ports.  The first link
/// between a pair takes the plain S-curve; the others bow to either side of it,
/// across the line between the ports, so two links on the same ports can be
/// told apart.
QPainterPath lane_curve(const QPointF& from, const QPointF& to, int lane,
                        const QVector<QRectF>& others, qreal clearance)
{
    const QLineF chord(from, to);
    if (lane > 0 && chord.length() > 1e-6)
    {
        const QPointF across(chord.dy() / chord.length(),
                             -chord.dx() / chord.length());
        // The bow it is due, then less of it, then the far side of the first
        // link, when a node is in the way.
        const qreal bow = lane_bow(lane);
        const QPointF run(port_run(chord.dx()), 0.0);
        for (const qreal scale : {1.0, 0.6, -1.6})
        {
            const QPainterPath bowed = smooth_route(
                {from, from + run, chord.center() + across * (bow * scale),
                 to - run, to});
            if (!curve_blocked(bowed, others, clearance))
                return bowed;
        }
    }
    return s_curve(from, to);
}

/// The right-angle link between two ports that run straight at each other:
/// the first link between a pair bends halfway, the others bend to either side
/// of that.
QPainterPath lane_elbow(const QPointF& from, const QPointF& to, int lane,
                        const QVector<QRectF>& others, qreal clearance)
{
    const qreal middle = (from.x() + to.x()) / 2.0;
    const qreal low = from.x() + kElbowRadius;
    const qreal high = to.x() - kElbowRadius;
    if (lane > 0 && low < high)
    {
        const qreal bow = lane_bow(lane);
        for (const qreal scale : {1.0, 0.6, -1.6})
        {
            const QPainterPath bent = elbow_curve(
                from, to, std::clamp(middle + bow * scale, low, high));
            if (!curve_blocked(bent, others, clearance))
                return bent;
        }
    }
    return elbow_curve(from, to, middle);
}

} // namespace

bool segment_blocked(const QLineF& line, const QVector<QRectF>& obstacles,
                     qreal clearance)
{
    for (const QRectF& obstacle : obstacles)
    {
        if (segment_touches(line, zone_of(obstacle, clearance)))
            return true;
    }
    return false;
}

QVector<QPointF> route_around(const QPointF& from, const QPointF& to,
                              const QVector<QRectF>& obstacles, qreal clearance)
{
    VSS_PROFILE_ZONE("gui.topology.link.route_around");
    QVector<QRectF> zones;
    zones.reserve(obstacles.size());
    for (const QRectF& obstacle : obstacles)
        zones.append(zone_of(obstacle, clearance));

    const auto visible = [&zones](const QPointF& a, const QPointF& b)
    {
        const QLineF line(a, b);
        for (const QRectF& zone : zones)
        {
            if (segment_touches(line, zone))
                return false;
        }
        return true;
    };
    if (visible(from, to))
        return {from, to};

    QVector<QPointF> points;
    points.append(from);
    points.append(to);
    for (qsizetype index = 0; index < zones.size(); ++index)
    {
        // The corners of the grown rectangle, not of the shrunk test zone.
        const QRectF grown = obstacles.at(index).adjusted(
            -clearance, -clearance, clearance, clearance);
        const QPointF corners[4] = {grown.topLeft(), grown.topRight(),
                                    grown.bottomRight(), grown.bottomLeft()};
        for (const QPointF& corner : corners)
        {
            bool inside = false;
            for (qsizetype other = 0; other < zones.size(); ++other)
            {
                if (zones.at(other).contains(corner))
                {
                    inside = true;
                    break;
                }
            }
            if (!inside)
                points.append(corner);
        }
    }

    const qsizetype count = points.size();
    constexpr qreal kInfinity = std::numeric_limits<qreal>::infinity();
    QVector<qreal> distance(count, kInfinity);
    QVector<qsizetype> previous(count, -1);
    QVector<bool> done(count, false);
    distance[0] = 0.0;

    for (qsizetype round = 0; round < count; ++round)
    {
        qsizetype current = -1;
        for (qsizetype index = 0; index < count; ++index)
        {
            if (!done.at(index) && distance.at(index) < kInfinity &&
                (current < 0 || distance.at(index) < distance.at(current)))
            {
                current = index;
            }
        }
        if (current < 0 || current == 1)
            break;
        done[current] = true;
        for (qsizetype next = 0; next < count; ++next)
        {
            if (done.at(next))
                continue;
            const qreal step =
                QLineF(points.at(current), points.at(next)).length();
            if (distance.at(current) + step >= distance.at(next))
                continue;
            if (!visible(points.at(current), points.at(next)))
                continue;
            distance[next] = distance.at(current) + step;
            previous[next] = current;
        }
    }

    if (previous.at(1) < 0)
        return {from, to};

    QVector<QPointF> path;
    for (qsizetype at = 1; at >= 0; at = previous.at(at))
        path.prepend(points.at(at));
    return simplified(path);
}

QPointF find_free_position(const QSizeF& size, const QRectF& anchor,
                           anchor_side side, const QVector<QRectF>& occupied,
                           const topology_metrics& metrics)
{
    VSS_PROFILE_ZONE("gui.topology.layout.find_free_position");
    QRectF content;
    for (const QRectF& rect : occupied)
        content = content.isNull() ? rect : content.united(rect);

    qreal x = metrics.margin;
    qreal y = metrics.margin;
    if (side == anchor_side::right_of)
    {
        x = anchor.right() + metrics.horizontal_gap;
        y = anchor.center().y() - size.height() / 2.0;
    }
    else if (side == anchor_side::left_of)
    {
        x = anchor.left() - metrics.horizontal_gap - size.width();
        y = anchor.center().y() - size.height() / 2.0;
    }
    else if (!content.isNull())
    {
        x = content.right() + metrics.horizontal_gap;
        y = content.top();
    }

    const qreal half_gap = metrics.vertical_gap / 2.0;
    const auto is_free = [&](const QRectF& candidate)
    {
        const QRectF grown =
            candidate.adjusted(-half_gap, -half_gap, half_gap, half_gap);
        for (const QRectF& rect : occupied)
        {
            if (grown.intersects(
                    rect.adjusted(-half_gap, -half_gap, half_gap, half_gap)))
                return false;
        }
        return true;
    };

    QVector<QRectF> others = occupied;
    others.removeAll(anchor);

    const qreal step = size.height() + metrics.vertical_gap;
    const bool anchored = side != anchor_side::none;
    // First pass wants a link to the anchor that crosses nothing; the second
    // takes any free spot, which the router then goes around.
    for (int pass = 0; pass < (anchored ? 2 : 1); ++pass)
    {
        for (int column = 0; column < 4; ++column)
        {
            const qreal column_x =
                side == anchor_side::left_of
                    ? x - column * (size.width() + metrics.horizontal_gap)
                    : x + column * (size.width() + metrics.horizontal_gap);
            for (int index = 0; index < 64; ++index)
            {
                const int rank = (index + 1) / 2;
                const qreal offset =
                    (index % 2 != 0 ? 1.0 : -1.0) * rank * step;
                const QRectF candidate(QPointF(column_x, y + offset), size);
                if (!is_free(candidate))
                    continue;
                if (anchored && pass == 0)
                {
                    const QLineF link = side == anchor_side::right_of
                                            ? QLineF(right_centre(anchor),
                                                     left_centre(candidate))
                                            : QLineF(right_centre(candidate),
                                                     left_centre(anchor));
                    if (segment_blocked(link, others, metrics.clearance))
                        continue;
                }
                return candidate.topLeft();
            }
        }
    }

    // Nothing near the anchor is free: start a new row under everything.
    return QPointF(
        x, content.isNull() ? y : content.bottom() + metrics.vertical_gap);
}

QRectF push_clear(const QRectF& rect, const QVector<QRectF>& occupied,
                  qreal gap)
{
    VSS_PROFILE_ZONE("gui.topology.layout.push_clear");
    QVector<QRectF> zones;
    zones.reserve(occupied.size());
    for (const QRectF& other : occupied)
        zones.append(other.adjusted(-gap, -gap, gap, gap));

    const auto is_clear = [&zones](const QRectF& candidate)
    {
        for (const QRectF& zone : zones)
        {
            if (candidate.intersects(zone))
                return false;
        }
        return true;
    };
    if (is_clear(rect))
        return rect;

    // Try the four ways out of every zone and keep the shortest one that is
    // clear of all of them: stepping out of one zone can land in the next.
    bool found = false;
    QRectF best = rect;
    qreal best_distance = 0.0;
    const auto consider = [&](const QPointF& shift)
    {
        QRectF candidate = rect;
        candidate.translate(shift);
        if (!is_clear(candidate))
            return;
        const qreal distance = std::abs(shift.x()) + std::abs(shift.y());
        if (!found || distance < best_distance)
        {
            found = true;
            best = candidate;
            best_distance = distance;
        }
    };
    for (const QRectF& zone : zones)
    {
        consider(QPointF(zone.left() - rect.right(), 0.0));
        consider(QPointF(zone.right() - rect.left(), 0.0));
        consider(QPointF(0.0, zone.top() - rect.bottom()));
        consider(QPointF(0.0, zone.bottom() - rect.top()));
    }
    if (found)
        return best;

    // Boxed in on every side: go below everything.
    qreal bottom = zones.first().bottom();
    for (const QRectF& zone : zones)
        bottom = std::max(bottom, zone.bottom());
    QRectF result = rect;
    result.moveTop(bottom);
    return result;
}

QVector<topo_link_placement>
compute_link_geometry(const QVector<topo_link_input>& links,
                      const QHash<vss::endpoint_id, QRectF>& rects,
                      const topology_metrics& metrics)
{
    const qreal clearance = metrics.clearance;
    const qreal soft = std::max(0.0, clearance - kCurveSlack);
    const link_style style = metrics.style;
    // How many links already run between a pair of ports.
    std::map<std::pair<vss::endpoint_id, vss::endpoint_id>, int> lanes;
    QVector<proxy_edge> proxies;
    for (const auto& link : links)
    {
        for (const vss::endpoint_id source : link.sources)
        {
            for (const vss::endpoint_id target : link.targets)
            {
                if (!rects.contains(source) || !rects.contains(target))
                    continue;
                proxies.append({source, target,
                                QLineF(right_centre(rects.value(source)),
                                       left_centre(rects.value(target)))});
            }
        }
    }
    QVector<topo_link_placement> placements;
    for (const auto& link : links)
    {
        QVector<vss::endpoint_id> source_ids;
        QVector<QRectF> sources;
        for (const vss::endpoint_id id : link.sources)
        {
            if (rects.contains(id))
            {
                sources.push_back(rects.value(id));
                source_ids.push_back(id);
            }
        }
        QVector<vss::endpoint_id> target_ids;
        QVector<QRectF> targets;
        for (const vss::endpoint_id id : link.targets)
        {
            if (rects.contains(id))
            {
                targets.push_back(rects.value(id));
                target_ids.push_back(id);
            }
        }
        const bool bus = link.shape == link_shape::bus;
        if (sources.isEmpty() || (targets.isEmpty() && !bus))
            continue;

        topo_link_placement placement;
        placement.id = link.id;

        const auto add_segment = [&placement](const QLineF& line)
        { placement.segments.append(line); };

        QVector<vss::endpoint_id> involved = source_ids;
        involved.append(target_ids);
        const QVector<QRectF> others = rects_except(rects, involved);

        // A split or a join keeps its bar only when the bar and every leg
        // are clear of the other nodes; otherwise it is drawn as one routed
        // line per pair, below.
        bool bar_shape = false;
        if (bus)
        {
            // The members hang off one vertical bar on their left, like taps
            // on a wire; there is no direction, so no arrow.
            std::sort(sources.begin(), sources.end(),
                      [](const QRectF& left, const QRectF& right)
                      { return left.center().y() < right.center().y(); });
            qreal member_left = sources.first().left();
            for (const auto& member : sources)
                member_left = std::min(member_left, member.left());

            const qreal bar_x = member_left - kBusStub;
            for (const auto& member : sources)
            {
                add_segment(QLineF(left_centre(member),
                                   QPointF(bar_x, member.center().y())));
            }
            placement.bar = QLineF(bar_x, sources.first().center().y(), bar_x,
                                   sources.last().center().y());

            // The bar turns into the first and the last member in a round
            // corner; the members in between join it with a plain tap.
            const QRectF top = sources.first();
            const QRectF bottom = sources.last();
            const qreal height = placement.bar.dy();
            const qreal corner = std::min(kBusCorner, height / 2.0);
            if (corner >= 1.0)
            {
                placement.stroke.moveTo(left_centre(top));
                placement.stroke.lineTo(bar_x + corner, top.center().y());
                placement.stroke.quadTo(
                    QPointF(bar_x, top.center().y()),
                    QPointF(bar_x, top.center().y() + corner));
                placement.stroke.lineTo(bar_x, bottom.center().y() - corner);
                placement.stroke.quadTo(
                    QPointF(bar_x, bottom.center().y()),
                    QPointF(bar_x + corner, bottom.center().y()));
                placement.stroke.lineTo(left_centre(bottom));
                for (qsizetype index = 1; index + 1 < sources.size(); ++index)
                {
                    placement.stroke.moveTo(left_centre(sources.at(index)));
                    placement.stroke.lineTo(bar_x,
                                            sources.at(index).center().y());
                }
            }
            else
            {
                for (const QLineF& line : placement.segments)
                {
                    placement.stroke.moveTo(line.p1());
                    placement.stroke.lineTo(line.p2());
                }
            }
            placement.primary_anchor = placement.bar.p1();
            placement.secondary_anchor = placement.primary_anchor;
            bar_shape = true;
        }
        else if (link.shape == link_shape::split && sources.size() == 1 &&
                 targets.size() > 1)
        {
            std::sort(targets.begin(), targets.end(),
                      [](const QRectF& left, const QRectF& right)
                      { return left.center().y() < right.center().y(); });

            const QRectF source = sources.first();
            qreal target_left = targets.first().left();
            for (const auto& target : targets)
                target_left = std::min(target_left, target.left());

            // One trunk from the source to a junction in the middle of the
            // gap, then a curve from the junction to every target.
            const qreal bar_x = (source.right() + target_left) / 2.0;
            const QLineF trunk(right_centre(source),
                               QPointF(bar_x, source.center().y()));
            QVector<QPainterPath> fan;
            bool clear = bar_x > source.right() && bar_x < target_left &&
                         !segment_blocked(trunk, others, clearance);
            for (const auto& target : targets)
            {
                fan.append(fan_out_leg(trunk.p2(), left_centre(target), style));
                clear = clear && !curve_blocked(fan.last(), others, soft);
            }

            if (clear)
            {
                add_segment(trunk);
                placement.stroke.moveTo(trunk.p1());
                placement.stroke.lineTo(trunk.p2());
                for (qsizetype index = 0; index < targets.size(); ++index)
                {
                    add_segment(
                        QLineF(trunk.p2(), left_centre(targets.at(index))));
                    placement.stroke.addPath(fan.at(index));
                    if (link.arrow)
                        placement.arrows.append(arrow_at_end(fan.at(index)));
                }
                placement.primary_anchor = QPointF(
                    (source.right() + bar_x) / 2.0, source.center().y());
                placement.secondary_anchor = placement.primary_anchor;
                bar_shape = true;
            }
        }
        else if (link.shape == link_shape::join && sources.size() > 1 &&
                 targets.size() == 1)
        {
            std::sort(sources.begin(), sources.end(),
                      [](const QRectF& left, const QRectF& right)
                      { return left.center().y() < right.center().y(); });

            const QRectF target = targets.first();
            qreal source_right = sources.first().right();
            for (const auto& source : sources)
                source_right = std::max(source_right, source.right());

            // A curve from every source to a junction in the middle of the
            // gap, then one trunk from the junction to the target.
            const qreal bar_x = (source_right + target.left()) / 2.0;
            const QLineF trunk(QPointF(bar_x, target.center().y()),
                               left_centre(target));
            QVector<QPainterPath> fan;
            bool clear = bar_x > source_right && bar_x < target.left() &&
                         !segment_blocked(trunk, others, clearance);
            for (const auto& source : sources)
            {
                fan.append(fan_in_leg(right_centre(source), trunk.p1(), style));
                clear = clear && !curve_blocked(fan.last(), others, soft);
            }

            if (clear)
            {
                for (qsizetype index = 0; index < sources.size(); ++index)
                {
                    add_segment(
                        QLineF(right_centre(sources.at(index)), trunk.p1()));
                    placement.stroke.addPath(fan.at(index));
                }
                add_segment(trunk);
                placement.stroke.moveTo(trunk.p1());
                placement.stroke.lineTo(trunk.p2());
                if (link.arrow)
                {
                    placement.arrows.append(arrow_head(trunk.p2(), trunk.p1()));
                }
                placement.primary_anchor =
                    QPointF((bar_x + target.left()) / 2.0, target.center().y());
                placement.secondary_anchor = placement.primary_anchor;
                bar_shape = true;
            }
        }

        if (!bar_shape)
        {
            bool first = true;
            for (qsizetype from = 0; from < source_ids.size(); ++from)
            {
                for (qsizetype to = 0; to < target_ids.size(); ++to)
                {
                    const QRectF source = rects.value(source_ids.at(from));
                    const QRectF target = rects.value(target_ids.at(to));
                    const QVector<QPointF> path = edge_path(
                        source_ids.at(from), target_ids.at(to), source, target,
                        others, clearance, proxies, style);
                    for (qsizetype index = 1; index < path.size(); ++index)
                        add_segment(QLineF(path.at(index - 1), path.at(index)));

                    const int lane =
                        lanes[{source_ids.at(from), target_ids.at(to)}]++;
                    QPainterPath curve;
                    if (path.size() == 2 && path.last().x() > path.first().x())
                    {
                        curve = style == link_style::curved
                                    ? lane_curve(path.first(), path.last(),
                                                 lane, others, soft)
                                    : lane_elbow(path.first(), path.last(),
                                                 lane, others, soft);
                    }
                    else
                    {
                        // A routed link on ports that already have a link
                        // moves its bends aside, if there is room.
                        QVector<QPointF> route = path;
                        if (lane > 0)
                        {
                            QVector<QRectF> obstacles = others;
                            obstacles.append(source);
                            obstacles.append(target);
                            const QVector<QPointF> aside =
                                offset_route(path, lane_bow(lane));
                            if (route_clear(aside, sorted_rects(obstacles),
                                            clearance))
                                route = aside;
                        }
                        curve = style == link_style::curved
                                    ? fitted_route(route, others,
                                                   {source, target}, clearance)
                                    : fitted_elbow_route(route, others,
                                                         {source, target},
                                                         clearance);
                    }
                    placement.stroke.addPath(curve);
                    if (link.arrow && path.size() >= 2)
                        placement.arrows.append(arrow_at_end(curve));
                    if (first)
                    {
                        placement.primary_anchor = flat_anchor(curve);
                        placement.secondary_anchor = placement.primary_anchor;
                        first = false;
                    }
                }
            }
        }

        placements.append(placement);
    }
    return placements;
}

QVector<vss::endpoint_id> find_cycle(const QVector<topo_link_input>& links,
                                     const QVector<vss::endpoint_id>& node_ids)
{
    VSS_PROFILE_ZONE("gui.topology.layout.find_cycle");
    QHash<vss::endpoint_id, QVector<vss::endpoint_id>> adjacency;
    for (const auto& link : links)
    {
        for (const vss::endpoint_id source : link.sources)
        {
            for (const vss::endpoint_id target : link.targets)
                adjacency[source].push_back(target);
        }
    }
    for (auto it = adjacency.begin(); it != adjacency.end(); ++it)
    {
        std::sort(it->begin(), it->end());
        it->erase(std::unique(it->begin(), it->end()), it->end());
    }

    QVector<vss::endpoint_id> starts = node_ids;
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    QHash<vss::endpoint_id, int> state;
    QVector<vss::endpoint_id> stack;
    QVector<vss::endpoint_id> loop;

    std::function<bool(vss::endpoint_id)> visit =
        [&](vss::endpoint_id id) -> bool
    {
        state[id] = 1;
        stack.append(id);
        for (const vss::endpoint_id next : adjacency.value(id))
        {
            const int colour = state.value(next, 0);
            if (colour == 0)
            {
                if (visit(next))
                    return true;
            }
            else if (colour == 1)
            {
                const qsizetype start = stack.indexOf(next);
                if (start < 0)
                    continue;
                loop = stack.mid(start);
                loop.append(next);
                return true;
            }
        }
        state[id] = 2;
        stack.removeLast();
        return false;
    };

    for (const vss::endpoint_id id : starts)
    {
        if (state.value(id, 0) == 0 && visit(id))
            break;
    }
    return loop;
}

QVector<vss::link_id> cycle_links(const QVector<topo_link_input>& links,
                                  const QVector<vss::endpoint_id>& cycle)
{
    QSet<vss::endpoint_id> members;
    for (const vss::endpoint_id id : cycle)
        members.insert(id);

    QVector<vss::link_id> result;
    for (const auto& link : links)
    {
        if (members.isEmpty())
            break;
        bool source_in = false;
        for (const vss::endpoint_id id : link.sources)
        {
            if (members.contains(id))
            {
                source_in = true;
                break;
            }
        }
        if (!source_in)
            continue;
        for (const vss::endpoint_id id : link.targets)
        {
            if (members.contains(id))
            {
                if (!result.contains(link.id))
                    result.append(link.id);
                break;
            }
        }
    }
    return result;
}

} // namespace vss::gui::topology
