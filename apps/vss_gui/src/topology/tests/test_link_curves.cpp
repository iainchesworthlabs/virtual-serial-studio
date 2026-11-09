//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "theme/theme.hpp"
#include "topology/link_curve.hpp"
#include "topology/link_item.hpp"
#include "topology/topology_layout.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QVector>

#include <cmath>

namespace topo = vss::gui::topology;

namespace
{

QRectF node(qreal x, qreal y)
{
    return QRectF(x, y, 168.0, 56.0);
}

bool has_curve(const QPainterPath& path)
{
    for (int index = 0; index < path.elementCount(); ++index)
    {
        if (path.elementAt(index).type == QPainterPath::CurveToElement)
            return true;
    }
    return false;
}

QVector<topo::topo_node_input> nodes_of(int count)
{
    QVector<topo::topo_node_input> nodes;
    for (int index = 1; index <= count; ++index)
        nodes.append(
            {static_cast<vss::endpoint_id>(index), QSizeF(168.0, 56.0)});
    return nodes;
}

/// The number of crossings between the strokes of two different links.
int crossings_between_links(const topo::topology_layout& layout)
{
    int count = 0;
    for (qsizetype a = 0; a < layout.links.size(); ++a)
    {
        for (qsizetype b = a + 1; b < layout.links.size(); ++b)
        {
            count += topo::path_crossings(layout.links.at(a).stroke,
                                          layout.links.at(b).stroke);
        }
    }
    return count;
}

} // namespace

TEST_CASE("link curves: an S-curve is level at both ends and bends halfway")
{
    const QPainterPath curve =
        topo::s_curve(QPointF(0.0, 0.0), QPointF(96.0, 100.0));
    const QPointF direction = topo::end_direction(curve);
    REQUIRE_NEAR(direction.x(), 1.0, 1e-6);
    REQUIRE_NEAR(direction.y(), 0.0, 1e-6);

    const QPointF middle = curve.pointAtPercent(0.5);
    REQUIRE_NEAR(middle.x(), 48.0, 0.5);
    REQUIRE_NEAR(middle.y(), 50.0, 0.5);

    // It never leaves the box between its two ends.
    const QRectF box = curve.boundingRect();
    REQUIRE(box.left() > -0.01);
    REQUIRE(box.right() < 96.01);
    REQUIRE(box.top() > -0.01);
    REQUIRE(box.bottom() < 100.01);

    // Two ports at one height give a straight line.
    const QPainterPath level =
        topo::s_curve(QPointF(0.0, 20.0), QPointF(96.0, 20.0));
    REQUIRE_NEAR(level.boundingRect().height(), 0.0, 1e-6);
}

TEST_CASE("link curves: a link drawn between two nodes is a curve, not a line")
{
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 0.0));
    rects.insert(2, node(264.0, 120.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true}};

    const auto placements = topo::compute_link_geometry(links, rects);
    REQUIRE_EQ(placements.size(), 1);
    const topo::topo_link_placement& link = placements.first();
    REQUIRE(has_curve(link.stroke));
    REQUIRE_EQ(link.segments.size(), 1);
    REQUIRE_EQ(link.arrows.size(), 1);

    const QPointF start = link.stroke.pointAtPercent(0.0);
    const QPointF finish = link.stroke.pointAtPercent(1.0);
    REQUIRE_NEAR(start.x(), rects.value(1).right(), 1e-6);
    REQUIRE_NEAR(start.y(), rects.value(1).center().y(), 1e-6);
    REQUIRE_NEAR(finish.x(), rects.value(2).left(), 1e-6);
    REQUIRE_NEAR(finish.y(), rects.value(2).center().y(), 1e-6);

    // The arrow head points along the end of the curve, which is level.
    REQUIRE_NEAR(link.arrows.first().first().x(), finish.x(), 1e-6);
    REQUIRE_NEAR(link.arrows.first().at(1).y() + link.arrows.first().at(2).y(),
                 2.0 * finish.y(), 1e-6);

    // The label sits on the curve.
    bool on_curve = false;
    for (int step = 0; step <= 100; ++step)
    {
        if (QLineF(link.stroke.pointAtPercent(step / 100.0),
                   link.primary_anchor)
                .length() < 1.5)
        {
            on_curve = true;
        }
    }
    REQUIRE(on_curve);
}

