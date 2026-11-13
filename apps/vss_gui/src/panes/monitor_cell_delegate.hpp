//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QFont>
#include <QStyledItemDelegate>

#include <string_view>

namespace vss::gui
{

/// The Monitor table's cells as Qt draws them, each inside a profiling zone
/// (`gui.monitor.paint_cell`, with the column as its text).
///
/// Qt lays a cell's whole text out before it cuts it to the cell, and that
/// costs about 2 microseconds a character: a 256 byte preview is 256
/// characters as text and 767 as hex, of which a cell shows the first hundred
/// or so. For the cells of one column, in a font whose characters all have
/// one width, the delegate hands Qt only as many characters as are sure to
/// overflow the cell. Qt cuts those where it would have cut the whole text
/// and draws the same ellipsis, so the cell looks the same. The model, the
/// tooltip and the accessible text keep the whole text.
class MonitorCellDelegate final : public QStyledItemDelegate
{
public:
    /// `long_text_column` is the column whose text is cut; -1 for none.
    explicit MonitorCellDelegate(int long_text_column,
                                 QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    /// The zone text of a painted cell: the name of its column.
    static std::string_view column_label(int column) noexcept;

    /// How many characters of a text are handed to Qt for a cell `width`
    /// pixels wide when every character is `advance` pixels wide: a quarter
    /// more than fill the cell and 16 on top, room for characters drawn a
    /// little narrower than measured.
    static qsizetype characters_kept(int width, double advance) noexcept;

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override;

private:
    int long_text_column_;
    // The width of a character of the font last seen, 0 when its characters
    // differ in width. Looking a font up for every cell would cost more than
    // it saves.
    mutable QFont font_;
    mutable double advance_ = 0.0;
    mutable bool measured_ = false;
};

} // namespace vss::gui
