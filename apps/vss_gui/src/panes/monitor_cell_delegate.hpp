//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QFont>
#include <QSize>
#include <QString>
#include <QStyledItemDelegate>

#include <string_view>

namespace vss::gui
{

/// What the cells of the Monitor table share: the fonts they are drawn in and
/// the space at the sides of a cell. The panel owns one and gives every
/// delegate a pointer to it, and `update` runs whenever the theme, the density
/// or the text size changes.
///
/// Every cell is a 12 px style at the 13 px base: Time, Bytes and Preview in
/// the data font, port names in the caption font. Both are multiplied by the
/// view's scale, the table's font over the body font, so text made larger
/// through `Theme` and text made larger on the widget both reach the cells.
struct monitor_cell_style
{
    /// Time, Bytes and Preview.
    QFont data;
    /// Port names.
    QFont caption;
    /// The words in the direction tags: the caption font, semi-bold.
    QFont tag;
    /// Space at the left and at the right of a cell.
    int padding = 0;

    /// Takes the fonts from `Theme`, multiplied by `scale`.
    void update(double scale, int padding_pixels);

    /// A direction tag is as wide as its text plus 12 and as high as its text
    /// plus 2.
    QSize tag_size(const QString& text) const;
};

/// The Monitor table's cells as Qt draws them, each inside a profiling zone
/// (`gui.monitor.paint_cell`, with the column as its text).
///
/// Qt lays a cell's whole text out before it cuts it to the cell, and that
/// costs about 2 microseconds a character: a 256 byte preview is 256
/// characters as text and 767 as hex, of which a cell shows the first hundred
/// or so. For the cells of one column, when the text is all ASCII and the
/// font's ASCII characters all have one width, the delegate hands Qt only as
/// many characters as are sure to overflow the cell. Qt cuts those where it would have cut the whole
/// text and draws the same ellipsis, so the cell looks the same. The model,
/// the tooltip and the accessible text keep the whole text.
class MonitorCellDelegate final : public QStyledItemDelegate
{
public:
    /// `long_text_column` is the column whose text is cut; -1 for none.
    explicit MonitorCellDelegate(int long_text_column,
                                 QObject* parent = nullptr);

    /// Takes the fonts from `style`, which must outlive the delegate: the
    /// port names (column 1) in its caption font and every other cell in its
    /// data font, whatever the model answers for the font role. With none,
    /// which is the start, a cell has the font its model gives it.
    void set_style(const monitor_cell_style* style) noexcept { style_ = style; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    /// The zone text of a painted cell: the name of its column.
    static std::string_view column_label(int column) noexcept;

    /// The width of a character of `font` when every printable ASCII
    /// character has that width, and 0 when they differ.
    static double uniform_advance(const QFont& font);

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
    const monitor_cell_style* style_ = nullptr;
    // The width of a character of the font last seen, 0 when its characters
    // differ in width. Looking a font up for every cell would cost more than
    // it saves.
    mutable QFont font_;
    mutable double advance_ = 0.0;
    mutable bool measured_ = false;
};

} // namespace vss::gui
