//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/topology_layout.hpp"

#include <vss/core/profile.hpp>

#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace vss::gui::topology
{
namespace
{

constexpr qreal kBusStub = 24.0;
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
    const auto order_column = [&columns, &neighbours](int column, int reference)
    {
        QHash<vss::endpoint_id, double> position;
        const auto& reference_ids = columns.at(reference);
        for (int index = 0; index < reference_ids.size(); ++index)
            position.insert(reference_ids.at(index),
                            static_cast<double>(index));

        auto& ids = columns[column];
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

    for (int column = 1; column < columns.size(); ++column)
        order_column(column, column - 1);
    for (int column = static_cast<int>(columns.size()) - 2; column >= 0;
         --column)
        order_column(column, column + 1);

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

    QHash<vss::endpoint_id, QRectF> rects;
    qreal x = metrics.margin;
    for (int column = 0; column < columns.size(); ++column)
    {
        qreal y =
            metrics.margin + (total_height - column_height.at(column)) / 2.0;
        for (const vss::endpoint_id id : columns.at(column))
        {
            const QSizeF size = sizes.value(id);
            const QRectF rect(x + (column_width.at(column) - size.width()) /
                                      2.0,
                              y, size.width(), size.height());
            result.nodes.append({id, rect});
            rects.insert(id, rect);
            y += size.height() + metrics.vertical_gap;
        }
        x += column_width.at(column) + metrics.horizontal_gap;
    }

    result.links = compute_link_geometry(ordered_links, rects, metrics);

    QRectF bounds;
    for (const auto& node : result.nodes)
        note_rect(bounds, node.rect);
    for (const auto& link : result.links)
    {
        for (const QLineF& line : link.segments)
        {
            note_point(bounds, line.p1());
            note_point(bounds, line.p2());
        }
        if (!link.bar.isNull())
        {
            note_point(bounds, link.bar.p1());
            note_point(bounds, link.bar.p2());
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
QVector<QPointF> edge_path(const QRectF& source, const QRectF& target,
                           const QVector<QRectF>& others, qreal clearance)
{
    const QPointF start = right_centre(source);
    const QPointF finish = left_centre(target);
    if (finish.x() > start.x() &&
        !segment_blocked(QLineF(start, finish), others, clearance))
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

/// The midpoint of the longest segment: the label goes where there is room.
QPointF path_anchor(const QVector<QPointF>& path)
{
    qreal best = -1.0;
    QPointF anchor;
    for (qsizetype index = 1; index < path.size(); ++index)
    {
        const QLineF line(path.at(index - 1), path.at(index));
        if (line.length() > best)
        {
            best = line.length();
            anchor = (line.p1() + line.p2()) / 2.0;
        }
    }
    return anchor;
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
                    (index % 2 == 1 ? 1.0 : -1.0) * rank * step;
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

            const qreal bar_x = (source.right() + target_left) / 2.0;
            QVector<QLineF> legs;
            legs.append(QLineF(right_centre(source),
                               QPointF(bar_x, source.center().y())));
            const QLineF bar(bar_x, targets.first().center().y(), bar_x,
                             targets.last().center().y());
            for (const auto& target : targets)
            {
                legs.append(QLineF(bar_x, target.center().y(), target.left(),
                                   target.center().y()));
            }
            bool clear = bar_x > source.right() && bar_x < target_left &&
                         !segment_blocked(bar, others, clearance);
            for (const QLineF& leg : legs)
                clear = clear && !segment_blocked(leg, others, clearance);

            if (clear)
            {
                add_segment(legs.first());
                placement.bar = bar;
                for (const auto& target : targets)
                {
                    add_segment(QLineF(bar_x, target.center().y(),
                                       target.left(), target.center().y()));
                    if (link.arrow)
                    {
                        placement.arrows.append(arrow_head(
                            QPointF(target.left(), target.center().y()),
                            QPointF(bar_x, target.center().y())));
                    }
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

            const qreal bar_x = (source_right + target.left()) / 2.0;
            QVector<QLineF> legs;
            for (const auto& source : sources)
            {
                legs.append(QLineF(right_centre(source),
                                   QPointF(bar_x, source.center().y())));
            }
            const QLineF bar(bar_x, sources.first().center().y(), bar_x,
                             sources.last().center().y());
            const QLineF last(bar_x, target.center().y(), target.left(),
                              target.center().y());
            legs.append(last);
            bool clear = bar_x > source_right && bar_x < target.left() &&
                         !segment_blocked(bar, others, clearance);
            for (const QLineF& leg : legs)
                clear = clear && !segment_blocked(leg, others, clearance);

            if (clear)
            {
                for (const QLineF& leg : legs)
                    add_segment(leg);
                placement.bar = bar;
                if (link.arrow)
                {
                    placement.arrows.append(
                        arrow_head(QPointF(target.left(), target.center().y()),
                                   QPointF(bar_x, target.center().y())));
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
                    const QVector<QPointF> path =
                        edge_path(source, target, others, clearance);
                    for (qsizetype index = 1; index < path.size(); ++index)
                        add_segment(QLineF(path.at(index - 1), path.at(index)));
                    if (link.arrow && path.size() >= 2)
                    {
                        placement.arrows.append(
                            arrow_head(path.last(), path.at(path.size() - 2)));
                    }
                    if (first)
                    {
                        placement.primary_anchor = path_anchor(path);
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
