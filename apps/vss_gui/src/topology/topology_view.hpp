//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_TOPOLOGY_TOPOLOGY_VIEW_HPP
#define VSS_GUI_TOPOLOGY_TOPOLOGY_VIEW_HPP

#include "backend/canvas_edit.hpp"
#include "backend/capabilities.hpp"
#include "backend/view_types.hpp"
#include "topology/topology_layout.hpp"
#include "widgets/notes.hpp"
#include "widgets/port_menu.hpp"

#include <QGraphicsView>
#include <QHash>
#include <QLineF>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QGraphicsPathItem;
class QGraphicsRectItem;
class QLabel;
class QMenu;
class QRubberBand;
class QToolButton;

namespace vss::gui::rendering
{
class GpuCanvas;
}

namespace vss::gui::topology
{

class LinkItem;
class NodeItem;

/// The topology dock: a scene of port nodes and link lines.
///
/// The view owns presentation and input only.  It is fed a snapshot with
/// `set_topology`, reports intent with signals, and keeps node positions for
/// the session (the JSON schema has no positions).  A node moves with the
/// mouse or the arrow keys and the links that end on it follow; nodes never
/// overlap and a link never runs under a node it is not attached to.
/// Keyboard operation is complete: Tab reaches nodes then links, Enter
/// inspects, Delete removes, the arrow keys move the node, Shift+F10 opens the
/// context menu and Ctrl+0 fits.
///
/// Resizing: while the user has not zoomed, scrolled or arranged anything the
/// view keeps fitting the content; after that a resize keeps the zoom and the
/// point at the centre, and brings the content back if it was left outside.
class TopologyView final : public QGraphicsView
{
    Q_OBJECT

public:
    explicit TopologyView(QWidget* parent = nullptr);
    ~TopologyView() override;

    /// What this build can do; a missing capability hides its control.
    void set_capabilities(const capabilities& caps);

    /// How links are drawn, curved or at right angles (Settings, Appearance).
    /// The links are redrawn at once; the nodes stay where they are.
    void set_link_style(link_style style);
    link_style link_style_mode() const noexcept { return link_style_; }

    /// Advanced mode: the canvas is an editor.  Tiles can be dropped on it,
    /// nodes show wiring handles, several items can be selected, and the
    /// menus offer Settings, Duplicate and Delete.  See
    /// `docs/gui-design/docs/screens/07-canvas-editor.md`.
    void set_editing(bool on);
    bool is_editing() const noexcept { return editing_; }

    /// Every selected port and link, the focused one included.
    QVector<vss::endpoint_id> selected_ports() const;
    QVector<vss::link_id> selected_links() const;
    void select_all();
    /// Selects exactly these items (the rubber band and the tests).
    void set_multi_selection(const QVector<vss::endpoint_id>& ports,
                             const QVector<vss::link_id>& links);
    /// Adds or removes one item, as Ctrl+click does.
    void toggle_selected(vss::endpoint_id port, vss::link_id link);

    /// The nodes of the next topology that are new go here, in scene units,
    /// instead of into the automatic layout: a tile dropped at a point.
    void place_next_at(const QPointF& scene_point);

    /// Starts a wire from a port, as dragging from its handle or Ctrl+L does.
    void begin_wire(vss::endpoint_id from);
    void cancel_wire();
    bool is_wiring() const noexcept { return wire_from_ != 0; }
    vss::endpoint_id wire_source() const noexcept { return wire_from_; }
    /// True when a link of this kind between the two would close a loop,
    /// which the engine refuses: bytes could come back to where they
    /// started.  Follows the engine's rule (`hub::would_create_cycle`).
    bool would_loop(vss::endpoint_id from, vss::endpoint_id to,
                    wire_kind kind = wire_kind::two_way) const;
    /// Ends the wire on a port with the chosen kind of link.
    void finish_wire(vss::endpoint_id to, wire_kind kind);
    /// Ends the wire on empty canvas: a new port there, linked as chosen.
    void finish_wire_new(slot_kind kind, wire_kind wire,
                         const QPointF& scene_point);
    /// The labels of the link chooser for a wire from one port to another.
    QStringList wire_menu_labels(vss::endpoint_id from,
                                 vss::endpoint_id to) const;

