//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_THEME_ICON_PROVIDER_HPP
#define VSS_GUI_THEME_ICON_PROVIDER_HPP

#include "theme/theme.hpp"

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QString>

#include <optional>

namespace vss::gui
{

/// Loads `:/icons/<name>.svg`, replaces `currentColor`, and caches by name and
/// colour or role.  The returned icon renders on demand at any size and device
/// pixel ratio, follows the theme when it was asked for by role, and draws its
/// disabled mode in the disabled text colour.  The cache is flushed on a theme
/// change.
class IconProvider final : public QObject
{
public:
    static IconProvider& instance();

    QIcon icon(const QString& name, colour_role role, int logical_size = 16);
    QIcon icon(const QString& name, const QColor& colour,
               int logical_size = 16);

    /// The application icon (`:/app/vss_gui.svg`, made from
    /// apps/vss_gui/assets/icon/vss_gui.svg). It is a picture in its own
    /// colours, so it does not follow the theme, and it renders at any size
    /// and device pixel ratio like the other icons.
    QIcon app_icon();

    /// True for the icons that show a direction of travel (an arrow, a fan out
    /// or a fan in, a play triangle): in a right-to-left layout they are drawn
    /// flipped, so they point the way the reading goes. Icons that show an
    /// object (a port, a folder, a link) and vertical ones (export, chevron)
    /// are never flipped.
    [[nodiscard]] static bool mirrors_in_right_to_left(const QString& name);

    void flush();
    int cache_size() const noexcept { return static_cast<int>(cache_.size()); }

private:
    explicit IconProvider(QObject* parent = nullptr);

    QIcon render(const QString& name, std::optional<colour_role> role,
                 const QColor& colour);
    QIcon load(const QString& path, std::optional<colour_role> role,
               const QColor& colour);

    QHash<QString, QIcon> cache_;
    QIcon app_icon_;
};

} // namespace vss::gui

#endif // VSS_GUI_THEME_ICON_PROVIDER_HPP
