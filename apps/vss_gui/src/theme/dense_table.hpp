//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "theme/theme.hpp"

#include <QFontMetrics>

#include <algorithm>

namespace vss::gui
{

/// The sizes of a dense table, the Monitor and the Log, from the theme's
/// density. One place, so the two tables have the same rows and headings.
///
/// A row is a pointer target, so it is never lower than the target size, in
/// either density (open question 1 of the Monitor design, whether Compact may
/// use 20 px rows, is not answered). Above that, a row is its text and a
/// little above and below: 4 px in Comfortable, 2 px in Compact.
inline int dense_row_height(int text_height)
{
    const Theme& theme = Theme::instance();
    const bool compact = theme.density() == density_mode::compact;
    const int vertical = compact ? theme.space(0) / 2 : theme.space(0);
    return std::max(theme.metric(QStringLiteral("minTarget")),
                    text_height + 2 * vertical);
}

/// The column heading row: its text and a little above and below, and at
/// least a target.
inline int dense_header_height(const QFont& font)
{
    const Theme& theme = Theme::instance();
    return std::max(theme.metric(QStringLiteral("minTarget")),
                    QFontMetrics(font).height() + 2 * theme.space(0));
}

/// Space at the left and right of a cell.
inline int dense_cell_padding()
{
    const Theme& theme = Theme::instance();
    return theme.density() == density_mode::compact ? theme.space(0)
                                                    : theme.space(1);
}

} // namespace vss::gui