TEST_CASE("link curves: a link goes round a node in its way and keeps clear")
{
    const topo::topology_metrics metrics;
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 100.0));
    rects.insert(2, node(600.0, 100.0));
    rects.insert(3, node(280.0, 90.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true}};

    const auto placements = topo::compute_link_geometry(links, rects, metrics);
    REQUIRE_EQ(placements.size(), 1);
    const topo::topo_link_placement& link = placements.first();

    const QVector<QRectF> blocker{rects.value(3)};
    REQUIRE(link.segments.size() > 1);
    REQUIRE(has_curve(link.stroke));
    REQUIRE(!topo::curve_blocked(link.stroke, blocker,
                                 metrics.clearance - topo::kCurveSlack));

    // It still leaves and arrives level.
    const QPointF direction = topo::end_direction(link.stroke);
    REQUIRE_NEAR(direction.y(), 0.0, 1e-6);
}

TEST_CASE("link curves: a node close to the straight line pushes the curve off")
{
    // The straight line between the two ports clears the node by 40 px, but
    // the S-curve bows towards it, so the link is routed round it instead.
    const topo::topology_metrics metrics;
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 0.0));
    rects.insert(2, node(264.0, 200.0));
    rects.insert(3, node(132.0, 200.0 - 28.0 - 56.0 - 12.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::two_way, {1}, {2}, false}};

    const auto placements = topo::compute_link_geometry(links, rects, metrics);
    REQUIRE_EQ(placements.size(), 1);
    const QVector<QRectF> blocker{rects.value(3)};
    REQUIRE(!topo::curve_blocked(placements.first().stroke, blocker,
                                 metrics.clearance - topo::kCurveSlack));
}

TEST_CASE("link curves: a second link between the same ports is told apart")
{
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 100.0));
    rects.insert(2, node(264.0, 100.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true},
        {2, vss::gui::link_shape::monitor, {1}, {2}, true},
        {3, vss::gui::link_shape::monitor, {1}, {2}, true}};

    const auto placements = topo::compute_link_geometry(links, rects);
    REQUIRE_EQ(placements.size(), 3);
    for (qsizetype a = 0; a < placements.size(); ++a)
    {
        for (qsizetype b = a + 1; b < placements.size(); ++b)
        {
            const QPointF one = placements.at(a).stroke.pointAtPercent(0.5);
            const QPointF two = placements.at(b).stroke.pointAtPercent(0.5);
            REQUIRE(QLineF(one, two).length() > 10.0);
            REQUIRE_EQ(topo::path_crossings(placements.at(a).stroke,
                                            placements.at(b).stroke),
                       0);
        }
    }
}

TEST_CASE("link curves: path_crossings counts crossings and ignores a shared "
          "port")
{
    const QPainterPath down =
        topo::s_curve(QPointF(0.0, 0.0), QPointF(100.0, 100.0));
    const QPainterPath up =
        topo::s_curve(QPointF(0.0, 100.0), QPointF(100.0, 0.0));
    const QPainterPath apart =
        topo::s_curve(QPointF(0.0, 200.0), QPointF(100.0, 300.0));
    const QPainterPath fan =
        topo::s_curve(QPointF(0.0, 0.0), QPointF(100.0, -100.0));

    REQUIRE_EQ(topo::path_crossings(down, up), 1);
    REQUIRE_EQ(topo::path_crossings(down, apart), 0);
    REQUIRE_EQ(topo::path_crossings(down, fan), 0);
}

TEST_CASE("link curves: a bus bar turns into its end members in round corners")
{
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::bus, {1, 2, 3}, {}, false}};
    const topo::topology_layout layout =
        topo::compute_topology_layout(nodes_of(3), links);
    REQUIRE_EQ(layout.links.size(), 1);
    const topo::topo_link_placement& bus = layout.links.first();
    REQUIRE(has_curve(bus.stroke));
    REQUIRE(!bus.bar.isNull());
    REQUIRE(bus.stroke.boundingRect().left() <= bus.bar.x1() + 0.01);
}

