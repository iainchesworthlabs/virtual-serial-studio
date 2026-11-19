//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QPoint>
#include <QRect>

#include <vector>

namespace vss::gui
{

/// Where a pane dragged over another pane, or over the window, would land.
/// The first five are relative to the pane under the cursor; the last four
/// are the edges of the whole dock area.
enum class drop_zone
{
    none,
    centre,
    left,
    right,
    top,
    bottom,
    window_left,
    window_right,
    window_top,
    window_bottom
};

/// One drop guide: the zone it stands for and where it is drawn.
struct drop_guide
{
    drop_zone zone = drop_zone::none;
    QRect rect;
};

/// True for the four zones that mean an edge of the whole window.
[[nodiscard]] bool is_window_zone(drop_zone zone) noexcept;

/// The guides drawn over one pane: a centre square (tab into the pane) and
/// one square on each side of it (split that side).  `size` is the guide's
/// width and height and `gap` the space between guides.  A pane too small for
/// the whole cross gets the centre guide only.
[[nodiscard]] std::vector<drop_guide> pane_guides(const QRect& pane, int size,
                                                  int gap);

/// The guides drawn at the middle of each edge of `area`, `margin` in from
/// the edge.
[[nodiscard]] std::vector<drop_guide> window_guides(const QRect& area,
                                                    int size, int margin);

/// The zone under `cursor`: a window-edge guide first, then a guide of the
/// pane. `none` when the cursor is on no guide.
[[nodiscard]] drop_zone zone_at(const QPoint& cursor, const QRect& pane,
                                const QRect& area, int size, int gap,
                                int margin);

/// The shaded rectangle that shows where the pane would land.  A pane zone
/// previews half of `pane` (all of it for the centre); a window zone previews
/// a strip along that edge of `area`.  Empty for `none`.
[[nodiscard]] QRect preview_rect(drop_zone zone, const QRect& pane,
                                 const QRect& area);

} // namespace vss::gui
