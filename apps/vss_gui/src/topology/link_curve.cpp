//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/link_curve.hpp"

#include "topology/topology_layout.hpp"

#include <vss/core/profile.hpp>

#include <QLineF>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vss::gui::topology
{
namespace
{

/// How far an inner handle reaches along the chord of its segment.
constexpr qreal kHandle = 0.38;
/// How far the handles of the S between two level runs reach, as a share of
/// the distance between the runs.
constexpr qreal kBend = 0.45;
/// The corner radius tried first when a smooth curve is not clear.
constexpr qreal kCornerRadius = 24.0;
/// A touch this close to an end of a path is a shared port: two links out of
/// one port run level together for a short way, then part.
constexpr qreal kPortTouch = kPortRun + 4.0;
/// Two crossings this close are one.
constexpr qreal kSameCrossing = 2.0;
/// Sampling for the label anchor.
constexpr int kAnchorSamples = 48;
/// Half the width of a short label such as "owner" or "two-way".
constexpr qreal kAnchorWindow = 20.0;
constexpr qreal kAnchorMiddlePenalty = 0.05;

QPointF unit_of(const QPointF& vector)
{
    const qreal length = std::hypot(vector.x(), vector.y());
    if (length < 1e-9)
        return QPointF(1.0, 0.0);
    return QPointF(vector.x() / length, vector.y() / length);
}

} // namespace

qreal port_run(qreal dx)
{
    return std::clamp(dx / 4.0, 0.0, kPortRun);
}

QPainterPath s_curve(const QPointF& from, const QPointF& to)
{
    const qreal dx = to.x() - from.x();
    QPainterPath path(from);
    if (dx <= 0.0)
    {
        const qreal bend = 40.0 + std::abs(dx) / 2.0;
        path.cubicTo(from + QPointF(bend, 0.0), to - QPointF(bend, 0.0), to);
        return path;
    }
    if (std::abs(to.y() - from.y()) < 0.5)
    {
        path.lineTo(to);
        return path;
    }

    // Level for a short run, one S between the runs, level again.
    const qreal run = port_run(dx);
    const QPointF start = from + QPointF(run, 0.0);
    const QPointF finish = to - QPointF(run, 0.0);
    const qreal bend = (finish.x() - start.x()) * kBend;
    path.lineTo(start);
    path.cubicTo(start + QPointF(bend, 0.0), finish - QPointF(bend, 0.0),
                 finish);
    path.lineTo(to);
    return path;
}

QPainterPath smooth_route(const QVector<QPointF>& route)
{
    QPainterPath path;
    if (route.size() < 2)
        return path;
    path.moveTo(route.first());
    if (route.size() == 2 && route.last().x() > route.first().x())
        return s_curve(route.first(), route.last());

    const qsizetype last = route.size() - 1;
    // Level at both ends and at the points next to them, which are the end of
    // the run out of a port; the points between follow the way the route goes.
    const auto tangent = [&route, last](qsizetype index)
    {
        if (index <= 1 || index + 1 >= last)
            return QPointF(1.0, 0.0);
        return unit_of(route.at(index + 1) - route.at(index - 1));
    };
    for (qsizetype index = 0; index < last; ++index)
    {
        const QPointF& from = route.at(index);
        const QPointF& to = route.at(index + 1);
        const qreal handle = QLineF(from, to).length() * kHandle;
        path.cubicTo(from + tangent(index) * handle,
                     to - tangent(index + 1) * handle, to);
    }
    return path;
}

QPainterPath polyline_route(const QVector<QPointF>& route)
{
    QPainterPath path;
    if (route.isEmpty())
        return path;
    path.moveTo(route.first());
    for (qsizetype index = 1; index < route.size(); ++index)
        path.lineTo(route.at(index));
    return path;
}

QPainterPath rounded_route(const QVector<QPointF>& route, qreal radius)
{
    QPainterPath path;
    if (route.isEmpty())
        return path;
    path.moveTo(route.first());
    for (qsizetype index = 1; index + 1 < route.size(); ++index)
    {
        const QPointF& corner = route.at(index);
        const qreal before = QLineF(route.at(index - 1), corner).length();
        const qreal after = QLineF(corner, route.at(index + 1)).length();
        const qreal reach = std::min({radius, before / 2.0, after / 2.0});
        const QPointF in = unit_of(corner - route.at(index - 1));
        const QPointF out = unit_of(route.at(index + 1) - corner);
        path.lineTo(corner - in * reach);
        path.quadTo(corner, corner + out * reach);
    }
    if (route.size() > 1)
        path.lineTo(route.last());
    return path;
}

bool curve_blocked(const QPainterPath& path, const QVector<QRectF>& obstacles,
                   qreal clearance)
{
    VSS_PROFILE_ZONE("gui.topology.link.curve_blocked");
    for (const QPolygonF& polygon : path.toSubpathPolygons())
    {
        for (qsizetype index = 1; index < polygon.size(); ++index)
        {
            if (segment_blocked(
                    QLineF(polygon.at(index - 1), polygon.at(index)), obstacles,
                    clearance))
            {
                return true;
            }
        }
    }
    return false;
}

QPainterPath fitted_route(const QVector<QPointF>& route,
                          const QVector<QRectF>& others,
                          const QVector<QRectF>& ends, qreal clearance)
{
    VSS_PROFILE_ZONE("gui.topology.link.fitted_route");
    const qreal soft = std::max(0.0, clearance - kCurveSlack);
    const auto clear = [&](const QPainterPath& candidate)
    {
        return !curve_blocked(candidate, others, soft) &&
               !curve_blocked(candidate, ends, 0.0);
    };

    QPainterPath smooth = smooth_route(route);
    if (clear(smooth))
        return smooth;
    for (const qreal radius : {kCornerRadius, kCornerRadius / 2.0})
    {
        QPainterPath rounded = rounded_route(route, radius);
        if (clear(rounded))
            return rounded;
    }
    return polyline_route(route);
}

QPainterPath elbow_curve(const QPointF& from, const QPointF& to, qreal bend_x)
{
    if (std::abs(to.y() - from.y()) < 0.5)
    {
        QPainterPath line(from);
        line.lineTo(to);
        return line;
    }
    return rounded_route(
        {from, QPointF(bend_x, from.y()), QPointF(bend_x, to.y()), to},
        kElbowRadius);
}

QVector<QPointF> orthogonal_route(const QVector<QPointF>& route,
                                  const QVector<QRectF>& obstacles,
                                  qreal clearance)
{
    if (route.size() < 2)
        return route;
    QVector<QPointF> result{route.first()};
    for (qsizetype index = 1; index < route.size(); ++index)
    {
        const QPointF from = route.at(index - 1);
        const QPointF to = route.at(index);
        if (std::abs(to.x() - from.x()) < 0.5 ||
            std::abs(to.y() - from.y()) < 0.5)
        {
            result.append(to);
            continue;
        }

        // Out and back at the middle, or one corner either way round.
        const qreal middle = (from.x() + to.x()) / 2.0;
        const QVector<QVector<QPointF>> options{
            {QPointF(middle, from.y()), QPointF(middle, to.y())},
            {QPointF(to.x(), from.y())},
            {QPointF(from.x(), to.y())}};
        bool placed = false;
        for (const QVector<QPointF>& corners : options)
        {
            QVector<QPointF> stretch{from};
            stretch.append(corners);
            stretch.append(to);
            bool clear = true;
            for (qsizetype step = 1; step < stretch.size() && clear; ++step)
            {
                clear = !segment_blocked(
                    QLineF(stretch.at(step - 1), stretch.at(step)), obstacles,
                    clearance);
            }
            if (!clear)
                continue;
            result.append(corners);
            result.append(to);
            placed = true;
            break;
        }
        if (!placed)
            return {};
    }
    return result;
}

QPainterPath fan_out_leg(const QPointF& junction, const QPointF& target,
                         link_style style)
{
    if (style == link_style::curved)
        return s_curve(junction, target);

    QPainterPath path(junction);
    const qreal rise = target.y() - junction.y();
    const qreal reach = std::min({kElbowRadius, std::abs(rise) / 2.0,
                                  (target.x() - junction.x()) / 2.0});
    if (std::abs(rise) < 0.5 || reach < 0.5)
    {
        path.lineTo(junction.x(), target.y());
        path.lineTo(target);
        return path;
    }
    const qreal side = rise > 0.0 ? 1.0 : -1.0;
    path.lineTo(junction.x(), target.y() - side * reach);
    path.quadTo(QPointF(junction.x(), target.y()),
                QPointF(junction.x() + reach, target.y()));
    path.lineTo(target);
    return path;
}

QPainterPath fan_in_leg(const QPointF& source, const QPointF& junction,
                        link_style style)
{
    if (style == link_style::curved)
        return s_curve(source, junction);

    QPainterPath path(source);
    const qreal rise = junction.y() - source.y();
    const qreal reach = std::min({kElbowRadius, std::abs(rise) / 2.0,
                                  (junction.x() - source.x()) / 2.0});
    if (std::abs(rise) < 0.5 || reach < 0.5)
    {
        path.lineTo(junction.x(), source.y());
        path.lineTo(junction);
        return path;
    }
    const qreal side = rise > 0.0 ? 1.0 : -1.0;
    path.lineTo(junction.x() - reach, source.y());
    path.quadTo(QPointF(junction.x(), source.y()),
                QPointF(junction.x(), source.y() + side * reach));
    path.lineTo(junction);
    return path;
}

QPainterPath fitted_elbow_route(const QVector<QPointF>& route,
                                const QVector<QRectF>& others,
                                const QVector<QRectF>& ends, qreal clearance)
{
    VSS_PROFILE_ZONE("gui.topology.link.fitted_elbow_route");
    QVector<QRectF> obstacles = others;
    obstacles.append(ends);
    const QVector<QPointF> square =
        orthogonal_route(route, obstacles, clearance);
    if (square.isEmpty())
        return fitted_route(route, others, ends, clearance);

    const qreal soft = std::max(0.0, clearance - kCurveSlack);
    QPainterPath rounded = rounded_route(square, kElbowRadius);
    if (!curve_blocked(rounded, others, soft) &&
        !curve_blocked(rounded, ends, 0.0))
    {
        return rounded;
    }
    return polyline_route(square);
}

QPointF end_direction(const QPainterPath& path)
{
    const int count = path.elementCount();
    if (count < 2)
        return QPointF(1.0, 0.0);
    const QPointF end(path.elementAt(count - 1).x, path.elementAt(count - 1).y);
    for (int index = count - 2; index >= 0; --index)
    {
        const QPointF before(path.elementAt(index).x, path.elementAt(index).y);
        if (QLineF(before, end).length() > 1e-6)
            return unit_of(end - before);
    }
    return QPointF(1.0, 0.0);
}

QPointF flat_anchor(const QPainterPath& path)
{
    if (path.isEmpty())
        return QPointF();
    QVector<QPointF> points;
    points.reserve(kAnchorSamples + 1);
    for (int index = 0; index <= kAnchorSamples; ++index)
    {
        points.append(path.pointAtPercent(static_cast<qreal>(index) /
                                          static_cast<qreal>(kAnchorSamples)));
    }
    const qreal step = path.length() / static_cast<qreal>(kAnchorSamples);
    // Keep a label off the first and last quarter, where it would meet a node.
    const int first = kAnchorSamples / 4;
    const int last = kAnchorSamples * 3 / 4;

    QPointF anchor = points.at(kAnchorSamples / 2);
    qreal best = std::numeric_limits<qreal>::infinity();
    for (int index = first; index <= last; ++index)
    {
        qreal deviation = 0.0;
        for (const QPointF& other : points)
        {
            if (std::abs(other.x() - points.at(index).x()) <= kAnchorWindow)
            {
                deviation = std::max(
                    deviation, std::abs(other.y() - points.at(index).y()));
            }
        }
        const qreal score =
            deviation +
            kAnchorMiddlePenalty * step * std::abs(index - kAnchorSamples / 2);
        if (score < best - 1e-9)
        {
            best = score;
            anchor = points.at(index);
        }
    }
    return anchor;
}

int path_crossings(const QPainterPath& first, const QPainterPath& second)
{
    const QList<QPolygonF> left = first.toSubpathPolygons();
    const QList<QPolygonF> right = second.toSubpathPolygons();

    QVector<QPointF> ports;
    for (const QPolygonF& polygon : left)
    {
        if (!polygon.isEmpty())
            ports << polygon.first() << polygon.last();
    }
    for (const QPolygonF& polygon : right)
    {
        if (!polygon.isEmpty())
            ports << polygon.first() << polygon.last();
    }

    // Two flattened curves that cross at a vertex meet on up to four pairs of
    // segments, so a crossing is counted once per point.
    QVector<QPointF> found;
    for (const QPolygonF& a : left)
    {
        for (const QPolygonF& b : right)
        {
            for (qsizetype i = 1; i < a.size(); ++i)
            {
                for (qsizetype j = 1; j < b.size(); ++j)
                {
                    QPointF at;
                    const auto kind =
                        QLineF(a.at(i - 1), a.at(i))
                            .intersects(QLineF(b.at(j - 1), b.at(j)), &at);
                    if (kind != QLineF::BoundedIntersection)
                        continue;
                    bool at_port = false;
                    for (const QPointF& port : ports)
                    {
                        if (QLineF(port, at).length() < kPortTouch)
                        {
                            at_port = true;
                            break;
                        }
                    }
                    if (at_port)
                        continue;
                    bool known = false;
                    for (const QPointF& other : found)
                    {
                        if (QLineF(other, at).length() < kSameCrossing)
                        {
                            known = true;
                            break;
                        }
                    }
                    if (!known)
                        found.append(at);
                }
            }
        }
    }
    return static_cast<int>(found.size());
}

} // namespace vss::gui::topology