    /// Replaces the graph.  Until the user moves a node the whole layout is
    /// recomputed; after that every node keeps its position for the rest of
    /// the session and a new node goes into free space next to the node it
    /// connects to.  Positions never enter the document.
    void set_topology(const std::vector<port_view>& ports,
                      const std::vector<link_view>& links);

    /// How a port's context menu reads its Send, Monitor and fixture actions.
    /// Without one the menu offers Send bytes and Open in monitor only.
    void set_menu_state_provider(
        std::function<port_menu_state(vss::endpoint_id)> provider)
    {
        menu_state_ = std::move(provider);
    }

    /// Mirrors the shared selection without emitting the selection signals.
    void set_selection(vss::endpoint_id port, vss::link_id link);
    vss::endpoint_id selected_port() const noexcept { return selected_port_; }
    vss::link_id selected_link() const noexcept { return selected_link_; }

    int node_count() const;
    int link_count() const;
    bool is_compact() const noexcept { return compact_; }
    qreal zoom() const;
    /// True while the view re-fits on its own (no manual zoom, scroll or move).
    bool is_auto_fit() const noexcept { return auto_fit_; }
    /// True once the user has moved a node, so the layout is no longer
    /// automatic.
    bool is_arranged() const noexcept { return user_arranged_; }
    /// Moves a node as a drag or a keyboard nudge would, and settles it clear
    /// of the other nodes.  Used by the tests and by the keyboard path.
    void move_node(vss::endpoint_id id, const QPointF& top_left);
    /// The polyline segments of a link, for the tests.
    QVector<QLineF> link_segments(vss::link_id id) const;
    /// What is drawn for a link, for the tests.
    QPainterPath link_stroke(vss::link_id id) const;

    /// The generated accessible description.
    QString summary() const { return summary_; }
    QRectF node_rect(vss::endpoint_id id) const;
    QSizeF node_size(vss::endpoint_id id) const;
    QPointF link_anchor(vss::link_id id) const;
    QRectF link_bounds(vss::link_id id) const;
    QString link_description(vss::link_id id) const;
    QStringList link_descriptions() const;
    bool node_is_unlinked(vss::endpoint_id id) const;
    bool node_is_device(vss::endpoint_id id) const;
    QStringList node_badges(vss::endpoint_id id) const;

    vss::endpoint_id focused_port() const noexcept { return focused_port_; }
    vss::link_id focused_link() const noexcept { return focused_link_; }

    /// The labels of the context menu, in order.  The product menu is built
    /// from the same list; the tests read it without opening a popup.
    QStringList node_menu_labels(vss::endpoint_id id) const;
    QStringList link_menu_labels(vss::link_id id) const;
    /// The labels of the menu on empty canvas: Tidy layout and Fit, and in
    /// Advanced mode the Add items and Select all before them.
    QStringList canvas_menu_labels() const;
    /// The labels of the menu for a selection of several ports.
    QStringList selection_menu_labels() const;

    /// Fits all items into view with a margin and returns to automatic fitting.
    void fit_content();
    /// Runs the automatic layout again, dropping every moved position, then
    /// fits.
    void tidy_layout();
    void zoom_in();
    void zoom_out();
    /// Back to 100 percent about the centre of the view.
    void reset_zoom();
    /// Zooms by `factor` about a view point (the wheel and pinch use the
    /// pointer; the buttons and keys use the centre), inside the limits.
    void zoom_at(qreal factor, const QPoint& view_point);
    /// The zoom limits.
    static qreal min_zoom() noexcept;
    static qreal max_zoom() noexcept;

    /// The "That link would create a loop" pattern: highlights the cycle and
    /// shows the danger note with its "Show the loop" action.  Returns false
    /// when the current links are acyclic (nothing to show).
    bool show_loop_note();
    void dismiss_loop_note();
    QString loop_note_text() const;
    int loop_link_count() const;
    QVector<vss::endpoint_id> loop_ports() const { return loop_nodes_; }