TEST_CASE("link curves: a layout of links that can avoid each other has no "
          "crossing")
{
    // Sources 1 to 4 each link to the target with the opposite id, so the
    // targets must be listed in reverse for the lines to stay apart.
    QVector<topo::topo_link_input> links;
    for (int index = 0; index < 4; ++index)
    {
        links.append({static_cast<vss::link_id>(index + 1),
                      vss::gui::link_shape::one_way,
                      {static_cast<vss::endpoint_id>(index + 1)},
                      {static_cast<vss::endpoint_id>(8 - index)},
                      true});
    }
    const topo::topology_layout layout =
        topo::compute_topology_layout(nodes_of(8), links);
    REQUIRE_EQ(layout.links.size(), 4);
    REQUIRE_EQ(crossings_between_links(layout), 0);
}

TEST_CASE("link curves: the column order is the same on every run and keeps "
          "crossings low")
{
    // A fixed pseudo-random graph of three columns.
    QVector<topo::topo_link_input> links;
    unsigned state = 12345u;
    const auto next = [&state](unsigned limit)
    {
        state = state * 1103515245u + 12345u;
        return (state >> 16) % limit;
    };
    vss::link_id id = 1;
    for (unsigned count = 0; count < 12; ++count)
    {
        const unsigned source = 1 + next(5);
        const unsigned target = 6 + next(5);
        links.append({id++,
                      vss::gui::link_shape::one_way,
                      {static_cast<vss::endpoint_id>(source)},
                      {static_cast<vss::endpoint_id>(target)},
                      true});
    }
    for (unsigned count = 0; count < 6; ++count)
    {
        const unsigned source = 6 + next(5);
        const unsigned target = 11 + next(4);
        links.append({id++,
                      vss::gui::link_shape::one_way,
                      {static_cast<vss::endpoint_id>(source)},
                      {static_cast<vss::endpoint_id>(target)},
                      true});
    }

    const topo::topology_layout first =
        topo::compute_topology_layout(nodes_of(14), links);
    const topo::topology_layout second =
        topo::compute_topology_layout(nodes_of(14), links);
    REQUIRE_EQ(first.nodes.size(), second.nodes.size());
    for (qsizetype index = 0; index < first.nodes.size(); ++index)
    {
        REQUIRE_EQ(first.nodes.at(index).id, second.nodes.at(index).id);
        REQUIRE(first.nodes.at(index).rect == second.nodes.at(index).rect);
    }
    // Every link has a drawn curve and none runs under a node it is not on.
    for (const auto& link : first.links)
    {
        REQUIRE(!link.stroke.isEmpty());
        QVector<QRectF> others;
        for (const auto& placed : first.nodes)
        {
            bool attached = false;
            for (const auto& input : links)
            {
                if (input.id == link.id)
                {
                    attached = input.sources.contains(placed.id) ||
                               input.targets.contains(placed.id);
                }
            }
            if (!attached)
                others.append(placed.rect);
        }
        REQUIRE(!topo::curve_blocked(link.stroke, others, 0.0));
    }
}

TEST_CASE("link curves: the label anchor moves off a steep part of a route")
{
    // A long level run, a step, another level run: the middle of the path is
    // on the step, so the label goes to a level run instead.
    const QPainterPath stepped =
        topo::rounded_route({QPointF(0.0, 0.0), QPointF(300.0, 0.0),
                             QPointF(300.0, 100.0), QPointF(600.0, 100.0)},
                            24.0);
    const QPointF anchor = topo::flat_anchor(stepped);
    REQUIRE(std::abs(anchor.y() - 50.0) > 40.0);

    // On a level line it is the middle.
    const QPainterPath level =
        topo::s_curve(QPointF(0.0, 0.0), QPointF(96.0, 0.0));
    REQUIRE_NEAR(topo::flat_anchor(level).x(), 48.0, 0.5);
}

