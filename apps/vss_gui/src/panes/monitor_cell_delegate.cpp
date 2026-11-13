//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/monitor_cell_delegate.hpp"

#include <vss/core/profile.hpp>

#include <QFontMetricsF>
#include <QModelIndex>
#include <QStringView>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <cmath>

namespace vss::gui
{

MonitorCellDelegate::MonitorCellDelegate(int long_text_column, QObject* parent)
    : QStyledItemDelegate(parent), long_text_column_(long_text_column)
{
}

void MonitorCellDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.monitor.paint_cell");
    VSS_PROFILE_ZONE_TEXT(zone, column_label(index.column()));
    QStyledItemDelegate::paint(painter, option, index);
}

std::string_view MonitorCellDelegate::column_label(int column) noexcept
{
    switch (column)
    {
    case 0:
        return "time";
    case 1:
        return "port";
    case 2:
        return "direction";
    case 3:
        return "bytes";
    default:
        return "preview";
    }
}

double MonitorCellDelegate::uniform_advance(const QFont& font)
{
    // Measured, not asked of the font: a font that calls itself fixed-width
    // may lack letters (an emoji font), and Qt then takes them from another
    // font, where they have many widths.
    const QFontMetricsF metrics(font);
    const double advance = metrics.horizontalAdvance(QLatin1Char('0'));
    if (advance <= 0.0)
        return 0.0;
    for (char character = 0x20; character < 0x7f; ++character)
    {
        const double other = metrics.horizontalAdvance(QLatin1Char(character));
        if (std::abs(other - advance) > 0.01)
            return 0.0;
    }
    return advance;
}

qsizetype MonitorCellDelegate::characters_kept(int width,
                                               double advance) noexcept
{
    const double fill = static_cast<double>(std::max(width, 0)) / advance;
    return static_cast<qsizetype>(fill * 1.25) + 16;
}

void MonitorCellDelegate::initStyleOption(QStyleOptionViewItem* option,
                                          const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    if (index.column() != long_text_column_)
        return;
    if (!measured_ || option->font != font_)
    {
        font_ = option->font;
        measured_ = true;
        advance_ = uniform_advance(font_);
    }
    if (advance_ <= 0.0)
        return;
    const qsizetype keep = characters_kept(option->rect.width(), advance_);
    if (option->text.size() <= keep)
        return;
    // Payload previews are ASCII. Other text (the translated sentence of a
    // dropped row) may hold characters of other widths and is left whole.
    const QStringView kept = QStringView(option->text).first(keep);
    if (std::all_of(kept.begin(), kept.end(),
                    [](QChar character) { return character.unicode() < 0x80; }))
        option->text.truncate(keep);
}

} // namespace vss::gui
