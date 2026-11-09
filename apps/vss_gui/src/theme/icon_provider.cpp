//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "theme/icon_provider.hpp"

#include <vss/core/profile.hpp>

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace vss::gui
{
namespace
{

/// The SVG source of one icon and the renderers made from it, one per colour
/// that has been asked for.  Shared by every copy of the icon.
struct svg_source
{
    QByteArray svg;
    std::optional<colour_role> role;
    QColor fixed;
    mutable QHash<QRgb, std::shared_ptr<QSvgRenderer>> renderers;

    QColor colour(bool disabled) const
    {
        if (disabled)
            return Theme::instance().colour(colour_role::disabledText);
        return role ? Theme::instance().colour(*role) : fixed;
    }

    QSvgRenderer& renderer(bool disabled) const
    {
        const QColor wanted = colour(disabled);
        const auto found = renderers.constFind(wanted.rgba());
        if (found != renderers.constEnd())
            return *found.value();
        QByteArray text = svg;
        text.replace("currentColor", wanted.name(QColor::HexRgb).toUtf8());
        auto renderer = std::make_shared<QSvgRenderer>(text);
        QSvgRenderer& reference = *renderer;
        renderers.insert(wanted.rgba(), std::move(renderer));
        return reference;
    }
};

/// Renders one SVG on demand at whatever size, device pixel ratio and mode
/// the caller asks for, in the colour the theme has at that moment.  A fixed
/// pixmap made when the icon is created is the wrong size as soon as the
/// window moves to a screen with another scale, the style asks for another
/// icon size or the user raises the text size, and the wrong colour after a
/// theme change; a pixmap that is scaled up looks soft.  The disabled mode is
/// drawn in the disabled text colour so a disabled action reads as disabled
/// in both themes instead of through the style's grey-out filter.
class svg_icon_engine final : public QIconEngine
{
public:
    explicit svg_icon_engine(std::shared_ptr<svg_source> source)
        : source_(std::move(source))
    {
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State) override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // Keep the icon square and centred in a rectangle of any shape.
        const int side = std::min(rect.width(), rect.height());
        const QRectF target(rect.x() + (rect.width() - side) / 2.0,
                            rect.y() + (rect.height() - side) / 2.0, side,
                            side);
        source_->renderer(mode == QIcon::Disabled).render(painter, target);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode,
                   QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override
    {
        const int side = std::max(1, std::min(size.width(), size.height()));
        const int pixels =
            std::max(1, static_cast<int>(std::lround(side * scale)));
        QPixmap result(pixels, pixels);
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing);
        // An explicit rectangle in logical units: without it the renderer
        // draws at device pixels and, above 100% scale, fills only a corner.
        source_->renderer(mode == QIcon::Disabled)
            .render(&painter, QRectF(0.0, 0.0, side, side));
        painter.end();
        return result;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override
    {
        const int side = std::min(size.width(), size.height());
        return {side, side};
    }

    QIconEngine* clone() const override { return new svg_icon_engine(source_); }

private:
    std::shared_ptr<svg_source> source_;
};

} // namespace

IconProvider& IconProvider::instance()
{
    static IconProvider provider;
    return provider;
}

IconProvider::IconProvider(QObject* parent) : QObject(parent)
{
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { flush(); });
}

QIcon IconProvider::icon(const QString& name, colour_role role,
                         int logical_size)
{
    // The engine draws at any size, so the size is only a hint for callers
    // that still pass one.
    static_cast<void>(logical_size);
    const QString key =
        QStringLiteral("%1|role%2").arg(name).arg(static_cast<int>(role));
    const auto cached = cache_.constFind(key);
    if (cached != cache_.constEnd())
        return cached.value();
    const QIcon icon = render(name, role, {});
    if (!icon.isNull())
        cache_.insert(key, icon);
    return icon;
}

QIcon IconProvider::icon(const QString& name, const QColor& colour,
                         int logical_size)
{
    static_cast<void>(logical_size);
    const QString key = QStringLiteral("%1|%2").arg(name, colour.name());
    const auto cached = cache_.constFind(key);
    if (cached != cache_.constEnd())
        return cached.value();
    const QIcon icon = render(name, std::nullopt, colour);
    if (!icon.isNull())
        cache_.insert(key, icon);
    return icon;
}

QIcon IconProvider::app_icon()
{
    if (app_icon_.isNull())
        app_icon_ = load(QStringLiteral(":/app/vss_gui.svg"), std::nullopt, {});
    return app_icon_;
}

void IconProvider::flush()
{
    cache_.clear();
}

QIcon IconProvider::render(const QString& name, std::optional<colour_role> role,
                           const QColor& colour)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone, "gui.theme.icon.render");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, name.toStdString());
    return load(QStringLiteral(":/icons/%1.svg").arg(name), role, colour);
}

QIcon IconProvider::load(const QString& path, std::optional<colour_role> role,
                         const QColor& colour)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning("IconProvider: missing icon \"%s\"",
                 path.toUtf8().constData());
        return {};
    }

    auto source = std::make_shared<svg_source>();
    source->svg = file.readAll();
    source->role = role;
    source->fixed = colour;
    if (!source->renderer(false).isValid())
    {
        qWarning("IconProvider: invalid SVG \"%s\"", path.toUtf8().constData());
        return {};
    }
    return QIcon(new svg_icon_engine(std::move(source)));
}

} // namespace vss::gui