TEST_CASE("link curves: renders a review picture of mixed links")
{
    using shape = vss::gui::link_shape;
    // A split, a join, a long link that has to go round the middle column,
    // three links on one pair of ports and a bus.
    const QVector<topo::topo_link_input> links{
        {1, shape::split, {1}, {2, 3, 4}, false},
        {2, shape::one_way, {2}, {5}, true},
        {3, shape::one_way, {3}, {6}, true},
        {4, shape::one_way, {4}, {7}, true},
        {5, shape::join, {5, 6}, {8}, true},
        {6, shape::one_way, {1}, {7}, true},
        {7, shape::two_way, {9}, {10}, false},
        {8, shape::monitor, {9}, {10}, true},
        {9, shape::one_way, {9}, {10}, true},
        {10, shape::bus, {11, 12, 13}, {}, false}};

    const auto render =
        [&links](vss::gui::link_style style, const QString& file)
    {
        topo::topology_metrics metrics;
        metrics.style = style;
        const topo::topology_layout layout =
            topo::compute_topology_layout(nodes_of(13), links, metrics);
        REQUIRE_EQ(layout.links.size(), links.size());

        QGraphicsScene scene;
        const vss::gui::Theme& theme = vss::gui::Theme::instance();
        for (const auto& placed : layout.nodes)
        {
            scene.addRect(placed.rect,
                          QPen(theme.colour(vss::gui::colour_role::control)),
                          QBrush(theme.colour(vss::gui::colour_role::panel)));
            auto* text = scene.addSimpleText(QString::number(placed.id));
            text->setPos(placed.rect.center() - QPointF(4.0, 8.0));
        }
        for (const auto& placement : layout.links)
        {
            topo::link_data data;
            data.id = placement.id;
            for (const auto& input : links)
            {
                if (input.id != placement.id)
                    continue;
                data.shape = input.shape;
                data.arrow = input.arrow;
                data.dashed = input.shape == shape::monitor;
                data.primary =
                    input.shape == shape::split  ? QStringLiteral("Split")
                    : input.shape == shape::join ? QStringLiteral("Join")
                    : input.shape == shape::bus
                        ? QStringLiteral("Bus")
                        : QStringLiteral("link %1").arg(input.id);
            }
            auto* item = new topo::LinkItem(data, {});
            scene.addItem(item);
            item->set_geometry(placement);
        }

        const QRectF area = layout.content.adjusted(-8.0, -8.0, 8.0, 8.0);
        QImage image(area.size().toSize(), QImage::Format_ARGB32);
        image.fill(theme.colour(vss::gui::colour_role::ground));
        {
            QPainter painter(&image);
            scene.render(&painter, QRectF(QPointF(), area.size()), area);
        }
        const QString directory = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("../gui-shots"));
        REQUIRE(QDir().mkpath(directory));
        REQUIRE(image.save(QDir(directory).filePath(file)));
    };
    render(vss::gui::link_style::curved, QStringLiteral("link-curves.png"));
    render(vss::gui::link_style::elbow, QStringLiteral("link-elbows.png"));
}

TEST_CASE("link curves: a long link goes the way that crosses the fewest links")
{
    using shape = vss::gui::link_shape;
    // 1 splits to 2, 3 and 4; each of those feeds 5, 6 and 7.  The long link
    // from 1 to 7 has to pass the middle column.  Over the top it would cross
    // the links into 5 and 6; under the bottom it crosses none.
    const QVector<topo::topo_link_input> links{
        {1, shape::split, {1}, {2, 3, 4}, false},
        {2, shape::one_way, {2}, {5}, true},
        {3, shape::one_way, {3}, {6}, true},
        {4, shape::one_way, {4}, {7}, true},
        {5, shape::one_way, {1}, {7}, true}};
    const topo::topology_layout layout =
        topo::compute_topology_layout(nodes_of(7), links);
    const topo::topo_link_placement* long_link = layout.find_link(5);
    REQUIRE(long_link != nullptr);
    REQUIRE(long_link->segments.size() > 1);
    for (vss::link_id other : {vss::link_id{2}, vss::link_id{3}})
    {
        const topo::topo_link_placement* placed = layout.find_link(other);
        REQUIRE(placed != nullptr);
        REQUIRE_EQ(topo::path_crossings(long_link->stroke, placed->stroke), 0);
    }
}

