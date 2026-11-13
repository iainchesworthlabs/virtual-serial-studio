//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "rendering/gpu_canvas.hpp"

#include <vss/core/profile.hpp>

#include <QCanvasImagePattern>
#include <QCanvasPainter>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QSet>
#include <QStyleOptionGraphicsItem>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>

namespace vss::gui::rendering
{
namespace
{

/// The largest texture made for one item, per side. A longer link is drawn
/// at a lower resolution rather than not at all.
constexpr qreal kMaxTexture = 4096.0;

/// A texture is reused, scaled, while it is within this fraction of the size
/// wanted and the zoom is still changing.
constexpr qreal kScaleTolerance = 0.3;

/// How long the zoom must be still before textures are made sharp again.
constexpr int kSettleMs = 120;

QPointF snapped(QPointF position, qreal pixel_ratio)
{
    return {std::round(position.x() * pixel_ratio) / pixel_ratio,
            std::round(position.y() * pixel_ratio) / pixel_ratio};
}

} // namespace

GpuCanvas::GpuCanvas(QGraphicsView& view)
    : QCanvasPainterWidget(view.viewport()), view_(view)
{
    setObjectName(QStringLiteral("gpuCanvas"));
    // Everything the person does lands on the viewport underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(view.viewport()->rect());
    view.viewport()->installEventFilter(this);
    if (view.scene() != nullptr)
    {
        connect(view.scene(), &QGraphicsScene::changed, this,
                &GpuCanvas::scene_changed);
    }
    settle_.setSingleShot(true);
    settle_.setInterval(kSettleMs);
    connect(&settle_, &QTimer::timeout, this,
            [this]
            {
                sharpen_ = true;
                update();
            });
    connect(this, &QRhiWidget::renderFailed, this,
            [this]
            {
                Q_EMIT device_lost(QCoreApplication::translate(
                    "Rendering", "The graphics device stopped working."));
            });
}

GpuCanvas::~GpuCanvas() = default;

void GpuCanvas::set_style(const style& value)
{
    style_ = value;
    setFillColor(value.ground);
    dot_tile_stale_ = true;
    update();
}

void GpuCanvas::invalidate_all()
{
    for (auto& item : cache_)
    {
        if (!item.image.isNull())
            pending_removal_.append(item.image);
    }
    cache_.clear();
    update();
}

void GpuCanvas::scene_changed(const QList<QRectF>& regions)
{
    VSS_PROFILE_ZONE("gui.render.sync");
    for (auto it = cache_.begin(); it != cache_.end();)
    {
        bool hit = false;
        for (const QRectF& region : regions)
        {
            if (region.intersects(it->scene))
            {
                hit = true;
                break;
            }
        }
        if (hit)
        {
            if (!it->image.isNull())
                pending_removal_.append(it->image);
            it = cache_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    update();
}

void GpuCanvas::initializeResources(QCanvasPainter*)
{
    if (reported_)
        return;
    reported_ = true;
    gpu_report report;
    if (auto* device = rhi(); device != nullptr)
    {
        report.device_created = true;
        report.backend = QString::fromLatin1(device->backendName());
        const QRhiDriverInfo info = device->driverInfo();
        report.device = QString::fromUtf8(info.deviceName);
        report.cpu_device = info.deviceType == QRhiDriverInfo::CpuDevice;
    }
    else
    {
        report.error = QCoreApplication::translate(
            "Rendering", "no graphics device was created");
    }
    QMetaObject::invokeMethod(
        this, [this, report] { Q_EMIT device_ready(report); },
        Qt::QueuedConnection);
}

void GpuCanvas::graphicsResourcesInvalidated()
{
    // The textures are gone with the device. Forget them; the next frame
    // draws every item again.
    cache_.clear();
    pending_removal_.clear();
    dot_tile_ = {};
    dot_tile_stale_ = true;
}

void GpuCanvas::render(QRhiCommandBuffer* cb)
{
    VSS_PROFILE_ZONE("gui.render.frame");
    QCanvasPainterWidget::render(cb);
    if (rhi() != nullptr && rhi()->isFeatureSupported(QRhi::Timestamps))
    {
        const double seconds = cb->lastCompletedGpuTime();
        if (seconds > 0.0)
            gpu_ms_ = seconds * 1e3;
        if (profiler_.recording())
            profiler_.frame_done(seconds);
    }
}

bool GpuCanvas::eventFilter(QObject* watched, QEvent* event)
{
    // Stay the size of the viewport.
    if (watched == view_.viewport() && event->type() == QEvent::Resize)
        setGeometry(view_.viewport()->rect());
    return QCanvasPainterWidget::eventFilter(watched, event);
}

void GpuCanvas::release_pending(QCanvasPainter& painter)
{
    for (const auto& image : pending_removal_)
        painter.removeImage(image);
    pending_removal_.clear();
}

void GpuCanvas::paint(QCanvasPainter* painter)
{
    VSS_PROFILE_ZONE("gui.render.draw");
    ++frame_;
    QElapsedTimer timer;
    timer.start();
    release_pending(*painter);
    const QTransform to_view = view_.viewportTransform();
    const qreal pixel_ratio = devicePixelRatioF();
    draw_dots(*painter, to_view, pixel_ratio);
    draw_items(*painter, to_view, pixel_ratio);
    VSS_PROFILE_PLOT("gui.render.item_textures", cache_.size());
    VSS_PROFILE_PLOT("gui.render.texture_bytes",
                     painter->activeImageMemoryUsage());
    paint_ms_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

void GpuCanvas::draw_dots(QCanvasPainter& painter, const QTransform& to_view,
                          qreal pixel_ratio)
{
    VSS_PROFILE_ZONE("gui.render.dots");
    const qreal cell = style_.grid * to_view.m11();
    if (cell < style_.min_spacing)
        return;
    // One cell of the grid as a texture with its dot in the middle, repeated
    // over the whole canvas by the GPU.
    const int size = std::max(2, static_cast<int>(std::lround(cell * pixel_ratio)));
    if (dot_tile_stale_ || dot_tile_.isNull() || size != dot_tile_size_ ||
        style_.dots != dot_tile_colour_)
    {
        if (!dot_tile_.isNull())
            painter.removeImage(dot_tile_);
        QImage tile(size, size, QImage::Format_RGBA8888_Premultiplied);
        tile.fill(Qt::transparent);
        {
            QPainter dots(&tile);
            dots.setRenderHint(QPainter::Antialiasing, true);
            dots.setPen(Qt::NoPen);
            dots.setBrush(style_.dots);
            const qreal radius = style_.dot * pixel_ratio / 2.0;
            dots.drawEllipse(QPointF(size / 2.0, size / 2.0), radius, radius);
        }
        dot_tile_ = painter.addImage(
            tile, QCanvasPainter::ImageFlag::Repeat |
                      QCanvasPainter::ImageFlag::Premultiplied);
        dot_tile_size_ = size;
        dot_tile_colour_ = style_.dots;
        dot_tile_stale_ = false;
    }
    const QPointF origin = to_view.map(QPointF(0.0, 0.0));
    painter.setFillStyle(QCanvasImagePattern(
        dot_tile_, origin.x() - cell / 2.0, origin.y() - cell / 2.0, cell,
        cell));
    painter.fillRect(0.0, 0.0, width(), height());
}

QImage GpuCanvas::rasterise(QGraphicsItem& item, qreal scale) const
{
    const QRectF local = item.boundingRect();
    QImage image(std::max(1, static_cast<int>(std::ceil(local.width() * scale))),
                 std::max(1, static_cast<int>(std::ceil(local.height() * scale))),
                 QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.scale(scale, scale);
    painter.translate(-local.topLeft());
    QStyleOptionGraphicsItem option;
    option.exposedRect = local;
    option.rect = local.toAlignedRect();
    item.paint(&painter, &option, nullptr);
    return image;
}

void GpuCanvas::draw_items(QCanvasPainter& painter, const QTransform& to_view,
                           qreal pixel_ratio)
{
    VSS_PROFILE_ZONE("gui.render.items");
    auto* scene = view_.scene();
    if (scene == nullptr)
        return;

    // Items that are gone take their textures with them.
    {
        VSS_PROFILE_ZONE("gui.render.sync");
        const QList<QGraphicsItem*> all = scene->items();
        const QSet<QGraphicsItem*> alive(all.begin(), all.end());
        for (auto it = cache_.begin(); it != cache_.end();)
        {
            if (!alive.contains(it.key()))
            {
                if (!it->image.isNull())
                    painter.removeImage(it->image);
                it = cache_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    const QRectF visible =
        view_.mapToScene(view_.viewport()->rect()).boundingRect();
    const QList<QGraphicsItem*> items = scene->items(
        visible, Qt::IntersectsItemBoundingRect, Qt::AscendingOrder);
    const qreal zoom = to_view.m11();
    for (QGraphicsItem* item : items)
    {
        if (!item->isVisible())
            continue;
        const QRectF local = item->boundingRect();
        if (local.isEmpty())
            continue;
        const qreal opacity = item->effectiveOpacity();
        if (opacity <= 0.0)
            continue;

        qreal scale = zoom * pixel_ratio;
        scale = std::min({scale, kMaxTexture / local.width(),
                          kMaxTexture / local.height()});
        auto found = cache_.find(item);
        const bool have = found != cache_.end() && !found->image.isNull() &&
                          found->local == local;
        const qreal ratio = have ? scale / found->scale : 0.0;
        const bool exact = have && std::abs(ratio - 1.0) < 1e-6;
        const bool close = have && !sharpen_ &&
                           std::abs(ratio - 1.0) < kScaleTolerance;
        if (have && !exact && close)
            settle_.start();
        if (!have || (!exact && !close))
        {
            VSS_PROFILE_ZONE("gui.render.upload");
            if (found != cache_.end() && !found->image.isNull())
                painter.removeImage(found->image);
            entry made;
            made.local = local;
            made.scene = item->mapRectToScene(local);
            made.scale = scale;
            made.image = painter.addImage(
                rasterise(*item, scale),
                QCanvasPainter::ImageFlag::Premultiplied);
            found = cache_.insert(item, made);
        }
        found->used = frame_;

        const QTransform full = item->sceneTransform() * to_view;
        painter.save();
        painter.setGlobalAlpha(opacity);
        if (full.type() <= QTransform::TxScale)
        {
            // At the size the texture was made, placed on a whole device
            // pixel, so it is not resampled. While the zoom is changing it
            // is stretched to the size wanted instead.
            const bool made_for_now =
                std::abs(scale / found->scale - 1.0) < 1e-6;
            const QPointF top_left = made_for_now
                                         ? snapped(full.map(local.topLeft()),
                                                   pixel_ratio)
                                         : full.map(local.topLeft());
            const qreal width = made_for_now
                                    ? found->image.width() / pixel_ratio
                                    : local.width() * full.m11();
            const qreal height = made_for_now
                                     ? found->image.height() / pixel_ratio
                                     : local.height() * full.m22();
            painter.drawImage(found->image, top_left.x(), top_left.y(), width,
                              height);
        }
        else
        {
            painter.setTransform(full);
            painter.drawImage(found->image, local.x(), local.y(),
                              local.width(), local.height());
        }
        painter.restore();
    }
    sharpen_ = false;
}

} // namespace vss::gui::rendering
