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
/// The item calls `content_changed()` wherever it calls `update()` for a
/// change in what `paint()` draws.
class content_revision
{
public:
    [[nodiscard]] quint64 revision() const noexcept { return revision_; }

protected:
    content_revision() = default;
    ~content_revision() = default;
    content_revision(const content_revision&) = default;
    content_revision& operator=(const content_revision&) = default;

    void content_changed() noexcept { ++revision_; }

private:
    quint64 revision_ = 1;
};

} // namespace vss::gui::rendering
