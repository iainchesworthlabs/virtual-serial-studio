//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/topology_view.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "topology/link_curve.hpp"
#include "topology/link_item.hpp"
#include "topology/node_item.hpp"
#include "topology/toolbox.hpp"
#include "topology/topology_text.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QAbstractSlider>
#include <QAccessible>
#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vss::gui::topology
{
namespace
{

/// The central column is about this wide at the 960 by 640 minimum window,
/// where the design asks for the compact 120 by 48 nodes.
constexpr int kCompactWidth = 500;
constexpr qreal kMinZoom = 0.25;
constexpr qreal kMaxZoom = 4.0;
constexpr qreal kFitMaxZoom = 1.25;
constexpr qreal kFitMargin = 24.0;
constexpr qreal kZoomStep = 1.25;
constexpr qreal kArrowStep = 8.0;
constexpr qreal kArrowStepLarge = 32.0;
constexpr qreal kNoteMargin = 12.0;
/// How near a press must be to a handle's centre to take it, in view pixels
/// (the hit area is 24 by 24).
constexpr qreal kHandleHit = 12.0;

/// True when the two snapshots draw exactly the same graph.  Traffic and
/// state counters change many times a second; the topology does not show
/// them, so a refresh must not rebuild the scene for a counter.
bool same_graph(const std::vector<port_view>& left_ports,
                const std::vector<port_view>& right_ports,
                const std::vector<link_view>& left_links,
                const std::vector<link_view>& right_links)
{
    if (left_ports.size() != right_ports.size() ||
        left_links.size() != right_links.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left_ports.size(); ++index)
    {
        const port_view& left = left_ports[index];
        const port_view& right = right_ports[index];
        if (left.id != right.id || left.name != right.name ||
            left.path != right.path || left.kind != right.kind ||
            left.studio_only != right.studio_only ||
            (left.stats.dropped > 0) != (right.stats.dropped > 0))
        {
            return false;
        }
    }
    for (std::size_t index = 0; index < left_links.size(); ++index)
    {
        const link_view& left = left_links[index];
        const link_view& right = right_links[index];
        if (left.id != right.id || left.shape != right.shape ||
            left.mode != right.mode ||
            left.options.policy != right.options.policy ||
            left.owner != right.owner || left.sources != right.sources ||
            left.targets != right.targets)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TopologyView::TopologyView(QWidget* parent) : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("topologyView"));
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setAccessibleName(topology_text::view_name());
    setAccessibleDescription(topology_text::empty_state());
    setFocusPolicy(Qt::StrongFocus);
    // The view takes the focus, never its viewport: the keyboard reaches the
    // nodes through the view.
    viewport()->setFocusPolicy(Qt::NoFocus);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setMinimumSize(240, 160);

    empty_ = new QLabel(topology_text::empty_state(), viewport());
    empty_->setObjectName(QStringLiteral("topologyEmptyState"));
    empty_->setAccessibleName(topology_text::empty_state());
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setFont(Theme::instance().font(text_style::body));
    empty_->setProperty("muted", true);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    hint_ = new QLabel(topology_text::hint(), viewport());
    hint_->setObjectName(QStringLiteral("topologyHint"));
    hint_->setAccessibleName(topology_text::hint());
    hint_->setFont(Theme::instance().font(text_style::caption));
    hint_->setProperty("muted", true);
    hint_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // The zoom controls sit in the bottom-right corner of the canvas.
    zoom_bar_ = new QFrame(viewport());
    zoom_bar_->setObjectName(QStringLiteral("topologyZoomBar"));
    zoom_bar_->setAccessibleName(tr("Zoom controls"));
    zoom_bar_->setAttribute(Qt::WA_StyledBackground, true);
    auto* zoom_layout = new QHBoxLayout(zoom_bar_);
    zoom_layout->setContentsMargins(2, 2, 2, 2);
    zoom_layout->setSpacing(0);
    const auto make_zoom_button = [this, zoom_layout](const QString& name,
                                                       const QString& tip)
    {
        auto* button = new QToolButton(zoom_bar_);
        button->setObjectName(name);
        button->setAccessibleName(tip);
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setAutoRaise(true);
        zoom_layout->addWidget(button);
        return button;
    };
    zoom_out_button_ =
        make_zoom_button(QStringLiteral("topologyZoomOut"), tr("Zoom out"));
    zoom_reset_button_ = make_zoom_button(QStringLiteral("topologyZoomReset"),
                                          tr("Reset zoom to 100%"));
    zoom_reset_button_->setFont(Theme::instance().font(text_style::data));
    zoom_in_button_ =
        make_zoom_button(QStringLiteral("topologyZoomIn"), tr("Zoom in"));
    connect(zoom_out_button_, &QToolButton::clicked, this,
            [this] { zoom_out(); });
    connect(zoom_reset_button_, &QToolButton::clicked, this,
            [this] { reset_zoom(); });
    connect(zoom_in_button_, &QToolButton::clicked, this,
            [this] { zoom_in(); });
    viewport()->installEventFilter(this);
    // A scroll bar action comes from the user (a drag, a click, a key); the
    // view's own moves do not trigger it.  Either one ends automatic fitting.
    connect(horizontalScrollBar(), &QAbstractSlider::actionTriggered, this,
            [this](int) { mark_manual_view(); });
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this,
            [this](int) { mark_manual_view(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                empty_->setFont(Theme::instance().font(text_style::body));
                hint_->setFont(Theme::instance().font(text_style::caption));
                update_zoom_controls();
                scene_->update();
                viewport()->update();
                update_overlays();
            });
    update_zoom_controls();
    update_accessibility();
    update_overlays();
}

TopologyView::~TopologyView() = default;

void TopologyView::set_capabilities(const capabilities& caps)
{
    caps_ = caps;
}

void TopologyView::set_topology(const std::vector<port_view>& ports,
                                const std::vector<link_view>& links)
{
    VSS_PROFILE_ZONE("gui.topology.set_topology");
    if (same_graph(ports_, ports, links_, links))
        return;

    ports_ = ports;
    links_ = links;

    names_.clear();
    node_ids_.clear();
    link_inputs_.clear();

    for (const auto& port : ports_)
    {
        names_.insert(port.id, port.name);
        node_ids_.append(port.id);
    }
    std::sort(node_ids_.begin(), node_ids_.end());

    for (const auto& link : links_)
    {
        topo_link_input input;
        input.id = link.id;
        input.shape = link.shape;
        input.sources = link.sources;
        input.targets = link.targets;
        input.arrow = link.shape == link_shape::one_way ||
                      link.shape == link_shape::monitor;
        link_inputs_.append(input);
    }
    std::sort(link_inputs_.begin(), link_inputs_.end(),
              [](const topo_link_input& left, const topo_link_input& right)
              { return left.id < right.id; });

    const int available =
        viewport()->width() > 0 ? viewport()->width() : width();
    compact_ = available < kCompactWidth;

    rebuild_items();
    update_accessibility();
    update_overlays();

    if (ports_.empty())
    {
        resetTransform();
        auto_fit_ = true;
        update_zoom_controls();
        user_arranged_ = false;
    }
    else if (auto_fit_)
    {
        fit_content();
    }
    else
    {
        // The user has taken over the view: leave it where it is, but make
        // sure a node that just appeared can be seen.
        for (const vss::endpoint_id id : new_nodes_)
        {
            const QRectF rect = node_rect(id);
            if (rect.isValid())
                ensureVisible(rect.adjusted(-16.0, -16.0, 16.0, 16.0));
        }
    }
}

void TopologyView::rebuild_items()
{
    VSS_PROFILE_ZONE("gui.topology.rebuild_items");
    // The scene owns the wire and the ghost; clearing it deletes them.
    wire_item_ = nullptr;
    ghost_ = nullptr;
    scene_->clear();
    node_items_.clear();
    link_items_.clear();
    node_order_.clear();
    link_order_.clear();
    node_sizes_.clear();

    QSet<vss::endpoint_id> linked;
    QSet<vss::endpoint_id> owners;
    QSet<vss::endpoint_id> join_targets;
    for (const auto& link : links_)
    {
        for (const vss::endpoint_id id : link.sources)
            linked.insert(id);
        for (const vss::endpoint_id id : link.targets)
            linked.insert(id);
        if (link.owner != 0)
            owners.insert(link.owner);
        if (link.shape == link_shape::join)
        {
            for (const vss::endpoint_id id : link.targets)
                join_targets.insert(id);
        }
    }

    const QFont name_font = Theme::instance().font(text_style::label);
    const QFont badge_font = Theme::instance().font(text_style::caption);

    QHash<vss::endpoint_id, node_data> data_by_id;
    QVector<topo_node_input> inputs;
    inputs.reserve(static_cast<qsizetype>(ports_.size()));
    for (const auto& port : ports_)
    {
        node_data node;
        node.id = port.id;
        node.name = port.name;
        node.detail = topology_text::node_detail(port);
        node.detail_mono = topology_text::node_detail_is_mono(port);
        node.device = port.kind == vss::endpoint_kind::physical;
        node.large = node.device || join_targets.contains(port.id);
        node.unlinked = !linked.contains(port.id);
        node.owner = owners.contains(port.id);
        node.dropped = port.stats.dropped > 0;
        node.tooltip = topology_text::node_tooltip(port, node.unlinked);
        switch (port.kind)
        {
        case vss::endpoint_kind::mock:
            node.icon = QStringLiteral("fixture");
            node.tile = node_data::tint::violet;
            break;
        case vss::endpoint_kind::physical:
            node.icon = QStringLiteral("device");
            node.tile = node_data::tint::ok;
            break;
        case vss::endpoint_kind::tcp:
        case vss::endpoint_kind::udp:
        case vss::endpoint_kind::rfc2217:
            node.icon = QStringLiteral("network");
            break;
        case vss::endpoint_kind::virtual_port:
            node.icon = port.path.startsWith(QStringLiteral("loopback:"))
                            ? QStringLiteral("loopback")
                            : QStringLiteral("port");
            break;
        }

        const QFont detail_font =
            node.detail_mono ? Theme::instance().font(text_style::data)
                             : Theme::instance().font(text_style::caption);
        const QSizeF size =
            node_size_for(node, compact_, name_font, detail_font, badge_font);

        data_by_id.insert(port.id, node);
        node_sizes_.insert(port.id, size);
        inputs.append({port.id, size});
    }

    {
        VSS_PROFILE_ZONE("gui.topology.compute_layout");
        topology_metrics metrics;
        metrics.style = link_style_;
        layout_ = compute_topology_layout(inputs, link_inputs_, metrics);
    }
    place_nodes();

    for (const auto& placement : layout_.nodes)
    {
        const auto found = data_by_id.constFind(placement.id);
        if (found == data_by_id.constEnd())
            continue;

        node_item_callbacks callbacks;
        callbacks.pressed = [this](vss::endpoint_id id)
        {
            setFocus(Qt::MouseFocusReason);
            // Pressing one of several selected nodes keeps the selection, so
            // the group can be dragged; any other node starts a new one.
            if (!multi_ports_.contains(id))
                clear_multi();
            const qsizetype index = focus_index_of_port(id);
            if (index >= 0)
                focus_at(index, true);
        };
        callbacks.double_clicked = [this](vss::endpoint_id id)
        {
            setFocus(Qt::MouseFocusReason);
            const qsizetype index = focus_index_of_port(id);
            if (index >= 0)
                focus_at(index, true);
            Q_EMIT inspect_requested(id);
        };
        callbacks.hover_entered = [](vss::endpoint_id) {};
        callbacks.hover_left = [](vss::endpoint_id) {};
        callbacks.dragged = [this](vss::endpoint_id id, const QPointF& position)
        {
            begin_drag();
            const QPointF delta = position - positions_.value(id);
            positions_.insert(id, position);
            if (multi_ports_.contains(id))
                move_selection_by(delta, id);
            apply_positions(false);
        };
        callbacks.drag_finished = [this](vss::endpoint_id id)
        {
            dragging_ = false;
            user_arranged_ = true;
            if (multi_ports_.contains(id))
            {
                for (const vss::endpoint_id other : selected_ports())
                {
                    if (other != id)
                        settle_node(other);
                }
            }
            settle_node(id);
        };

        auto* item = new NodeItem(found.value(), placement.rect.size(),
                                  std::move(callbacks));
        item->set_editing(editing_);
        item->setPos(positions_.value(placement.id));
        scene_->addItem(item);
        node_items_.insert(placement.id, item);
        node_order_.append(placement.id);
    }

    QHash<vss::link_id, const link_view*> views;
    for (const auto& link : links_)
        views.insert(link.id, &link);

    for (const auto& placement : layout_.links)
    {
        const auto view = views.constFind(placement.id);
        if (view == views.constEnd())
            continue;

        link_data link;
        link.id = view.value()->id;
        link.shape = view.value()->shape;
        link.primary = topology_text::link_primary_label(link.shape);
        link.secondary = topology_text::link_secondary_label(*view.value());
        link.accessible_name = topology_text::link_name(*view.value(), names_);
        link.dashed = link.shape == link_shape::monitor;
        link.arrow = link.shape == link_shape::one_way ||
                     link.shape == link_shape::monitor;

        link_item_callbacks callbacks;
        callbacks.pressed = [this](vss::link_id id)
        {
            setFocus(Qt::MouseFocusReason);
            // As with a node, a plain press starts a new selection. Without
            // this the ports an Add left selected stay selected, and Delete
            // removes them instead of the link that was clicked.
            if (!multi_links_.contains(id))
                clear_multi();
            const qsizetype index = focus_index_of_link(id);
            if (index >= 0)
                focus_at(index, true);
        };
        callbacks.hover_entered = [](vss::link_id) {};
        callbacks.hover_left = [](vss::link_id) {};

        auto* item = new LinkItem(std::move(link), std::move(callbacks));
        item->set_geometry(placement);
        scene_->addItem(item);
        link_items_.insert(placement.id, item);
        link_order_.append(placement.id);
    }

    for (auto it = multi_ports_.begin(); it != multi_ports_.end();)
        it = node_items_.contains(*it) ? std::next(it) : multi_ports_.erase(it);
    for (auto it = multi_links_.begin(); it != multi_links_.end();)
        it = link_items_.contains(*it) ? std::next(it) : multi_links_.erase(it);
    if (wire_from_ != 0 && !node_items_.contains(wire_from_))
        wire_from_ = 0;
    if (wire_from_ != 0)
    {
        for (const vss::endpoint_id id : node_order_)
            node_items_.value(id)->set_wiring(true, id == wire_from_);
    }
    if (selected_port_ != 0 && !node_items_.contains(selected_port_))
        selected_port_ = 0;
    if (selected_link_ != 0 && !link_items_.contains(selected_link_))
        selected_link_ = 0;
    if (focused_port_ != 0 && !node_items_.contains(focused_port_))
        focused_port_ = 0;
    if (focused_link_ != 0 && !link_items_.contains(focused_link_))
        focused_link_ = 0;
    if (focused_port_ != 0)
        focus_index_ = focus_index_of_port(focused_port_);
    else if (focused_link_ != 0)
        focus_index_ = focus_index_of_link(focused_link_);
    else
        focus_index_ = -1;

    apply_positions();
    refresh_focus_visuals();

    if (!loop_links_.isEmpty())
    {
        QSet<vss::endpoint_id> members;
        for (const vss::endpoint_id id : loop_nodes_)
            members.insert(id);
        for (const vss::endpoint_id id : node_order_)
            node_items_.value(id)->set_in_loop(members.contains(id));
        for (const vss::link_id id : link_order_)
            link_items_.value(id)->set_in_loop(loop_links_.contains(id));
    }
}

void TopologyView::place_nodes()
{
    QSet<vss::endpoint_id> known;
    for (auto it = positions_.begin(); it != positions_.end();)
    {
        if (!node_sizes_.contains(it.key()))
        {
            it = positions_.erase(it);
        }
        else
        {
            known.insert(it.key());
            ++it;
        }
    }

    new_nodes_.clear();
    if (pending_drop_)
    {
        // A tile was dropped at a point: the nodes that are new keep the
        // shape the automatic layout gives them, moved to the drop point.
        // Every other node stays where it is.
        const QPointF drop = *pending_drop_;
        pending_drop_.reset();
        if (!user_arranged_ || positions_.isEmpty())
        {
            positions_.clear();
            for (const auto& placement : layout_.nodes)
            {
                if (known.contains(placement.id))
                    positions_.insert(placement.id, placement.rect.topLeft());
            }
        }
        QRectF bounds;
        QVector<QRectF> occupied;
        for (const auto& placement : layout_.nodes)
        {
            if (known.contains(placement.id))
            {
                occupied.append(QRectF(positions_.value(placement.id),
                                       node_sizes_.value(placement.id)));
            }
            else
            {
                bounds = bounds.isNull() ? placement.rect
                                         : bounds.united(placement.rect);
            }
        }
        for (const auto& placement : layout_.nodes)
        {
            if (known.contains(placement.id))
                continue;
            QRectF rect(drop + (placement.rect.topLeft() - bounds.topLeft()),
                        placement.rect.size());
            rect = push_clear(rect, occupied, topology_metrics{}.drop_gap);
            positions_.insert(placement.id, rect.topLeft());
            occupied.append(rect);
            new_nodes_.append(placement.id);
        }
        user_arranged_ = true;
        return;
    }
    if (!user_arranged_ || positions_.isEmpty())
    {
        // Nothing has been moved by hand: the automatic layout decides every
        // position, so a new link can move a whole column.
        positions_.clear();
        for (const auto& placement : layout_.nodes)
        {
            positions_.insert(placement.id, placement.rect.topLeft());
            if (!known.contains(placement.id))
                new_nodes_.append(placement.id);
        }
        return;
    }

    // The user arranged the nodes: keep them where they are and put each new
    // node in free space next to the node it connects to.
    QVector<QRectF> occupied;
    for (const auto& placement : layout_.nodes)
    {
        if (known.contains(placement.id))
        {
            occupied.append(QRectF(positions_.value(placement.id),
                                   node_sizes_.value(placement.id)));
        }
    }

    for (const auto& placement : layout_.nodes)
    {
        if (known.contains(placement.id))
            continue;

        QRectF anchor;
        anchor_side side = anchor_side::none;
        for (const auto& link : link_inputs_)
        {
            if (link.targets.contains(placement.id))
            {
                for (const vss::endpoint_id other : link.sources)
                {
                    if (other != placement.id && positions_.contains(other))
                    {
                        anchor = QRectF(positions_.value(other),
                                        node_sizes_.value(other));
                        side = anchor_side::right_of;
                        break;
                    }
                }
            }
            if (side == anchor_side::none &&
                link.sources.contains(placement.id))
            {
                for (const vss::endpoint_id other : link.targets)
                {
                    if (other != placement.id && positions_.contains(other))
                    {
                        anchor = QRectF(positions_.value(other),
                                        node_sizes_.value(other));
                        side = anchor_side::left_of;
                        break;
                    }
                }
            }
            if (side != anchor_side::none)
                break;
        }

        const QSizeF size = node_sizes_.value(placement.id);
        const QPointF position =
            find_free_position(size, anchor, side, occupied);
        positions_.insert(placement.id, position);
        occupied.append(QRectF(position, size));
        new_nodes_.append(placement.id);
    }

    // A change of density can make a kept node overlap its neighbour.
    QVector<QRectF> accepted;
    for (const auto& placement : layout_.nodes)
    {
        QRectF rect(positions_.value(placement.id),
                    node_sizes_.value(placement.id));
        rect = push_clear(rect, accepted, 0.0);
        positions_.insert(placement.id, rect.topLeft());
        accepted.append(rect);
    }
}

void TopologyView::set_link_style(link_style style)
{
    if (link_style_ == style)
        return;
    link_style_ = style;
    apply_positions(true);
}

void TopologyView::apply_positions(bool refresh_scene_rect)
{
    QHash<vss::endpoint_id, QRectF> rects;
    for (const auto& placement : layout_.nodes)
    {
        auto* item = node_items_.value(placement.id);
        if (item == nullptr)
            continue;
        const QRectF rect(positions_.value(placement.id),
                          node_sizes_.value(placement.id));
        item->setPos(rect.topLeft());
        rects.insert(placement.id, rect);
    }

    QVector<topo_link_placement> geometry;
    {
        VSS_PROFILE_ZONE("gui.topology.compute_link_geometry");
        topology_metrics metrics;
        metrics.style = link_style_;
        geometry = compute_link_geometry(link_inputs_, rects, metrics);
    }
    for (const auto& placement : geometry)
    {
        if (LinkItem* item = link_items_.value(placement.id))
            item->set_geometry(placement);
    }

    if (refresh_scene_rect)
        update_scene_rect();
}

void TopologyView::update_scene_rect()
{
    QRectF content;
    for (const auto& placement : layout_.nodes)
    {
        if (auto* item = node_items_.value(placement.id))
        {
            const QRectF rect(item->pos(), node_sizes_.value(placement.id));
            content = content.isNull() ? rect : content.united(rect);
        }
    }
    for (const vss::link_id id : link_order_)
    {
        auto* item = link_items_.value(id);
        if (item == nullptr)
            continue;
        const QRectF rect = item->boundingRect();
        if (rect.isValid())
            content = content.isNull() ? rect : content.united(rect);
    }

    if (content.isNull())
    {
        setSceneRect(QRectF(0.0, 0.0, std::max(1, viewport()->width()),
                            std::max(1, viewport()->height())));
        return;
    }

    QRectF rect = content.adjusted(-16.0, -16.0, 16.0, 16.0);
    if (!auto_fit_)
    {
        // Once the user is in charge of the view the scene is never smaller
        // than what they see, so Qt does not re-centre the content when a
        // node is dropped near an edge.
        rect = rect.united(mapToScene(viewport()->rect()).boundingRect());
    }
    setSceneRect(rect);
}

void TopologyView::update_overlays()
{
    const bool has_ports = !ports_.empty();
    const int view_width = std::max(1, viewport()->width());
    const int view_height = std::max(1, viewport()->height());

    empty_->setVisible(!has_ports);
    empty_->setFixedWidth(std::max(120, std::min(360, view_width - 48)));
    empty_->adjustSize();
    empty_->move((view_width - empty_->width()) / 2,
                 (view_height - empty_->height()) / 2);

    zoom_bar_->setVisible(has_ports);
    zoom_bar_->adjustSize();
    zoom_bar_->move(
        view_width - zoom_bar_->width() - static_cast<int>(kNoteMargin),
        view_height - zoom_bar_->height() - static_cast<int>(kNoteMargin));

    // The hint gives way to the zoom controls when the canvas is narrow.
    hint_->adjustSize();
    hint_->setVisible(view_width > hint_->width() + zoom_bar_->width() + 48);
    hint_->move(static_cast<int>(kNoteMargin),
                view_height - hint_->height() - static_cast<int>(kNoteMargin));

    if (loop_note_ != nullptr && loop_note_->isVisible())
    {
        const int width =
            std::min(640, view_width - 2 * static_cast<int>(kNoteMargin));
        if (width > 160)
        {
            loop_note_->setFixedWidth(width);
            loop_note_->adjustSize();
            loop_note_->move((view_width - loop_note_->width()) / 2,
                             static_cast<int>(kNoteMargin));
        }
    }

    empty_->raise();
    hint_->raise();
    zoom_bar_->raise();
    if (loop_note_ != nullptr)
        loop_note_->raise();
}

void TopologyView::update_accessibility()
{
    QString next = topology_text::summary(ports_, links_);
    const qsizetype selected =
        selected_ports().size() + selected_links().size();
    if (editing_ && selected > 1)
    {
        QStringList names;
        for (const vss::endpoint_id id : selected_ports())
            names.append(names_.value(id));
        next += QLatin1Char(' ') +
                tr("%n selected: %1", nullptr, static_cast<int>(selected))
                    .arg(names.join(QStringLiteral(", ")));
    }
    if (next == summary_)
        return;
    summary_ = std::move(next);
    setAccessibleDescription(summary_);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}

void TopologyView::refresh_focus_visuals()
{
    for (const vss::endpoint_id id : node_order_)
    {
        auto* item = node_items_.value(id);
        item->set_state(is_selected_port(id),
                        show_focus_ && id == focused_port_, item->is_hovered());
    }
    for (const vss::link_id id : link_order_)
    {
        auto* item = link_items_.value(id);
        item->set_state(is_selected_link(id),
                        show_focus_ && id == focused_link_, item->is_hovered());
    }
}

bool TopologyView::select_port(vss::endpoint_id id)
{
    const bool port_changed = selected_port_ != id;
    const bool link_cleared = selected_link_ != 0;
    selected_port_ = id;
    selected_link_ = 0;
    if (!port_changed && !link_cleared)
        return false;

    refresh_focus_visuals();
    if (port_changed)
        Q_EMIT port_selected(id);
    if (link_cleared)
        Q_EMIT link_selected(0);
    return true;
}

bool TopologyView::select_link(vss::link_id id)
{
    const bool link_changed = selected_link_ != id;
    const bool port_cleared = selected_port_ != 0;
    selected_link_ = id;
    selected_port_ = 0;
    if (!link_changed && !port_cleared)
        return false;

    refresh_focus_visuals();
    if (link_changed)
        Q_EMIT link_selected(id);
    if (port_cleared)
        Q_EMIT port_selected(0);
    return true;
}

void TopologyView::set_selection(vss::endpoint_id port, vss::link_id link)
{
    selected_port_ = node_items_.contains(port) ? port : 0;
    selected_link_ = link_items_.contains(link) ? link : 0;
    refresh_focus_visuals();
}

qsizetype TopologyView::focus_index_of_port(vss::endpoint_id id) const
{
    return node_order_.indexOf(id);
}

qsizetype TopologyView::focus_index_of_link(vss::link_id id) const
{
    const qsizetype index = link_order_.indexOf(id);
    return index < 0 ? -1 : node_order_.size() + index;
}

void TopologyView::focus_at(qsizetype index, bool select)
{
    VSS_PROFILE_ZONE("gui.topology.focus_at");
    const qsizetype node_total = node_order_.size();
    if (index < 0 || index >= node_total + link_order_.size())
    {
        clear_focus();
        return;
    }

    focus_index_ = index;
    focused_port_ = index < node_total ? node_order_.at(index) : 0;
    focused_link_ = index < node_total ? 0 : link_order_.at(index - node_total);
    show_focus_ = hasFocus();

    // With several items selected, moving the focus does not change the
    // selection: Shift+Space adds or removes the focused item.
    const bool keep_selection =
        editing_ && (!multi_ports_.isEmpty() || !multi_links_.isEmpty());
    if (select && !keep_selection)
    {
        if (focused_port_ != 0)
            select_port(focused_port_);
        else
            select_link(focused_link_);
    }

    refresh_focus_visuals();
    if (wire_from_ != 0)
        update_wire_targets(focused_port_);

    const QRectF rect = focused_port_ != 0 ? node_rect(focused_port_)
                                           : link_bounds(focused_link_);
    if (rect.isValid())
        ensureVisible(rect.adjusted(-16.0, -16.0, 16.0, 16.0));
}

void TopologyView::clear_focus()
{
    focused_port_ = 0;
    focused_link_ = 0;
    focus_index_ = -1;
    refresh_focus_visuals();
}

void TopologyView::nudge_focused(qreal dx, qreal dy)
{
    if (focused_port_ == 0)
        return;
    if (editing_ && multi_ports_.contains(focused_port_))
    {
        move_selection_by(QPointF(dx, dy), focused_port_);
        for (const vss::endpoint_id other : selected_ports())
        {
            if (other != focused_port_)
                settle_node(other);
        }
    }
    move_node(focused_port_,
              node_rect(focused_port_).topLeft() + QPointF(dx, dy));
}

void TopologyView::move_node(vss::endpoint_id id, const QPointF& top_left)
{
    if (!node_items_.contains(id))
        return;
    positions_.insert(id, top_left);
    user_arranged_ = true;
    mark_manual_view();
    settle_node(id);
}

void TopologyView::begin_drag()
{
    if (dragging_)
        return;
    dragging_ = true;
    // Taking hold of a node means the user is arranging the diagram: stop
    // re-fitting, and widen the scene to the visible area first so it does not
    // re-centre under the pointer while the node moves.
    mark_manual_view();
    update_scene_rect();
}

void TopologyView::mark_manual_view()
{
    if (internal_scroll_ || fitting_)
        return;
    auto_fit_ = false;
}

/// Settles a moved node clear of the others and shows it.
void TopologyView::settle_node(vss::endpoint_id id)
{
    QVector<QRectF> others;
    for (const auto& placement : layout_.nodes)
    {
        if (placement.id != id)
        {
            others.append(QRectF(positions_.value(placement.id),
                                 node_sizes_.value(placement.id)));
        }
    }
    const QRectF settled =
        push_clear(QRectF(positions_.value(id), node_sizes_.value(id)), others,
                   topology_metrics{}.drop_gap);
    positions_.insert(id, settled.topLeft());
    apply_positions(true);

    const QRectF rect = node_rect(id);
    if (rect.isValid())
        ensureVisible(rect.adjusted(-16.0, -16.0, 16.0, 16.0));
}

bool TopologyView::focusNextPrevChild(bool next)
{
    const qsizetype total = node_order_.size() + link_order_.size();
    if (total == 0)
        return QGraphicsView::focusNextPrevChild(next);

    qsizetype target = 0;
    if (focus_index_ < 0)
        target = next ? 0 : total - 1;
    else
        target = focus_index_ + (next ? 1 : -1);

    if (target < 0 || target >= total)
    {
        clear_focus();
        return QGraphicsView::focusNextPrevChild(next);
    }

    focus_at(target, true);
    return true;
}

bool TopologyView::event(QEvent* event)
{
    // The window's Remove shortcut (Delete) removes one port at a time and
    // knows nothing of the canvas selection or its Undo step. On the editing
    // canvas, claim Delete first so the canvas deletes what is selected.
    if (event->type() == QEvent::ShortcutOverride && editing_)
    {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete &&
            (!selected_ports().isEmpty() || !selected_links().isEmpty() ||
             focused_port_ != 0 || focused_link_ != 0))
        {
            event->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

void TopologyView::keyPressEvent(QKeyEvent* event)
{
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);

    // Editing-only shortcuts come first. Any key they do not take falls
    // through to the shortcuts that work in every mode, further down.
    if (editing_)
    {
        switch (event->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Alt+Enter opens settings for the focused link, or else for
            // the selected ports.
            if (alt)
            {
                if (focused_link_ != 0)
                    Q_EMIT link_settings_requested(focused_link_);
                else if (!selected_ports().isEmpty())
                    Q_EMIT settings_requested(selected_ports());
                event->accept();
                return;
            }
            // While a wire is being drawn, Enter on another node finishes
            // it by opening the wire menu there.
            if (wire_from_ != 0 && focused_port_ != 0 &&
                focused_port_ != wire_from_)
            {
                open_wire_menu(focused_port_,
                               mapFromScene(node_rect(focused_port_).center()));
                event->accept();
                return;
            }
            break;
        case Qt::Key_A:
            if (control)
            {
                select_all();
                event->accept();
                return;
            }
            break;
        case Qt::Key_D:
            if (control && !selected_ports().isEmpty())
            {
                Q_EMIT duplicate_requested(selected_ports());
                event->accept();
                return;
            }
            break;
        // Ctrl+L starts a wire from the focused node, so wiring can be done
        // without the mouse.
        case Qt::Key_L:
            if (control && focused_port_ != 0)
            {
                begin_wire(focused_port_);
                event->accept();
                return;
            }
            break;
        // Shift+Space toggles the focused item in the selection. A plain
        // Space is held as the pan modifier (see mousePressEvent), and
        // auto-repeat is ignored.
        case Qt::Key_Space:
            if (shift && (focused_port_ != 0 || focused_link_ != 0))
            {
                toggle_selected(focused_port_, focused_link_);
                event->accept();
                return;
            }
            if (!event->isAutoRepeat())
                space_down_ = true;
            event->accept();
            return;
        // Escape cancels a wire in progress.
        case Qt::Key_Escape:
            if (wire_from_ != 0)
            {
                cancel_wire();
                event->accept();
                return;
            }
            break;
        case Qt::Key_Delete:
        {
            // Delete removes the whole selection, or the focused item when
            // nothing is selected.
            QVector<vss::endpoint_id> ports = selected_ports();
            QVector<vss::link_id> links = selected_links();
            if (ports.isEmpty() && links.isEmpty())
            {
                if (focused_port_ != 0)
                    ports.append(focused_port_);
                else if (focused_link_ != 0)
                    links.append(focused_link_);
            }
            if (ports.isEmpty() && links.isEmpty())
                break;
            Q_EMIT delete_requested(ports, links);
            event->accept();
            return;
        }
        default:
            break;
        }
    }

    // These shortcuts work whether or not the view is being edited.
    switch (event->key())
    {
    // Enter inspects the focused node, or reads out the focused link.
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (focused_port_ != 0)
        {
            Q_EMIT inspect_requested(focused_port_);
            event->accept();
            return;
        }
        if (focused_link_ != 0)
        {
            Q_EMIT status_message(
                topology_text::selected(link_description(focused_link_)));
            event->accept();
            return;
        }
        break;
    // Outside editing, Delete removes the focused item, else the single
    // selected one.
    case Qt::Key_Delete:
        if (focused_port_ != 0)
            Q_EMIT remove_port_requested(focused_port_);
        else if (focused_link_ != 0)
            Q_EMIT remove_link_requested(focused_link_);
        else if (selected_port_ != 0)
            Q_EMIT remove_port_requested(selected_port_);
        else if (selected_link_ != 0)
            Q_EMIT remove_link_requested(selected_link_);
        else
            break;
        event->accept();
        return;
    // Shift+F10 and the Menu key open the context menu for the focused item.
    case Qt::Key_F10:
        if (shift)
        {
            open_focused_menu();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Menu:
        open_focused_menu();
        event->accept();
        return;
    // Escape first closes the loop note; a second press clears the selection.
    case Qt::Key_Escape:
        if (loop_note_ != nullptr && loop_note_->isVisible())
        {
            dismiss_loop_note();
            event->accept();
            return;
        }
        if (selected_port_ != 0 || selected_link_ != 0 ||
            !multi_ports_.isEmpty() || !multi_links_.isEmpty())
        {
            clear_multi();
            select_port(0);
            select_link(0);
            refresh_focus_visuals();
            emit_selection_count();
            event->accept();
            return;
        }
        break;
    // Zoom shortcuts: Ctrl+0 fits everything, Ctrl+Plus and Ctrl+Minus zoom.
    case Qt::Key_0:
        if (control)
        {
            fit_content();
            event->accept();
            return;
        }
        break;
    case Qt::Key_1:
        if (control)
        {
            reset_zoom();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (control)
        {
            zoom_in();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Minus:
        if (control)
        {
            zoom_out();
            event->accept();
            return;
        }
        break;
    // Arrow keys move the focused node, in larger steps with Shift. With Ctrl
    // they are left to the base class.
    case Qt::Key_Left:
        if (!control && focused_port_ != 0)
        {
            nudge_focused(-(shift ? kArrowStepLarge : kArrowStep), 0.0);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Right:
        if (!control && focused_port_ != 0)
        {
            nudge_focused(shift ? kArrowStepLarge : kArrowStep, 0.0);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Up:
        if (!control && focused_port_ != 0)
        {
            nudge_focused(0.0, -(shift ? kArrowStepLarge : kArrowStep));
            event->accept();
            return;
        }
        break;
    case Qt::Key_Down:
        if (!control && focused_port_ != 0)
        {
            nudge_focused(0.0, shift ? kArrowStepLarge : kArrowStep);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }

    // Anything not handled above goes to the base class.
    QGraphicsView::keyPressEvent(event);
}

void TopologyView::mousePressEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    if (editing_)
    {
        if (event->button() == Qt::MiddleButton ||
            (event->button() == Qt::LeftButton && space_down_))
        {
            panning_ = true;
            pan_last_ = point;
            mark_manual_view();
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (wire_from_ != 0 && event->button() == Qt::RightButton)
        {
            cancel_wire();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton)
        {
            const QPointF scene_point = mapToScene(point);
            if (const auto handle = handle_at(scene_point))
            {
                setFocus(Qt::MouseFocusReason);
                begin_wire(handle->first);
                const QRectF rect = node_rect(handle->first);
                wire_start_ = handle->second
                                  ? QPointF(rect.right(), rect.center().y())
                                  : QPointF(rect.left(), rect.center().y());
                update_wire(scene_point);
                event->accept();
                return;
            }
            const bool adds =
                event->modifiers().testFlag(Qt::ControlModifier) ||
                event->modifiers().testFlag(Qt::ShiftModifier);
            QGraphicsItem* item = itemAt(point);
            if (adds && item != nullptr)
            {
                setFocus(Qt::MouseFocusReason);
                if (auto* node = dynamic_cast<NodeItem*>(item))
                    toggle_selected(node->id(), 0);
                else if (auto* link = dynamic_cast<LinkItem*>(item))
                    toggle_selected(0, link->id());
                event->accept();
                return;
            }
            if (item == nullptr)
            {
                setFocus(Qt::MouseFocusReason);
                if (wire_from_ != 0)
                    cancel_wire();
                if (!adds)
                {
                    clear_focus();
                    clear_multi();
                    select_port(0);
                    select_link(0);
                    refresh_focus_visuals();
                    emit_selection_count();
                }
                band_active_ = true;
                band_adds_ = adds;
                band_origin_ = point;
                if (band_ == nullptr)
                {
                    band_ = std::make_unique<QRubberBand>(QRubberBand::Rectangle,
                                                          viewport());
                    band_->setObjectName(QStringLiteral("topologyRubberBand"));
                }
                band_->setGeometry(QRect(point, QSize()));
                band_->show();
                event->accept();
                return;
            }
        }
    }

    if (event->button() == Qt::LeftButton && itemAt(point) == nullptr)
    {
        setFocus(Qt::MouseFocusReason);
        clear_focus();
        clear_multi();
        select_port(0);
        select_link(0);
    }
    QGraphicsView::mousePressEvent(event);
}

void TopologyView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    if (panning_)
    {
        const QPoint delta = point - pan_last_;
        pan_last_ = point;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() -
                                        delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    if (band_active_ && band_ != nullptr)
    {
        band_->setGeometry(QRect(band_origin_, point).normalized());
        event->accept();
        return;
    }
    if (wire_from_ != 0 && wire_item_ != nullptr &&
        (event->buttons() & Qt::LeftButton))
    {
        const QPointF scene_point = mapToScene(point);
        update_wire(scene_point);
        update_wire_targets(node_at(scene_point));
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void TopologyView::mouseReleaseEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    if (panning_)
    {
        panning_ = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    if (band_active_)
    {
        band_active_ = false;
        const QRect area = QRect(band_origin_, point).normalized();
        if (band_ != nullptr)
            band_->hide();
        if (area.width() > 3 || area.height() > 3)
        {
            const QRectF scene_area = mapToScene(area).boundingRect();
            QVector<vss::endpoint_id> ports =
                band_adds_ ? selected_ports() : QVector<vss::endpoint_id>{};
            QVector<vss::link_id> links =
                band_adds_ ? selected_links() : QVector<vss::link_id>{};
            for (const vss::endpoint_id id : node_order_)
            {
                if (node_rect(id).intersects(scene_area) && !ports.contains(id))
                    ports.append(id);
            }
            set_multi_selection(ports, links);
        }
        event->accept();
        return;
    }
    if (wire_from_ != 0 && wire_item_ != nullptr &&
        event->button() == Qt::LeftButton)
    {
        const vss::endpoint_id target = node_at(mapToScene(point));
        if (target != 0 && target != wire_from_)
        {
            if (would_loop(wire_from_, target, wire_kind::one_way))
            {
                Q_EMIT status_message(topology_text::loop_note());
                cancel_wire();
            }
            else
            {
                open_wire_menu(target, point);
            }
        }
        else if (target == 0)
        {
            open_wire_new_menu(point);
        }
        else
        {
            cancel_wire();
        }
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void TopologyView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat())
        space_down_ = false;
    QGraphicsView::keyReleaseEvent(event);
}

void TopologyView::dragEnterEvent(QDragEnterEvent* event)
{
    if (!editing_ || !item_from_mime(event->mimeData()))
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    if (ghost_ == nullptr)
    {
        const Theme& theme = Theme::instance();
        ghost_ = scene_->addRect(QRectF(QPointF(), QSizeF(168.0, 56.0)));
        QPen pen(theme.colour(colour_role::accent), 1.0, Qt::DashLine);
        ghost_->setPen(pen);
        ghost_->setBrush(theme.colour(colour_role::accentSoft));
        ghost_->setOpacity(0.6);
        ghost_->setZValue(3.0);
    }
    ghost_->setPos(mapToScene(event->position().toPoint()));
}

void TopologyView::dragMoveEvent(QDragMoveEvent* event)
{
    if (!editing_ || !item_from_mime(event->mimeData()))
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    if (ghost_ != nullptr)
        ghost_->setPos(mapToScene(event->position().toPoint()));
}

void TopologyView::dragLeaveEvent(QDragLeaveEvent* event)
{
    delete ghost_;
    ghost_ = nullptr;
    event->accept();
}

void TopologyView::dropEvent(QDropEvent* event)
{
    delete ghost_;
    ghost_ = nullptr;
    const auto item = item_from_mime(event->mimeData());
    if (!editing_ || !item)
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    const QPointF scene_point = mapToScene(event->position().toPoint());
    // A port tile dropped on a node is linked to it straight away.
    const vss::endpoint_id target = node_at(scene_point);
    if (target != 0 &&
        (*item == canvas_item::port || *item == canvas_item::fixture ||
         *item == canvas_item::loopback))
    {
        const slot_kind kind =
            *item == canvas_item::fixture    ? slot_kind::new_fixture
            : *item == canvas_item::loopback ? slot_kind::new_loopback
                                             : slot_kind::new_port;
        Q_EMIT wire_new_requested(target, kind, wire_kind::two_way,
                                  node_rect(target).topRight() +
                                      QPointF(64.0, 0.0));
        return;
    }
    Q_EMIT add_requested(*item, scene_point, true);
}

void TopologyView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);

    const bool compact_now = viewport()->width() < kCompactWidth;
    if (compact_now != compact_ && !ports_.empty())
    {
        compact_ = compact_now;
        rebuild_items();
    }

    update_overlays();
    if (ports_.empty() || fitting_)
        return;

    // A resize keeps the content in view.  Until the user zooms, scrolls or
    // moves a node, "in view" means fitted, so the content follows the
    // window.  After that the zoom and the centre are the user's: only bring
    // the content back when the window left none of it visible.
    if (auto_fit_)
        fit_content();
    else
        keep_content_in_view();
}

void TopologyView::keep_content_in_view()
{
    internal_scroll_ = true;
    update_scene_rect();
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const QRectF content = scene_->itemsBoundingRect();
    if (!content.isNull() && !visible.intersects(content))
        centerOn(content.center());
    internal_scroll_ = false;
}

void TopologyView::focusInEvent(QFocusEvent* event)
{
    QGraphicsView::focusInEvent(event);
    show_focus_ = true;

    const bool keyboard_entry = event->reason() == Qt::TabFocusReason ||
                                event->reason() == Qt::BacktabFocusReason ||
                                event->reason() == Qt::ShortcutFocusReason;
    if (keyboard_entry && focus_index_ < 0 &&
        (!node_order_.isEmpty() || !link_order_.isEmpty()))
    {
        const qsizetype total = node_order_.size() + link_order_.size();
        focus_at(event->reason() == Qt::BacktabFocusReason ? total - 1 : 0,
                 true);
    }
    refresh_focus_visuals();
}

void TopologyView::focusOutEvent(QFocusEvent* event)
{
    show_focus_ = false;
    refresh_focus_visuals();
    QGraphicsView::focusOutEvent(event);
}

void TopologyView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier))
    {
        const int delta = event->angleDelta().y();
        if (delta != 0)
        {
            zoom_at(std::pow(1.0015, static_cast<qreal>(delta)),
                    event->position().toPoint());
        }
        event->accept();
        return;
    }
    // A plain wheel scrolls a view that is larger than its window; that is the
    // user taking over, so the view stops re-fitting on resize.
    if (horizontalScrollBar()->maximum() > horizontalScrollBar()->minimum() ||
        verticalScrollBar()->maximum() > verticalScrollBar()->minimum())
    {
        mark_manual_view();
    }
    QGraphicsView::wheelEvent(event);
}

void TopologyView::drawBackground(QPainter* painter, const QRectF& rect)
{
    VSS_PROFILE_ZONE("gui.paint.canvas_background");
    // The canvas is the sunken ground with a quiet dot grid, so the raised
    // node cards stand out (design language, `10-design-language.md`).
    const Theme& theme = Theme::instance();
    painter->fillRect(rect, theme.colour(colour_role::sunken));
    constexpr qreal kGrid = 16.0;
    if (transform().m11() * kGrid < 6.0)
        return;
    painter->setPen(QPen(theme.colour(colour_role::line2), 1.5, Qt::SolidLine,
                         Qt::RoundCap));
    const qreal left = std::floor(rect.left() / kGrid) * kGrid;
    const qreal top = std::floor(rect.top() / kGrid) * kGrid;
    const int columns = static_cast<int>((rect.right() - left) / kGrid) + 1;
    const int rows = static_cast<int>((rect.bottom() - top) / kGrid) + 1;
    QVector<QPointF> dots;
    dots.reserve(static_cast<qsizetype>(columns) * rows);
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
            dots.append(QPointF(left + column * kGrid, top + row * kGrid));
    }
    painter->drawPoints(dots.constData(), static_cast<int>(dots.size()));
}

bool TopologyView::eventFilter(QObject* watched, QEvent* event)
{
    // A trackpad pinch (macOS, and Windows precision touchpads where Qt
    // reports it) zooms about the fingers.
    if (watched == viewport() && event->type() == QEvent::NativeGesture)
    {
        const auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture)
        {
            zoom_at(1.0 + gesture->value(), gesture->position().toPoint());
            return true;
        }
    }
    if (watched == viewport() && event->type() == QEvent::ContextMenu)
    {
        auto* context = static_cast<QContextMenuEvent*>(event);
        open_context_menu(context->pos());
        return true;
    }
    return QGraphicsView::eventFilter(watched, event);
}

void TopologyView::open_focused_menu()
{
    if (focused_port_ != 0)
    {
        const QPoint position = mapFromScene(node_rect(focused_port_).center());
        open_context_menu(position);
        return;
    }
    if (focused_link_ != 0)
    {
        const QPoint position = mapFromScene(link_anchor(focused_link_));
        open_context_menu(position);
        return;
    }
    // Nothing is focused: the menu of the canvas itself, so Tidy layout and
    // Fit can be reached from the keyboard too.
    open_context_menu(viewport()->rect().center());
}

void TopologyView::open_context_menu(const QPoint& viewport_position)
{
    VSS_PROFILE_ZONE("gui.topology.open_context_menu");
    QGraphicsItem* item = itemAt(viewport_position);
    if (item == nullptr)
    {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setAccessibleName(topology_text::view_name());
        populate_canvas_menu(menu, mapToScene(viewport_position), true);
        menu->popup(viewport()->mapToGlobal(viewport_position));
        return;
    }

    const QPoint global = viewport()->mapToGlobal(viewport_position);
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(topology_text::view_name());

    if (auto* edited_node = dynamic_cast<NodeItem*>(item);
        edited_node != nullptr && editing_)
    {
        const vss::endpoint_id id = edited_node->id();
        if (!multi_ports_.contains(id))
            clear_multi();
        const qsizetype index = focus_index_of_port(id);
        if (index >= 0)
            focus_at(index, true);
        if (selected_ports().size() > 1)
            populate_selection_menu(menu, id);
        else
            populate_node_menu(menu, id);
    }
    else if (auto* edited_link = dynamic_cast<LinkItem*>(item);
             edited_link != nullptr && editing_)
    {
        const vss::link_id id = edited_link->id();
        const qsizetype index = focus_index_of_link(id);
        if (index >= 0)
            focus_at(index, true);
        populate_link_menu(menu, id);
    }
    else if (auto* node = dynamic_cast<NodeItem*>(item))
    {
        const vss::endpoint_id id = node->id();
        const qsizetype index = focus_index_of_port(id);
        if (index >= 0)
            focus_at(index, true);

        menu->addAction(topology_text::inspect_action(), this,
                        [this, id] { Q_EMIT inspect_requested(id); });
        add_port_menu_actions(menu, this,
                              menu_state_ ? menu_state_(id) : port_menu_state{},
                              [this, id](port_menu_action action)
                              { Q_EMIT port_action_requested(id, action); });
        menu->addAction(topology_text::copy_name_action(), this,
                        [this, id] { Q_EMIT copy_name_requested(id); });
        if (caps_.rename_port)
        {
            menu->addAction(topology_text::rename_action(), this,
                            [this, id] { Q_EMIT rename_requested(id); });
        }
        QAction* connect_action =
            menu->addAction(topology_text::connect_from_here_action(), this,
                            [this, id] { Q_EMIT connect_requested(id); });
        connect_action->setEnabled(node_order_.size() >= 2);
        if (!connect_action->isEnabled())
            connect_action->setToolTip(topology_text::connect_needs_two());
        menu->addSeparator();
        menu->addAction(topology_text::remove_action(), this,
                        [this, id] { Q_EMIT remove_port_requested(id); });
    }
    else if (auto* link = dynamic_cast<LinkItem*>(item))
    {
        const vss::link_id id = link->id();
        const qsizetype index = focus_index_of_link(id);
        if (index >= 0)
            focus_at(index, true);
        menu->addAction(topology_text::remove_link_action(), this,
                        [this, id] { Q_EMIT remove_link_requested(id); });
    }
    else
    {
        delete menu;
        return;
    }

    menu->popup(global);
}

QStringList TopologyView::node_menu_labels(vss::endpoint_id id) const
{
    if (!node_items_.contains(id))
        return {};
    if (editing_)
    {
        QMenu menu;
        const_cast<TopologyView*>(this)->populate_node_menu(&menu, id);
        QStringList labels;
        for (QAction* action : menu.actions())
        {
            if (!action->isSeparator())
                labels << action->text();
        }
        return labels;
    }
    QStringList labels;
    labels << topology_text::inspect_action()
           << port_menu_labels(menu_state_ ? menu_state_(id)
                                           : port_menu_state{})
           << topology_text::copy_name_action();
    if (caps_.rename_port)
        labels << topology_text::rename_action();
    labels << topology_text::connect_from_here_action()
           << topology_text::remove_action();
    return labels;
}

QStringList TopologyView::link_menu_labels(vss::link_id id) const
{
    if (!link_items_.contains(id))
        return {};
    if (editing_)
    {
        QMenu menu;
        const_cast<TopologyView*>(this)->populate_link_menu(&menu, id);
        QStringList labels;
        for (QAction* action : menu.actions())
        {
            if (!action->isSeparator())
                labels << action->text();
        }
        return labels;
    }
    return {topology_text::remove_link_action()};
}

QStringList TopologyView::canvas_menu_labels() const
{
    if (editing_)
    {
        QMenu menu;
        const_cast<TopologyView*>(this)->populate_canvas_menu(&menu, {}, false);
        QStringList labels;
        for (QAction* action : menu.actions())
        {
            if (!action->isSeparator())
                labels << action->text();
        }
        return labels;
    }
    return {topology_text::tidy_layout_action(), topology_text::fit_action()};
}

QStringList TopologyView::selection_menu_labels() const
{
    const QVector<vss::endpoint_id> ports = selected_ports();
    if (ports.isEmpty())
        return {};
    QMenu menu;
    const_cast<TopologyView*>(this)->populate_selection_menu(
        &menu, focused_port_ != 0 ? focused_port_ : ports.first());
    QStringList labels;
    for (QAction* action : menu.actions())
    {
        if (!action->isSeparator())
            labels << action->text();
    }
    return labels;
}

QVector<QLineF> TopologyView::link_segments(vss::link_id id) const
{
    LinkItem* item = link_items_.value(id);
    return item == nullptr ? QVector<QLineF>() : item->geometry().segments;
}

QPainterPath TopologyView::link_stroke(vss::link_id id) const
{
    LinkItem* item = link_items_.value(id);
    return item == nullptr ? QPainterPath() : item->geometry().stroke;
}

void TopologyView::tidy_layout()
{
    if (ports_.empty())
        return;

    positions_.clear();
    for (const auto& placement : layout_.nodes)
        positions_.insert(placement.id, placement.rect.topLeft());
    user_arranged_ = false;
    auto_fit_ = true;
    apply_positions(true);
    fit_content();
    Q_EMIT status_message(topology_text::layout_tidied());
}

void TopologyView::fit_content()
{
    if (ports_.empty())
    {
        resetTransform();
        auto_fit_ = true;
        update_zoom_controls();
        return;
    }

    // Fit means "show everything": the scene shrinks back to the content so
    // nothing but the content decides where the view sits.
    auto_fit_ = true;
    update_scene_rect();

    const QRectF content = scene_->itemsBoundingRect();
    if (content.isNull())
        return;

    const QRectF available =
        QRectF(viewport()->rect())
            .adjusted(kFitMargin, kFitMargin, -kFitMargin, -kFitMargin);
    if (available.width() <= 0.0 || available.height() <= 0.0)
        return;

    qreal scale_factor = std::min(available.width() / content.width(),
                                  available.height() / content.height());
    scale_factor = std::clamp(scale_factor, kMinZoom, kFitMaxZoom);

    fitting_ = true;
    internal_scroll_ = true;
    resetTransform();
    scale(scale_factor, scale_factor);
    centerOn(content.center());
    internal_scroll_ = false;
    fitting_ = false;
    auto_fit_ = true;
    update_zoom_controls();
}

qreal TopologyView::min_zoom() noexcept
{
    return kMinZoom;
}

qreal TopologyView::max_zoom() noexcept
{
    return kMaxZoom;
}

void TopologyView::zoom_at(qreal factor, const QPoint& view_point)
{
    const qreal current = transform().m11();
    if (current <= 0.0 || factor <= 0.0)
        return;
    const qreal target = std::clamp(current * factor, kMinZoom, kMaxZoom);
    if (qFuzzyCompare(current, target))
        return;

    // Keep the scene point under `view_point` where it is, whatever the
    // pointer is doing, so the buttons and the tests behave like the wheel.
    const QGraphicsView::ViewportAnchor previous = transformationAnchor();
    setTransformationAnchor(QGraphicsView::NoAnchor);
    internal_scroll_ = true;
    auto_fit_ = false;
    // The scene must cover the area that will be visible once the point is
    // held where it is, or the scroll bars cannot hold it (a diagram smaller
    // than the window has no room to scroll).
    const QPointF before = mapToScene(view_point);
    const QPointF top_left = before - QPointF(view_point) / target;
    const QRectF wanted(top_left, QSizeF(viewport()->width() / target,
                                         viewport()->height() / target));
    scale(target / current, target / current);
    setSceneRect(sceneRect().united(wanted));
    centerOn(wanted.center());
    internal_scroll_ = false;
    setTransformationAnchor(previous);
    update_zoom_controls();
}

void TopologyView::zoom_in()
{
    zoom_at(kZoomStep, viewport()->rect().center());
}

void TopologyView::zoom_out()
{
    zoom_at(1.0 / kZoomStep, viewport()->rect().center());
}

void TopologyView::reset_zoom()
{
    const qreal current = transform().m11();
    if (current > 0.0)
        zoom_at(1.0 / current, viewport()->rect().center());
}

void TopologyView::update_zoom_controls()
{
    if (zoom_bar_ == nullptr)
        return;
    const Theme& theme = Theme::instance();
    IconProvider& icons = IconProvider::instance();
    zoom_out_button_->setIcon(
        icons.icon(QStringLiteral("minus"), colour_role::ink2, 16));
    zoom_in_button_->setIcon(
        icons.icon(QStringLiteral("add"), colour_role::ink2, 16));

    const qreal current = zoom();
    zoom_out_button_->setEnabled(current > kMinZoom + 1e-6);
    zoom_in_button_->setEnabled(current < kMaxZoom - 1e-6);
    zoom_reset_button_->setFont(theme.font(text_style::data));
    // Wide enough for the widest reading, so the bar does not change size
    // while zooming.
    const QFontMetrics metrics(zoom_reset_button_->font());
    zoom_reset_button_->setMinimumWidth(
        metrics.horizontalAdvance(QStringLiteral("400%")) + 16);
    zoom_reset_button_->setText(
        QString::number(static_cast<int>(std::lround(current * 100.0))) +
        QLatin1Char('%'));
    zoom_bar_->adjustSize();
    update_overlays();
}
qreal TopologyView::zoom() const
{
    return transform().m11();
}

void TopologyView::centre_on_links(const QVector<vss::link_id>& ids)
{
    QRectF bounds;
    for (const vss::link_id id : ids)
    {
        const QRectF rect = link_bounds(id);
        if (!rect.isValid())
            continue;
        bounds = bounds.isNull() ? rect : bounds.united(rect);
    }
    if (bounds.isNull())
        return;

    const QRectF available =
        QRectF(viewport()->rect()).adjusted(24.0, 24.0, -24.0, -24.0);
    qreal scale_factor = std::min(available.width() / bounds.width(),
                                  available.height() / bounds.height());
    scale_factor = std::clamp(scale_factor, 0.3, kFitMaxZoom);

    fitting_ = true;
    resetTransform();
    scale(scale_factor, scale_factor);
    centerOn(bounds.center());
    fitting_ = false;
    auto_fit_ = false;
    update_zoom_controls();
}

bool TopologyView::show_loop_note()
{
    const QVector<vss::endpoint_id> cycle = find_cycle(link_inputs_, node_ids_);
    if (cycle.isEmpty())
    {
        dismiss_loop_note();
        return false;
    }

    loop_nodes_ = cycle;
    loop_links_ = cycle_links(link_inputs_, cycle);

    QSet<vss::endpoint_id> members;
    for (const vss::endpoint_id id : cycle)
        members.insert(id);
    for (const vss::endpoint_id id : node_order_)
        node_items_.value(id)->set_in_loop(members.contains(id));
    for (const vss::link_id id : link_order_)
        link_items_.value(id)->set_in_loop(loop_links_.contains(id));

    if (loop_note_ == nullptr)
    {
        loop_note_ = new NoteBanner(NoteBanner::kind::danger,
                                    topology_text::loop_note(), this);
        loop_note_->setObjectName(QStringLiteral("topologyLoopNote"));
        loop_note_->set_action(topology_text::show_loop(),
                               [this] { centre_on_links(loop_links_); });
    }
    loop_note_->set_text(topology_text::loop_note());
    loop_note_->show();
    loop_note_->raise();
    update_overlays();
    return true;
}

void TopologyView::dismiss_loop_note()
{
    loop_links_.clear();
    loop_nodes_.clear();
    if (loop_note_ != nullptr)
        loop_note_->hide();
    for (const vss::endpoint_id id : node_order_)
        node_items_.value(id)->set_in_loop(false);
    for (const vss::link_id id : link_order_)
        link_items_.value(id)->set_in_loop(false);
}

QString TopologyView::loop_note_text() const
{
    return loop_note_ == nullptr ? QString() : loop_note_->text();
}

int TopologyView::loop_link_count() const
{
    return static_cast<int>(loop_links_.size());
}

int TopologyView::node_count() const
{
    return static_cast<int>(node_items_.size());
}

int TopologyView::link_count() const
{
    return static_cast<int>(link_items_.size());
}

QRectF TopologyView::node_rect(vss::endpoint_id id) const
{
    NodeItem* item = node_items_.value(id);
    if (item == nullptr)
        return {};
    return QRectF(item->pos(), node_sizes_.value(id));
}

QSizeF TopologyView::node_size(vss::endpoint_id id) const
{
    return node_sizes_.value(id);
}

QPointF TopologyView::link_anchor(vss::link_id id) const
{
    LinkItem* item = link_items_.value(id);
    return item == nullptr ? QPointF() : item->geometry().primary_anchor;
}

QRectF TopologyView::link_bounds(vss::link_id id) const
{
    LinkItem* item = link_items_.value(id);
    return item == nullptr ? QRectF() : item->boundingRect();
}

QString TopologyView::link_description(vss::link_id id) const
{
    for (const auto& link : links_)
    {
        if (link.id == id)
            return topology_text::link_name(link, names_);
    }
    return {};
}

QStringList TopologyView::link_descriptions() const
{
    QStringList result;
    for (const vss::link_id id : link_order_)
        result.append(link_description(id));
    return result;
}

bool TopologyView::node_is_unlinked(vss::endpoint_id id) const
{
    NodeItem* item = node_items_.value(id);
    return item != nullptr && item->data().unlinked;
}

bool TopologyView::node_is_device(vss::endpoint_id id) const
{
    NodeItem* item = node_items_.value(id);
    return item != nullptr && item->data().device;
}

QStringList TopologyView::node_badges(vss::endpoint_id id) const
{
    QStringList result;
    NodeItem* item = node_items_.value(id);
    if (item == nullptr)
        return result;
    if (item->data().owner)
        result.append(topology_text::owner_badge());
    if (item->data().dropped)
        result.append(topology_text::dropped_badge());
    if (item->data().unlinked)
        result.append(topology_text::unlinked_badge());
    return result;
}

// -- Advanced canvas ---------------------------------------------------------

void TopologyView::set_editing(bool on)
{
    if (editing_ == on)
        return;
    editing_ = on;
    setAcceptDrops(on);
    viewport()->setAcceptDrops(on);
    const QString empty =
        on ? tr("Drag a port or bundle from the toolbox, or press Enter on "
                "one.")
           : topology_text::empty_state();
    empty_->setText(empty);
    empty_->setAccessibleName(empty);
    update_overlays();
    if (!on)
    {
        cancel_wire();
        clear_multi();
        space_down_ = false;
    }
    for (const vss::endpoint_id id : node_order_)
        node_items_.value(id)->set_editing(on);
    refresh_focus_visuals();
    update_accessibility();
}

bool TopologyView::is_selected_port(vss::endpoint_id id) const
{
    return id != 0 && (id == selected_port_ || multi_ports_.contains(id));
}

bool TopologyView::is_selected_link(vss::link_id id) const
{
    return id != 0 && (id == selected_link_ || multi_links_.contains(id));
}

QVector<vss::endpoint_id> TopologyView::selected_ports() const
{
    QVector<vss::endpoint_id> out;
    for (const vss::endpoint_id id : node_order_)
    {
        if (is_selected_port(id))
            out.append(id);
    }
    return out;
}

QVector<vss::link_id> TopologyView::selected_links() const
{
    QVector<vss::link_id> out;
    for (const vss::link_id id : link_order_)
    {
        if (is_selected_link(id))
            out.append(id);
    }
    return out;
}

void TopologyView::clear_multi()
{
    multi_ports_.clear();
    multi_links_.clear();
}

void TopologyView::emit_selection_count()
{
    update_accessibility();
    Q_EMIT selection_changed(
        static_cast<int>(selected_ports().size() + selected_links().size()));
}

void TopologyView::select_all()
{
    multi_ports_ =
        QSet<vss::endpoint_id>(node_order_.begin(), node_order_.end());
    multi_links_ = QSet<vss::link_id>(link_order_.begin(), link_order_.end());
    if (selected_port_ == 0 && selected_link_ == 0 && !node_order_.isEmpty())
        select_port(node_order_.first());
    refresh_focus_visuals();
    emit_selection_count();
}

void TopologyView::set_multi_selection(const QVector<vss::endpoint_id>& ports,
                                       const QVector<vss::link_id>& links)
{
    multi_ports_.clear();
    multi_links_.clear();
    for (const vss::endpoint_id id : ports)
    {
        if (node_items_.contains(id))
            multi_ports_.insert(id);
    }
    for (const vss::link_id id : links)
    {
        if (link_items_.contains(id))
            multi_links_.insert(id);
    }
    if (!multi_ports_.isEmpty() && !multi_ports_.contains(selected_port_))
        select_port(selected_ports().first());
    else if (multi_ports_.isEmpty() && !multi_links_.isEmpty() &&
             !multi_links_.contains(selected_link_))
        select_link(selected_links().first());
    else if (multi_ports_.isEmpty() && multi_links_.isEmpty())
    {
        select_port(0);
        select_link(0);
    }
    refresh_focus_visuals();
    emit_selection_count();
}

void TopologyView::toggle_selected(vss::endpoint_id port, vss::link_id link)
{
    if (port != 0 && node_items_.contains(port))
    {
        if (is_selected_port(port))
        {
            multi_ports_.remove(port);
            if (selected_port_ == port)
            {
                selected_port_ = 0;
                const QVector<vss::endpoint_id> rest = selected_ports();
                Q_EMIT port_selected(rest.isEmpty() ? 0 : rest.first());
                selected_port_ = rest.isEmpty() ? 0 : rest.first();
            }
        }
        else
        {
            if (selected_port_ != 0)
                multi_ports_.insert(selected_port_);
            if (selected_link_ != 0)
                multi_links_.insert(selected_link_);
            multi_ports_.insert(port);
            if (selected_port_ == 0)
            {
                selected_port_ = port;
                Q_EMIT port_selected(port);
            }
        }
    }
    else if (link != 0 && link_items_.contains(link))
    {
        if (is_selected_link(link))
        {
            multi_links_.remove(link);
            if (selected_link_ == link)
                selected_link_ = 0;
        }
        else
        {
            if (selected_port_ != 0)
                multi_ports_.insert(selected_port_);
            if (selected_link_ != 0)
                multi_links_.insert(selected_link_);
            multi_links_.insert(link);
        }
    }
    refresh_focus_visuals();
    emit_selection_count();
}

void TopologyView::place_next_at(const QPointF& scene_point)
{
    pending_drop_ = scene_point;
}

void TopologyView::move_selection_by(const QPointF& delta,
                                     vss::endpoint_id except)
{
    for (const vss::endpoint_id id : selected_ports())
    {
        if (id != except)
            positions_.insert(id, positions_.value(id) + delta);
    }
}

std::optional<std::pair<vss::endpoint_id, bool>>
TopologyView::handle_at(const QPointF& scene_point) const
{
    const qreal scale = std::max(0.01, transform().m11());
    const qreal reach = kHandleHit / scale;
    for (const vss::endpoint_id id : node_order_)
    {
        const QRectF rect = node_rect(id);
        const QPointF left(rect.left(), rect.center().y());
        const QPointF right(rect.right(), rect.center().y());
        if (std::abs(scene_point.x() - right.x()) <= reach &&
            std::abs(scene_point.y() - right.y()) <= reach)
            return std::make_pair(id, true);
        if (std::abs(scene_point.x() - left.x()) <= reach &&
            std::abs(scene_point.y() - left.y()) <= reach)
            return std::make_pair(id, false);
    }
    return std::nullopt;
}

vss::endpoint_id TopologyView::node_at(const QPointF& scene_point) const
{
    for (const vss::endpoint_id id : node_order_)
    {
        if (node_rect(id).contains(scene_point))
            return id;
    }
    return 0;
}

void TopologyView::begin_wire(vss::endpoint_id from)
{
    if (!editing_ || !node_items_.contains(from))
        return;
    cancel_wire();
    wire_from_ = from;
    const QRectF rect = node_rect(from);
    wire_start_ = QPointF(rect.right(), rect.center().y());
    for (const vss::endpoint_id id : node_order_)
        node_items_.value(id)->set_wiring(true, id == from);
    Q_EMIT status_message(
        tr("Choose the port to link %1 to, then press Enter. Esc cancels.")
            .arg(names_.value(from)));
}

void TopologyView::cancel_wire()
{
    wire_from_ = 0;
    delete wire_item_;
    wire_item_ = nullptr;
    for (const vss::endpoint_id id : node_order_)
    {
        NodeItem* item = node_items_.value(id);
        item->set_wiring(false, false);
        item->set_drop_state(NodeItem::drop_state::none);
    }
}

void TopologyView::update_wire(const QPointF& scene_point)
{
    if (wire_item_ == nullptr)
    {
        wire_item_ = scene_->addPath(QPainterPath());
        QPen pen(Theme::instance().colour(colour_role::accent), 2.0,
                 Qt::DashLine);
        pen.setCapStyle(Qt::RoundCap);
        wire_item_->setPen(pen);
        wire_item_->setZValue(4.0);
    }
    // The wire is drawn the way the link will be once it is made.
    wire_item_->setPath(
        link_style_ == link_style::curved
            ? s_curve(wire_start_, scene_point)
            : elbow_curve(wire_start_, scene_point,
                          (wire_start_.x() + scene_point.x()) / 2.0));
}

void TopologyView::update_wire_targets(vss::endpoint_id hovered)
{
    for (const vss::endpoint_id id : node_order_)
    {
        NodeItem::drop_state state = NodeItem::drop_state::none;
        if (id == hovered && id != wire_from_ && wire_from_ != 0)
        {
            state = would_loop(wire_from_, id, wire_kind::one_way)
                        ? NodeItem::drop_state::refuse
                        : NodeItem::drop_state::accept;
        }
        node_items_.value(id)->set_drop_state(state);
    }
    if (hovered != 0 && hovered != wire_from_ && wire_from_ != 0 &&
        would_loop(wire_from_, hovered, wire_kind::one_way))
        viewport()->setToolTip(topology_text::loop_note());
    else
        viewport()->setToolTip({});
}

bool TopologyView::reaches(vss::endpoint_id from, vss::endpoint_id to) const
{
    // Every route a link gives: a two-way link, a split and a bus carry bytes
    // both ways; a join, a one-way link and a monitor one way.
    QHash<vss::endpoint_id, QVector<vss::endpoint_id>> routes;
    for (const link_view& link : links_)
    {
        const bool both = link.shape == link_shape::two_way ||
                          link.shape == link_shape::split ||
                          link.shape == link_shape::bus;
        if (link.shape == link_shape::bus)
        {
            for (const vss::endpoint_id a : link.sources)
                for (const vss::endpoint_id b : link.sources)
                    if (a != b)
                        routes[a].append(b);
            continue;
        }
        for (const vss::endpoint_id a : link.sources)
        {
            for (const vss::endpoint_id b : link.targets)
            {
                routes[a].append(b);
                if (both)
                    routes[b].append(a);
            }
        }
    }
    QSet<vss::endpoint_id> seen;
    QVector<vss::endpoint_id> pending{from};
    while (!pending.isEmpty())
    {
        const vss::endpoint_id next = pending.takeLast();
        if (next == to)
            return true;
        if (seen.contains(next))
            continue;
        seen.insert(next);
        pending += routes.value(next);
    }
    return false;
}

bool TopologyView::would_loop(vss::endpoint_id from, vss::endpoint_id to,
                              wire_kind kind) const
{
    if (from == 0 || to == 0 || from == to)
        return true;
    // A new route from -> to closes a loop when `to` already reaches `from`;
    // a two-way link adds to -> from as well.
    if (reaches(to, from))
        return true;
    return kind == wire_kind::two_way && reaches(from, to);
}

void TopologyView::finish_wire(vss::endpoint_id to, wire_kind kind)
{
    const vss::endpoint_id from = wire_from_;
    cancel_wire();
    if (from != 0 && to != 0 && from != to)
        Q_EMIT wire_requested(from, to, kind);
}

void TopologyView::finish_wire_new(slot_kind kind, wire_kind wire,
                                   const QPointF& scene_point)
{
    const vss::endpoint_id from = wire_from_;
    cancel_wire();
    if (from != 0)
        Q_EMIT wire_new_requested(from, kind, wire, scene_point);
}

QStringList TopologyView::wire_menu_labels(vss::endpoint_id from,
                                           vss::endpoint_id to) const
{
    return {
        tr("Two-way"),
        tr("One way, from %1 to %2").arg(names_.value(from), names_.value(to)),
        tr("Monitor %1 on %2").arg(names_.value(from), names_.value(to))};
}

void TopologyView::open_wire_menu(vss::endpoint_id to,
                                  const QPoint& viewport_position)
{
    const vss::endpoint_id from = wire_from_;
    if (from == 0 || to == 0 || from == to)
    {
        cancel_wire();
        return;
    }
    if (would_loop(from, to, wire_kind::one_way))
    {
        Q_EMIT status_message(topology_text::loop_note());
        cancel_wire();
        return;
    }
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(
        tr("Link %1 and %2").arg(names_.value(from), names_.value(to)));
    const QStringList labels = wire_menu_labels(from, to);
    const wire_kind kinds[] = {wire_kind::two_way, wire_kind::one_way,
                               wire_kind::monitor};
    QAction* first = nullptr;
    for (int index = 0; index < 3; ++index)
    {
        const wire_kind kind = kinds[index];
        QAction* action =
            menu->addAction(labels.value(index), this, [this, from, to, kind]
                            { Q_EMIT wire_requested(from, to, kind); });
        if (would_loop(from, to, kind))
        {
            action->setEnabled(false);
            action->setToolTip(topology_text::loop_note());
        }
        else if (first == nullptr)
        {
            first = action;
        }
    }
    menu->setActiveAction(first);
    connect(menu, &QMenu::aboutToHide, this, [this] { cancel_wire(); });
    menu->popup(viewport()->mapToGlobal(viewport_position));
}

void TopologyView::open_wire_new_menu(const QPoint& viewport_position)
{
    const vss::endpoint_id from = wire_from_;
    if (from == 0)
        return;
    const QPointF scene_point = mapToScene(viewport_position);
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(tr("New port here"));
    const QPair<QString, slot_kind> kinds[] = {
        {tr("New port here"), slot_kind::new_port},
        {tr("New fixture here"), slot_kind::new_fixture},
        {tr("New loopback here"), slot_kind::new_loopback}};
    for (const auto& [label, kind] : kinds)
    {
        QMenu* sub = menu->addMenu(label);
        const QPair<QString, wire_kind> wires[] = {
            {tr("Two-way"), wire_kind::two_way},
            {tr("One way, to the new port"), wire_kind::one_way},
            {tr("Monitor on the new port"), wire_kind::monitor}};
        for (const auto& [wire_label, wire] : wires)
        {
            sub->addAction(
                wire_label, this, [this, from, kind, wire, scene_point]
                { Q_EMIT wire_new_requested(from, kind, wire, scene_point); });
        }
    }
    connect(menu, &QMenu::aboutToHide, this,
            [this]
            {
                // A submenu hides its parent only after its action ran.
                QMetaObject::invokeMethod(
                    this, [this] { cancel_wire(); }, Qt::QueuedConnection);
            });
    menu->popup(viewport()->mapToGlobal(viewport_position));
}

void TopologyView::populate_node_menu(QMenu* menu, vss::endpoint_id id)
{
    VSS_PROFILE_ZONE("gui.topology.populate_node_menu");
    const auto port =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    const bool network =
        port != ports_.end() && (port->kind == vss::endpoint_kind::tcp ||
                                 port->kind == vss::endpoint_kind::udp);
    const bool copyable = port != ports_.end() && can_duplicate(*port);

    const auto shortcut = [](QAction* action, const QKeySequence& keys)
    {
        action->setShortcut(keys);
        action->setShortcutVisibleInContextMenu(true);
        return action;
    };

    shortcut(menu->addAction(topology_text::inspect_action(), this,
                             [this, id] { Q_EMIT inspect_requested(id); }),
             QKeySequence(Qt::Key_Return));
    QAction* settings =
        shortcut(menu->addAction(tr("Settings..."), this, [this, id]
                                 { Q_EMIT settings_requested({id}); }),
                 QKeySequence(Qt::ALT | Qt::Key_Return));
    if (network)
    {
        settings->setEnabled(false);
        settings->setToolTip(tr("A network port has no line settings."));
    }
    if (caps_.rename_port)
    {
        shortcut(menu->addAction(topology_text::rename_action(), this,
                                 [this, id] { Q_EMIT rename_requested(id); }),
                 QKeySequence(Qt::Key_F2));
    }
    QAction* duplicate =
        shortcut(menu->addAction(tr("Duplicate"), this, [this, id]
                                 { Q_EMIT duplicate_requested({id}); }),
                 QKeySequence(Qt::CTRL | Qt::Key_D));
    if (!copyable)
    {
        duplicate->setEnabled(false);
        duplicate->setToolTip(tr("This port can only be opened once."));
    }
    menu->addSeparator();
    shortcut(menu->addAction(topology_text::connect_from_here_action(), this,
                             [this, id] { begin_wire(id); }),
             QKeySequence(Qt::CTRL | Qt::Key_L));
    add_port_menu_actions(menu, this,
                          menu_state_ ? menu_state_(id) : port_menu_state{},
                          [this, id](port_menu_action action)
                          { Q_EMIT port_action_requested(id, action); });
    menu->addSeparator();
    shortcut(menu->addAction(topology_text::copy_name_action(), this,
                             [this, id] { Q_EMIT copy_name_requested(id); }),
             QKeySequence(Qt::CTRL | Qt::Key_C));
    shortcut(menu->addAction(tr("Delete"), this,
                             [this, id] { Q_EMIT delete_requested({id}, {}); }),
             QKeySequence(Qt::Key_Delete));
}

void TopologyView::add_link_selected_menu(QMenu* menu, vss::endpoint_id hub)
{
    const QVector<vss::endpoint_id> ports = selected_ports();
    QMenu* sub = menu->addMenu(tr("Link selected"));
    const QString name = names_.value(hub);
    const auto add = [this, sub, ports, hub](const QString& label,
                                             canvas_item shape, bool enabled)
    {
        QAction* action = sub->addAction(
            label, this, [this, ports, hub, shape]
            { Q_EMIT link_selected_requested(ports, hub, shape); });
        action->setEnabled(enabled);
        if (!enabled)
            action->setToolTip(QCoreApplication::translate(
                "Advanced", "Not available in this build"));
        return action;
    };
    QAction* two_way = add(tr("Two-way"), canvas_item::pair, ports.size() == 2);
    if (ports.size() != 2)
        two_way->setToolTip(tr("Select exactly two ports."));
    add(tr("Split from %1").arg(name), canvas_item::split, caps_.links);
    add(tr("Join into %1").arg(name), canvas_item::join, caps_.links);
    add(tr("Bus"), canvas_item::bus, caps_.shared_bus);
    add(tr("Switch on %1").arg(name), canvas_item::switch_link,
        caps_.links && caps_.owner);
}

void TopologyView::populate_selection_menu(QMenu* menu, vss::endpoint_id id)
{
    const QVector<vss::endpoint_id> ports = selected_ports();
    add_link_selected_menu(menu, id);
    bool copyable = true;
    for (const vss::endpoint_id port : ports)
    {
        const auto view = std::find_if(ports_.begin(), ports_.end(),
                                       [port](const port_view& candidate)
                                       { return candidate.id == port; });
        copyable = copyable && view != ports_.end() && can_duplicate(*view);
    }
    QAction* duplicate =
        menu->addAction(tr("Duplicate"), this,
                        [this, ports] { Q_EMIT duplicate_requested(ports); });
    duplicate->setEnabled(copyable);
    if (!copyable)
        duplicate->setToolTip(tr("This port can only be opened once."));
    menu->addAction(
        tr("Settings for %n ports...", nullptr, static_cast<int>(ports.size())),
        this, [this, ports] { Q_EMIT settings_requested(ports); });
    menu->addSeparator();
    menu->addAction(tr("Delete"), this, [this, ports, links = selected_links()]
                    { Q_EMIT delete_requested(ports, links); });
}

void TopologyView::populate_link_menu(QMenu* menu, vss::link_id id)
{
    const auto link =
        std::find_if(links_.begin(), links_.end(),
                     [id](const link_view& view) { return view.id == id; });
    QAction* settings =
        menu->addAction(tr("Settings..."), this,
                        [this, id] { Q_EMIT link_settings_requested(id); });
    settings->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    settings->setShortcutVisibleInContextMenu(true);
    if (link != links_.end() && link->shape == link_shape::monitor)
    {
        settings->setEnabled(false);
        settings->setToolTip(tr("A monitor link has no settings."));
    }
    if (link != links_.end() && link->shape == link_shape::one_way)
    {
        menu->addAction(tr("Reverse direction"), this,
                        [this, id] { Q_EMIT reverse_requested(id); });
    }
    menu->addSeparator();
    QAction* remove = menu->addAction(tr("Delete"), this, [this, id]
                                      { Q_EMIT delete_requested({}, {id}); });
    remove->setShortcut(QKeySequence(Qt::Key_Delete));
    remove->setShortcutVisibleInContextMenu(true);
}

void TopologyView::populate_canvas_menu(QMenu* menu, const QPointF& scene_point,
                                        bool at_point)
{
    if (editing_)
    {
        const auto add = [this, scene_point, at_point](QMenu* target,
                                                       const QString& label,
                                                       canvas_item item)
        {
            QAction* action = target->addAction(
                label, this, [this, item, scene_point, at_point]
                { Q_EMIT add_requested(item, scene_point, at_point); });
            action->setEnabled(item_available(caps_, item));
            if (!action->isEnabled())
                action->setToolTip(QCoreApplication::translate(
                    "Advanced", "Not available in this build"));
        };
        add(menu, tr("Add port"), canvas_item::port);
        add(menu, tr("Add fixture"), canvas_item::fixture);
        add(menu, tr("Add loopback"), canvas_item::loopback);
        QMenu* bundles = menu->addMenu(tr("Add bundle"));
        for (const canvas_item item :
             {canvas_item::pair, canvas_item::split, canvas_item::join,
              canvas_item::bus, canvas_item::switch_link, canvas_item::redirect,
              canvas_item::monitor, canvas_item::share, canvas_item::bench})
            add(bundles, item_label(item), item);
        QAction* all =
            menu->addAction(tr("Select all"), this, [this] { select_all(); });
        all->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
        all->setShortcutVisibleInContextMenu(true);
        all->setEnabled(!ports_.empty());
        menu->addSeparator();
    }
    QAction* tidy = menu->addAction(topology_text::tidy_layout_action(), this,
                                    [this] { tidy_layout(); });
    tidy->setEnabled(!ports_.empty());
    QAction* fit = menu->addAction(topology_text::fit_action(), this,
                                   [this] { fit_content(); });
    fit->setEnabled(!ports_.empty());
}

} // namespace vss::gui::topology
