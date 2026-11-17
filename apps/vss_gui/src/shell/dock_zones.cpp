//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/dock_zones.hpp"

namespace vss::gui
{

bool is_window_zone(drop_zone zone) noexcept
{
    return zone == drop_zone::window_left || zone == drop_zone::window_right ||
           zone == drop_zone::window_top || zone == drop_zone::window_bottom;
}

std::vector<drop_guide> pane_guides(const QRect& pane, int size, int gap)
{
    std::vector<drop_guide> guides;
    if (!pane.isValid() || size <= 0)
        return guides;
    const QPoint centre = pane.center();
    const QRect middle(centre.x() - size / 2, centre.y() - size / 2, size,
                       size);
    guides.push_back({drop_zone::centre, middle});
    const int step = size + gap;
    const bool wide = pane.width() >= 3 * size + 2 * gap + 2 * gap;
    const bool tall = pane.height() >= 3 * size + 2 * gap + 2 * gap;
    if (wide)
    {
        guides.push_back({drop_zone::left, middle.translated(-step, 0)});
        guides.push_back({drop_zone::right, middle.translated(step, 0)});
    }
    if (tall)
    {
        guides.push_back({drop_zone::top, middle.translated(0, -step)});
        guides.push_back({drop_zone::bottom, middle.translated(0, step)});
    }
    return guides;
}

std::vector<drop_guide> window_guides(const QRect& area, int size, int margin)
{
    std::vector<drop_guide> guides;
    if (!area.isValid() || size <= 0)
        return guides;
    const QPoint centre = area.center();
    guides.push_back({drop_zone::window_left,
                      QRect(area.left() + margin, centre.y() - size / 2, size,
                            size)});
    guides.push_back({drop_zone::window_right,
                      QRect(area.right() + 1 - margin - size,
                            centre.y() - size / 2, size, size)});
    guides.push_back({drop_zone::window_top,
                      QRect(centre.x() - size / 2, area.top() + margin, size,
                            size)});
    guides.push_back({drop_zone::window_bottom,
                      QRect(centre.x() - size / 2,
                            area.bottom() + 1 - margin - size, size, size)});
    return guides;
}

drop_zone zone_at(const QPoint& cursor, const QRect& pane, const QRect& area,
                  int size, int gap, int margin)
{
    for (const drop_guide& guide : window_guides(area, size, margin))
    {
        if (guide.rect.contains(cursor))
            return guide.zone;
    }
    for (const drop_guide& guide : pane_guides(pane, size, gap))
    {
        if (guide.rect.contains(cursor))
            return guide.zone;
    }
    return drop_zone::none;
}

QRect preview_rect(drop_zone zone, const QRect& pane, const QRect& area)
{
    switch (zone)
    {
    case drop_zone::centre:
        return pane;
    case drop_zone::left:
        return QRect(pane.left(), pane.top(), pane.width() / 2, pane.height());
    case drop_zone::right:
        return QRect(pane.left() + pane.width() / 2, pane.top(),
                     pane.width() - pane.width() / 2, pane.height());
    case drop_zone::top:
        return QRect(pane.left(), pane.top(), pane.width(), pane.height() / 2);
    case drop_zone::bottom:
        return QRect(pane.left(), pane.top() + pane.height() / 2, pane.width(),
                     pane.height() - pane.height() / 2);
    case drop_zone::window_left:
        return QRect(area.left(), area.top(), area.width() / 4, area.height());
    case drop_zone::window_right:
        return QRect(area.right() + 1 - area.width() / 4, area.top(),
                     area.width() / 4, area.height());
    case drop_zone::window_top:
        return QRect(area.left(), area.top(), area.width(), area.height() / 4);
    case drop_zone::window_bottom:
        return QRect(area.left(), area.bottom() + 1 - area.height() / 4,
                     area.width(), area.height() / 4);
    case drop_zone::none:
        break;
    }
    return {};
}

} // namespace vss::gui
