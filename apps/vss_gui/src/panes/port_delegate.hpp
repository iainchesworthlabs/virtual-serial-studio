//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_PORT_DELEGATE_HPP
#define VSS_GUI_PANES_PORT_DELEGATE_HPP

#include <vss/core/types.hpp>

#include <QStyledItemDelegate>

#include <functional>

class QStyleOptionViewItem;

namespace vss::gui
{

/// Paints the three-line Simple row and the two-line Advanced table row.
/// It never creates a widget: the copy hit area is handled by editorEvent().
class PortDelegate final : public QStyledItemDelegate
{
public:
    explicit PortDelegate(QObject* parent = nullptr);

    /// Width of a state chip holding `text`, as painted.
    static int state_chip_width(const QString& text);

    void set_compact(bool compact);
    void set_table_mode(bool table) { table_ = table; }
    void set_copy_handler(std::function<void(vss::endpoint_id)> handler)
    {
        copy_handler_ = std::move(handler);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

protected:
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

private:
    QRect copy_rect_for(const QStyleOptionViewItem& option,
                        const QModelIndex& index) const;

    bool compact_ = false;
    bool table_ = false;
    mutable QRect copy_rect_;
    std::function<void(vss::endpoint_id)> copy_handler_;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_PORT_DELEGATE_HPP
