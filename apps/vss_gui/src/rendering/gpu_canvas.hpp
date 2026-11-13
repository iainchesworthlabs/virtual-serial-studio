//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "rendering/gpu_profiler.hpp"
#include "rendering/render_mode.hpp"

#include <QCanvasImage>
#include <QCanvasPainterWidget>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QTimer>

class QGraphicsItem;
class QGraphicsView;

namespace vss::gui::rendering
{

/// The GPU side of a QGraphicsView.
///
/// It is a child of the view's viewport, covers it, and takes no mouse or
/// keyboard events, so the view keeps hit-testing, focus, drag and drop and
/// accessibility exactly as before. It draws what the view would have drawn:
///
///   the ground and the dot grid, on the GPU, every frame;
///   each scene item, as a texture that the item's own `paint()` filled once.
///
/// The textures are drawn at the view's zoom and the screen's pixel ratio,
/// and made again only when the item changes (the scene reports the changed
/// regions), the zoom changes or the theme changes. Moving, panning and
/// resizing the view cost the GPU a handful of textured quads and no CPU
/// drawing. What stays on the CPU is the drawing of an item's content (text,
/// icons, shapes) into its texture.
class GpuCanvas : public QCanvasPainterWidget
{
    Q_OBJECT

public:
    struct style
    {
        QColor ground;
        QColor dots;
        /// Distance between dots, in scene units.
        qreal grid = 16.0;
        /// No dots when they would be closer than this on the screen.
        qreal min_spacing = 6.0;
        /// Diameter of a dot in logical pixels.
        qreal dot = 1.5;
    };

    explicit GpuCanvas(QGraphicsView& view);
    ~GpuCanvas() override;

    void set_style(const style& value);

    /// Keeps the canvas the size of the view's viewport.
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// Drops every item texture, so each item is drawn again (theme change).
    void invalidate_all();

    /// How many item textures exist, for tests and the profiler plot.
    [[nodiscard]] int cached_items() const noexcept
    {
        return static_cast<int>(cache_.size());
    }

    /// CPU time of the last frame's drawing calls, in milliseconds, and the
    /// GPU time of the last completed frame (0 until the device reports one).
    [[nodiscard]] double last_paint_ms() const noexcept { return paint_ms_; }
    [[nodiscard]] double last_gpu_ms() const noexcept { return gpu_ms_; }

Q_SIGNALS:
    /// The graphics device exists (or could not be made). Queued.
    void device_ready(const vss::gui::rendering::gpu_report& report);
    /// The device was lost while running. Queued.
    void device_lost(const QString& reason);

protected:
    void initializeResources(QCanvasPainter* painter) override;
    void paint(QCanvasPainter* painter) override;
    void graphicsResourcesInvalidated() override;
    void render(QRhiCommandBuffer* cb) override;

private:
    struct entry
    {
        QCanvasImage image;
        QRectF local;
        QRectF scene;
        qreal scale = 0.0;
        quint64 used = 0;
    };

    void scene_changed(const QList<QRectF>& regions);
    void draw_dots(QCanvasPainter& painter, const QTransform& to_view,
                   qreal pixel_ratio);
    void draw_items(QCanvasPainter& painter, const QTransform& to_view,
                    qreal pixel_ratio);
    QImage rasterise(QGraphicsItem& item, qreal scale) const;
    void release_pending(QCanvasPainter& painter);

    QGraphicsView& view_;
    style style_;
    QHash<QGraphicsItem*, entry> cache_;
    QList<QCanvasImage> pending_removal_;
    QCanvasImage dot_tile_;
    int dot_tile_size_ = 0;
    QColor dot_tile_colour_;
    bool dot_tile_stale_ = true;
    quint64 frame_ = 0;
    /// While the zoom is changing (a window resize that refits the canvas, a
    /// wheel turn) the textures already made are drawn scaled, a little soft,
    /// instead of being made again every frame. This timer fires when the zoom
    /// has been still for a moment, and the next frame makes them sharp.
    QTimer settle_;
    bool sharpen_ = true;
    gpu_profiler profiler_;
    bool reported_ = false;
    double paint_ms_ = 0.0;
    double gpu_ms_ = 0.0;
};

} // namespace vss::gui::rendering