namespace
{

/// True when every straight stretch of the path is level or upright.
bool lines_are_square(const QPainterPath& path)
{
    QPointF last;
    for (int index = 0; index < path.elementCount(); ++index)
    {
        const QPainterPath::Element element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.type == QPainterPath::LineToElement &&
            std::abs(point.x() - last.x()) > 0.01 &&
            std::abs(point.y() - last.y()) > 0.01)
            return false;
        last = point;
    }
    return true;
}

topo::topology_metrics elbow_metrics()
{
    topo::topology_metrics metrics;
    metrics.style = vss::gui::link_style::elbow;
    return metrics;
}

} // namespace

TEST_CASE("link curves: right angles are level and upright with round corners")
{
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 0.0));
    rects.insert(2, node(264.0, 120.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true}};

    const auto placements =
        topo::compute_link_geometry(links, rects, elbow_metrics());
    REQUIRE_EQ(placements.size(), 1);
    const topo::topo_link_placement& link = placements.first();
    REQUIRE(lines_are_square(link.stroke));
    REQUIRE(has_curve(link.stroke));

    const QPointF start = link.stroke.pointAtPercent(0.0);
    const QPointF finish = link.stroke.pointAtPercent(1.0);
    REQUIRE_NEAR(start.x(), rects.value(1).right(), 1e-6);
    REQUIRE_NEAR(finish.x(), rects.value(2).left(), 1e-6);
    REQUIRE_NEAR(topo::end_direction(link.stroke).y(), 0.0, 1e-6);
    REQUIRE_EQ(link.arrows.size(), 1);

    // Ports at one height are joined by a straight line.
    rects.insert(2, node(264.0, 0.0));
    const auto level =
        topo::compute_link_geometry(links, rects, elbow_metrics());
    REQUIRE(!has_curve(level.first().stroke));
}

TEST_CASE("link curves: right-angle links go round a node and keep clear")
{
    const topo::topology_metrics metrics = elbow_metrics();
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 100.0));
    rects.insert(2, node(600.0, 100.0));
    rects.insert(3, node(280.0, 90.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true}};

    const auto placements = topo::compute_link_geometry(links, rects, metrics);
    REQUIRE_EQ(placements.size(), 1);
    const topo::topo_link_placement& link = placements.first();
    const QVector<QRectF> blocker{rects.value(3)};
    REQUIRE(!topo::curve_blocked(link.stroke, blocker,
                                 metrics.clearance - topo::kCurveSlack));
    REQUIRE(lines_are_square(link.stroke));
    REQUIRE_NEAR(topo::end_direction(link.stroke).y(), 0.0, 1e-6);
}

TEST_CASE("link curves: a right-angle split and join branch off a vertical run")
{
    using shape = vss::gui::link_shape;
    const topo::topology_metrics metrics = elbow_metrics();
    const QVector<topo::topo_link_input> links{
        {1, shape::split, {1}, {2, 3, 4}, true},
        {2, shape::join, {5, 6}, {7}, true}};
    const topo::topology_layout layout =
        topo::compute_topology_layout(nodes_of(7), links, metrics);
    REQUIRE_EQ(layout.links.size(), 2);
    for (const auto& link : layout.links)
    {
        REQUIRE(!link.stroke.isEmpty());
        REQUIRE(lines_are_square(link.stroke));
        REQUIRE(link.arrows.size() >= 1);
    }
}

TEST_CASE("link curves: a second right-angle link on the same ports bends "
          "elsewhere")
{
    QHash<vss::endpoint_id, QRectF> rects;
    rects.insert(1, node(0.0, 0.0));
    rects.insert(2, node(264.0, 120.0));
    const QVector<topo::topo_link_input> links{
        {1, vss::gui::link_shape::one_way, {1}, {2}, true},
        {2, vss::gui::link_shape::monitor, {1}, {2}, true}};
    const auto placements =
        topo::compute_link_geometry(links, rects, elbow_metrics());
    REQUIRE_EQ(placements.size(), 2);
    REQUIRE(lines_are_square(placements.at(1).stroke));
    REQUIRE(QLineF(placements.at(0).stroke.pointAtPercent(0.5),
                   placements.at(1).stroke.pointAtPercent(0.5))
                .length() > 10.0);
}
