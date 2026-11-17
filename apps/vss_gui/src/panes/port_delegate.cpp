//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/port_delegate.hpp"

#include "panes/port_model.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include <algorithm>

namespace vss::gui
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

StatusChip::kind chip_kind_of(port_state_kind kind)
{
    switch (kind)
    {
    case port_state_kind::open:
        return StatusChip::kind::open;
    case port_state_kind::connected:
        return StatusChip::kind::connected;
    case port_state_kind::waiting:
        return StatusChip::kind::waiting;
    case port_state_kind::studio_only:
        return StatusChip::kind::studio_only;
    case port_state_kind::closed:
        return StatusChip::kind::closed;
    case port_state_kind::failed:
        return StatusChip::kind::failed;
    case port_state_kind::fixture:
        return StatusChip::kind::fixture;
    case port_state_kind::paused:
        return StatusChip::kind::paused;
    }
    return StatusChip::kind::closed;
}

// The chip is drawn by StatusChip, the same code as the Inspector's.  Its
// width does not depend on the state, so the text alone gives it.
int chip_width(const QString& text)
{
    return StatusChip::chip_width(text, StatusChip::kind::open);
}

void paint_chip(QPainter* painter, const QRect& rect, const QString& text,
                port_state_kind kind)
{
    StatusChip::paint(painter, rect, text, chip_kind_of(kind));
}

} // namespace

PortDelegate::PortDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

int PortDelegate::state_chip_width(const QString& text)
{
    return chip_width(text);
}

void PortDelegate::set_compact(bool compact)
{
    compact_ = compact;
}

QRect PortDelegate::copy_rect_for(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    if (index.column() != 0)
        return {};
    const QString path = index.data(PortModel::path_role).toString();
    const QFont mono = Theme::instance().font(text_style::data);
    const QFontMetrics metrics(mono);
    const int pad = table_ ? 12 : 16;
    const int top = table_ ? option.rect.top() + 20
                           : option.rect.bottom() - (compact_ ? 28 : 30);
    // The path is as wide as the row lets it be, and the button follows its
    // end: to the right of it, or to the left in a right-to-left layout.
    const int room = std::max(0, option.rect.width() - 2 * pad - 34);
    const int width = std::min(metrics.horizontalAdvance(path), room);
    if (option.direction == Qt::RightToLeft)
        return QRect(option.rect.right() - pad - width - 4 - 24, top, 24, 24);
    return QRect(option.rect.left() + pad + width + 4, top, 24, 24);
}

void PortDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
{
    VSS_PROFILE_ZONE("gui.paint.port_row");
    const Theme& theme = Theme::instance();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    if (table_)
    {
        painter->fillRect(option.rect,
                          selected  ? theme.colour(colour_role::accentSoft)
                          : hovered ? theme.colour(colour_role::hover)
                                    : theme.colour(colour_role::panel));
    }
    else
    {
        // A list row is a rounded tile inset from the pane edge; selection
        // and hover fill the tile, and there are no rules between rows.
        painter->fillRect(option.rect, theme.colour(colour_role::panel));
        if (selected || hovered)
        {
            const qreal radius =
                Theme::instance().radius(QStringLiteral("inner"));
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? theme.colour(colour_role::accentSoft)
                                       : theme.colour(colour_role::hover));
            painter->drawRoundedRect(QRectF(option.rect).adjusted(6, 2, -6, -2),
                                     radius, radius);
        }
    }

    if (table_ && index.column() > 0)
    {
        const QString text = index.data(Qt::DisplayRole).toString();
        if (index.column() == 1)
        {
            const auto kind = static_cast<port_state_kind>(
                index.data(PortModel::state_kind_role).toInt());
            const int width = chip_width(text);
            paint_chip(painter,
                       QRect(option.rect.left() + 4,
                             option.rect.center().y() - 10, width, 20),
                       text, kind);
        }
        else
        {
            const QFont font = theme.font(text_style::data);
            painter->setFont(font);
            painter->setPen(theme.colour(colour_role::ink2));
            // The model says which side a column's text sits on; a long
            // address or kind is cut with an ellipsis, not run into the
            // next column.
            const QRect cell = option.rect.adjusted(8, 0, -8, 0);
            const auto alignment = static_cast<Qt::Alignment>(
                index.data(Qt::TextAlignmentRole).toInt());
            const bool right = alignment.testFlag(Qt::AlignRight);
            const Qt::Alignment horizontal =
                right ? Qt::AlignRight : Qt::AlignLeft;
            painter->drawText(
                cell, static_cast<int>((horizontal | Qt::AlignVCenter).toInt()),
                QFontMetrics(font).elidedText(text, Qt::ElideRight,
                                              cell.width()));
        }
        painter->restore();
        return;
    }

    const QString name = index.data(PortModel::name_role).toString();
    const QString path = index.data(PortModel::path_role).toString();
    const QString role_text = index.data(PortModel::role_text_role).toString();
    const QString state = index.data(PortModel::state_text_role).toString();
    const auto kind = static_cast<port_state_kind>(
        index.data(PortModel::state_kind_role).toInt());

    const int left = option.rect.left() + (table_ ? 12 : 16);
    const int right = option.rect.right() - (table_ ? 12 : 16);
    const int top = option.rect.top() + (compact_ ? 6 : 8);
    const QFont name_font = semibold(
        theme.font(compact_ ? text_style::body : text_style::bodyLarge));
    painter->setFont(name_font);
    painter->setPen(theme.colour(colour_role::ink));
    const int name_width =
        table_ ? std::max(0, right - left)
               : std::max(0, right - left - chip_width(state) - 8);
    // In a right-to-left layout the chip is on the left, so the name starts
    // after it.
    const int name_left = (!table_ && option.direction == Qt::RightToLeft)
                              ? left + chip_width(state) + 8
                              : left;
    painter->drawText(
        QRect(name_left, top, name_width, 20), Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(name_font).elidedText(name, Qt::ElideRight, name_width));
    if (!table_)
    {
        // The chip is at the end of the line, opposite the name.
        const int chip_left = option.direction == Qt::RightToLeft
                                  ? left
                                  : right - chip_width(state);
        paint_chip(painter, QRect(chip_left, top - 1, chip_width(state), 20),
                   state, kind);
    }

    if (!compact_ && !table_)
    {
        painter->setFont(theme.font(text_style::caption));
        // What the engine knows of the other side shares the line under the
        // name.  A warning takes the whole line, in the warning colour, since
        // it is the part that needs acting on.
        const QString status = index.data(PortModel::status_role).toString();
        const bool warn = index.data(PortModel::format_warning_role).toBool();
        const QString line = status.isEmpty() ? role_text
                             : warn ? status
                                    : role_text + QStringLiteral(". ") + status;
        painter->setPen(
            theme.colour(warn ? colour_role::noteWarnText : colour_role::ink3));
        painter->drawText(QRect(left, top + 20, right - left, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(theme.font(text_style::caption))
                              .elidedText(line, Qt::ElideRight, right - left));
    }

    const int path_y = table_ ? option.rect.top() + 22
                              : option.rect.bottom() - (compact_ ? 28 : 30);
    painter->setFont(theme.font(text_style::data));
    painter->setPen(theme.colour(colour_role::ink2));
    const QFontMetrics mono(theme.font(text_style::data));
    // The button takes the last 34 pixels of the line, so in a right-to-left
    // row the path starts after it and ends at the right edge.
    const int path_left =
        option.direction == Qt::RightToLeft ? left + 34 : left;
    painter->drawText(QRect(path_left, path_y, right - left - 34, 24),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      mono.elidedText(path, Qt::ElideRight, right - left - 34));

    copy_rect_ = copy_rect_for(option, index);
    if (!path.isEmpty())
    {
        const QIcon icon = IconProvider::instance().icon(QStringLiteral("copy"),
                                                         colour_role::ink3, 14);
        icon.paint(painter, copy_rect_.adjusted(5, 5, -5, -5));
    }

    const QString serial = index.data(PortModel::serial_role).toString();
    if (!serial.isEmpty() && !compact_ && !table_)
    {
        painter->setFont(theme.font(text_style::caption));
        painter->setPen(theme.colour(colour_role::ink3));
        painter->drawText(
            QRect(left, option.rect.bottom() - 26, right - left, 18),
            Qt::AlignRight | Qt::AlignVCenter, serial);
    }

    painter->restore();
}

QSize PortDelegate::sizeHint(const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    Q_UNUSED(option);
    if (table_)
    {
        // The width is what a double click on a header divider fits to: the
        // widest this column's cell paints, with the padding paint() uses.
        const QString text = index.data(Qt::DisplayRole).toString();
        switch (index.column())
        {
        case 0:
        {
            const Theme& theme = Theme::instance();
            QFont name_font = theme.font(text_style::body);
            name_font.setWeight(QFont::DemiBold);
            const QString name = index.data(PortModel::name_role).toString();
            const QString path = index.data(PortModel::path_role).toString();
            return QSize(
                std::max(QFontMetrics(name_font).horizontalAdvance(name) + 24,
                         QFontMetrics(theme.font(text_style::data))
                                 .horizontalAdvance(path) +
                             24 + 4 + 24),
                44);
        }
        case 1:
            return QSize(chip_width(text) + 12, 44);
        default:
            return QSize(QFontMetrics(Theme::instance().font(text_style::data))
                                 .horizontalAdvance(text) +
                             16,
                         44);
        }
    }
    return QSize(240, compact_ ? 58 : 78);
}

bool PortDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        const QRect copy_rect = copy_rect_for(option, index);
        if (copy_rect.isValid() &&
            copy_rect.contains(
                static_cast<QMouseEvent*>(event)->position().toPoint()))
        {
            if (copy_handler_)
                copy_handler_(static_cast<vss::endpoint_id>(
                    index.data(PortModel::port_id_role).toULongLong()));
            return true;
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace vss::gui
