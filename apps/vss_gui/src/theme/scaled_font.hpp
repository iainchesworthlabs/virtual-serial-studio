//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QFont>

namespace vss::gui
{

/// `font` made `scale` times as large, for a view whose text is larger than
/// the theme's by a factor (text made larger on a widget, as the tests do,
/// next to the text made larger through `Theme`). The font comes from
/// `Theme::font`; this only multiplies its size, so no size is named outside
/// the theme.
inline QFont scaled_font(QFont font, double scale)
{
    font.setPointSizeF(font.pointSizeF() * scale);
    return font;
}

} // namespace vss::gui
