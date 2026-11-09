//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/view_types.hpp"

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace vss::gui::topology
{

/// How much closer to a node than the routing clearance a drawn curve may come.
/// The router keeps a polyline the full clearance away; a curve that cuts a
/// corner is allowed to give up this much of it.
constexpr qreal kCurveSlack = 8.0;

/// How far a curved link runs level out of a port before it bends.
constexpr qreal kPortRun = 24.0;

/// The corner radius of a right-angle link.
constexpr qreal kElbowRadius = 12.0;

/// The level run at a port when the ports are `dx` apart: `kPortRun`, less when
/// the gap is too narrow for it.
qreal port_run(qreal dx);

/// The curve a link takes between two ports: it runs level out of `from`,
/// bends in one smooth S, and runs level into `to`.  This is the shape the wire
/// has while it is dragged.  When `to` is not right of `from` the bend is at
/// least 40 px, so the curve loops out and back instead of folding.
QPainterPath s_curve(const QPointF& from, const QPointF& to);

/// A smooth curve through every point of `route`, level at the first two and
/// the last two points.  A route of two points to the right is the S-curve.
QPainterPath smooth_route(const QVector<QPointF>& route);

/// A right-angle link between two ports: out of `from` to `bend_x`, up or down,
/// and into `to`, with both corners rounded.  A straight line when the ports
/// are level.
QPainterPath elbow_curve(const QPointF& from, const QPointF& to, qreal bend_x);

/// The right-angle form of `route`: every slanted stretch is replaced by a
/// stretch that bends once or twice at right angles and keeps `clearance` from
/// `obstacles`.  Empty when some stretch has no such form.
QVector<QPointF> orthogonal_route(const QVector<QPointF>& route,
                                  const QVector<QRectF>& obstacles,
                                  qreal clearance);

/// The leg from a junction to a target in a split: a curve, or a vertical run
/// from the junction and one rounded corner into the target.
QPainterPath fan_out_leg(const QPointF& junction, const QPointF& target,
                         link_style style);

/// The leg from a source to the junction of a join, mirrored.
QPainterPath fan_in_leg(const QPointF& source, const QPointF& junction,
                        link_style style);

/// `route` as straight lines with each inner corner rounded by up to `radius`.
QPainterPath rounded_route(const QVector<QPointF>& route, qreal radius);

/// `route` as straight lines.
QPainterPath polyline_route(const QVector<QPointF>& route);

/// True when the curve comes closer than `clearance` to any obstacle.
bool curve_blocked(const QPainterPath& path, const QVector<QRectF>& obstacles,
                   qreal clearance);

/// The smoothest curve through `route` that stays clear: the smooth curve, else
/// the rounded polyline, else the polyline itself (which the router already
/// kept clear).  A curve keeps `clearance - kCurveSlack` from every rectangle
/// of `others` and does not enter a rectangle of `ends`, the two nodes the link
/// is attached to.
QPainterPath fitted_route(const QVector<QPointF>& route,
                          const QVector<QRectF>& others,
                          const QVector<QRectF>& ends, qreal clearance);

/// The right-angle form of `fitted_route`: the orthogonal route with rounded
/// corners, else with square ones, else (no right-angle form exists) the
/// smooth curve.
QPainterPath fitted_elbow_route(const QVector<QPointF>& route,
                                const QVector<QRectF>& others,
                                const QVector<QRectF>& ends, qreal clearance);

/// The unit direction in which the path arrives at its last point.
QPointF end_direction(const QPainterPath& path);

/// Where a label sits: the part of the middle stretch of the path that is
/// closest to level, so the text does not lie across a steep part of a bend.
QPointF flat_anchor(const QPainterPath& path);

/// How many times two paths cross.  A touch within `kPortRun` (and a little) of
/// an end of either path is a shared port, not a crossing.
int path_crossings(const QPainterPath& first, const QPainterPath& second);

} // namespace vss::gui::topology