    /// What the last frame cost, in milliseconds: the CPU's drawing time (for
    /// the GPU canvas, the time to record the frame) and, with the GPU, the
    /// GPU's time. For the render bench and tests.
    [[nodiscard]] double last_paint_ms() const noexcept;
    [[nodiscard]] double last_gpu_ms() const noexcept;
    [[nodiscard]] bool gpu_active() const noexcept { return gpu_ != nullptr; }

Q_SIGNALS:
    void port_selected(vss::endpoint_id id);
    void link_selected(vss::link_id id);
    void inspect_requested(vss::endpoint_id id);
    void copy_name_requested(vss::endpoint_id id);
    void connect_requested(vss::endpoint_id id);
    void remove_port_requested(vss::endpoint_id id);
    void remove_link_requested(vss::link_id id);
    /// Emitted only when the build can rename (Capabilities::rename_port).
    void rename_requested(vss::endpoint_id id);
    /// Send bytes, repeat, Open in monitor, and a fixture's generator and
    /// echo, chosen from a node's menu.
    void port_action_requested(vss::endpoint_id id, port_menu_action action);
    void status_message(const QString& text);

    // -- Advanced canvas -------------------------------------------------
    /// A toolbox tile dropped on the canvas, or an Add item of the canvas
    /// menu.  `at_point` is false when the tile should go to a free spot.
    void add_requested(vss::gui::canvas_item item, const QPointF& scene_point,
                       bool at_point);
    void wire_requested(vss::endpoint_id from, vss::endpoint_id to,
                        vss::gui::wire_kind kind);
    void wire_new_requested(vss::endpoint_id from, vss::gui::slot_kind kind,
                            vss::gui::wire_kind wire,
                            const QPointF& scene_point);
    void settings_requested(const QVector<vss::endpoint_id>& ports);
    void link_settings_requested(vss::link_id id);
    void reverse_requested(vss::link_id id);
    void duplicate_requested(const QVector<vss::endpoint_id>& ports);
    void delete_requested(const QVector<vss::endpoint_id>& ports,
                          const QVector<vss::link_id>& links);
    /// Link selected: a two-way link, or a split, join, bus or switch with
    /// `hub` as the one end.
    void link_selected_requested(const QVector<vss::endpoint_id>& ports,
                                 vss::endpoint_id hub,
                                 vss::gui::canvas_item shape);
    void selection_changed(int count);

protected:
    bool event(QEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// The GPU canvas covers the viewport while the canvas is drawn in
    /// hardware; `disable_gpu` hands drawing back to the QPainter path.
    void enable_gpu();
    void disable_gpu();
    void apply_gpu_style();
    void rebuild_items();
    void apply_positions(bool refresh_scene_rect = true);
    void update_scene_rect();
    void update_overlays();
    void update_zoom_controls();
    void update_accessibility();
    void refresh_focus_visuals();

    bool select_port(vss::endpoint_id id);
    bool select_link(vss::link_id id);
    void focus_at(qsizetype index, bool select);
    void clear_focus();
    void nudge_focused(qreal dx, qreal dy);
    void settle_node(vss::endpoint_id id);
    void begin_drag();
    void place_nodes();
    void keep_content_in_view();
    void remember_centre();
    void mark_manual_view();
    void open_context_menu(const QPoint& viewport_position);
    void open_focused_menu();
    void centre_on_links(const QVector<vss::link_id>& ids);
    qsizetype focus_index_of_port(vss::endpoint_id id) const;
    qsizetype focus_index_of_link(vss::link_id id) const;

    bool is_selected_port(vss::endpoint_id id) const;
    bool is_selected_link(vss::link_id id) const;
    void clear_multi();
    void emit_selection_count();
    void populate_node_menu(QMenu* menu, vss::endpoint_id id);
    void populate_selection_menu(QMenu* menu, vss::endpoint_id id);
    void populate_link_menu(QMenu* menu, vss::link_id id);
    void populate_canvas_menu(QMenu* menu, const QPointF& scene_point,
                              bool at_point);
    void add_link_selected_menu(QMenu* menu, vss::endpoint_id hub);
    void open_wire_menu(vss::endpoint_id to, const QPoint& viewport_position);
    void open_wire_new_menu(const QPoint& viewport_position);
    std::optional<std::pair<vss::endpoint_id, bool>>
    handle_at(const QPointF& scene_point) const;
    vss::endpoint_id node_at(const QPointF& scene_point) const;
    /// True when bytes can travel from one port to the other over links.
    bool reaches(vss::endpoint_id from, vss::endpoint_id to) const;
    void update_wire(const QPointF& scene_point);
    void update_wire_targets(vss::endpoint_id hovered);
    void move_selection_by(const QPointF& delta, vss::endpoint_id except);

    QGraphicsScene* scene_ = nullptr;
    /// Set while the canvas is drawn by the GPU.
    rendering::GpuCanvas* gpu_ = nullptr;
    /// CPU time of the last QPainter frame (software drawing).
    double software_paint_ms_ = 0.0;
    std::vector<port_view> ports_;
    std::vector<link_view> links_;
    QHash<vss::endpoint_id, QString> names_;
    QVector<topo_link_input> link_inputs_;
    link_style link_style_ = link_style::curved;
    QVector<vss::endpoint_id> node_ids_;

    QHash<vss::endpoint_id, NodeItem*> node_items_;
    QHash<vss::link_id, LinkItem*> link_items_;
    QVector<vss::endpoint_id> node_order_;
    QVector<vss::link_id> link_order_;
    topology_layout layout_;
    /// The top-left of every node, in scene units.  The automatic layout fills
    /// it until the user moves a node.
    QHash<vss::endpoint_id, QPointF> positions_;
    QVector<vss::endpoint_id> new_nodes_;
    QHash<vss::endpoint_id, QSizeF> node_sizes_;

    std::function<port_menu_state(vss::endpoint_id)> menu_state_;
    QLabel* empty_ = nullptr;
    QLabel* hint_ = nullptr;
    NoteBanner* loop_note_ = nullptr;
    QWidget* zoom_bar_ = nullptr;
    QToolButton* zoom_out_button_ = nullptr;
    QToolButton* zoom_reset_button_ = nullptr;
    QToolButton* zoom_in_button_ = nullptr;

    QString summary_;
    capabilities caps_;

    vss::endpoint_id selected_port_ = 0;
    vss::link_id selected_link_ = 0;
    vss::endpoint_id focused_port_ = 0;
    vss::link_id focused_link_ = 0;
    qsizetype focus_index_ = -1;
    bool show_focus_ = false;

    bool compact_ = false;
    bool auto_fit_ = true;
    bool user_arranged_ = false;
    bool dragging_ = false;
    bool fitting_ = false;
    bool internal_scroll_ = false;
    bool resizing_ = false;    /// The scene point at the centre of the view, as the user last left it.
    QPointF view_centre_;

    QVector<vss::link_id> loop_links_;
    QVector<vss::endpoint_id> loop_nodes_;

    // -- Advanced canvas -------------------------------------------------
    bool editing_ = false;
    QSet<vss::endpoint_id> multi_ports_;
    QSet<vss::link_id> multi_links_;
    std::optional<QPointF> pending_drop_;
    vss::endpoint_id wire_from_ = 0;
    QPointF wire_start_;
    QGraphicsPathItem* wire_item_ = nullptr;
    QGraphicsRectItem* ghost_ = nullptr;
    // The rubber band is a child of the viewport; owning it here releases it
    // with the view and before the viewport goes.
    std::unique_ptr<QRubberBand> band_;
    QPoint band_origin_;
    bool band_active_ = false;
    bool band_adds_ = false;
    bool panning_ = false;
    bool space_down_ = false;
    QPoint pan_last_;
};

} // namespace vss::gui::topology

#endif // VSS_GUI_TOPOLOGY_TOPOLOGY_VIEW_HPP
