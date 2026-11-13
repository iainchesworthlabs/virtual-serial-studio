//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QtTypes>

namespace vss::gui::rendering
{

/// For a scene item that knows when its own drawing changed.
///
/// The GPU canvas keeps one texture per item. A scene only reports the
/// rectangles that changed, so without this an item is drawn again whenever
/// anything that overlaps it changes: a link that follows a moved node takes
/// the node at its other end with it. An item that derives from this class is
/// drawn again only when its revision has moved (or its size or the zoom has),
/// so moving it, or changing a neighbour, costs no drawing.
///
/// The item counts its revision up wherever it calls `update()` for a change
/// in what `paint()` draws. A revision is never 0: the canvas uses 0 for an
/// item that does not report one.
///
/// This is an interface only (no state), so an item can derive from it
/// beside `QGraphicsItem`.
class content_revision
{
public:
    virtual ~content_revision() = default;

    [[nodiscard]] virtual quint64 revision() const noexcept = 0;
};

} // namespace vss::gui::rendering
