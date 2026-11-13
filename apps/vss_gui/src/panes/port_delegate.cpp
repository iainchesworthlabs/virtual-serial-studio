//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/port_delegate.hpp"

#include "panes/port_model.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

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

struct chip_style
{
    QColor fill;
    QColor edge;
    QColor text;
    QColor mark;
    bool hollow = false;
    bool cross = false;
};

chip_style style_for(port_state_kind kind)
{
    // The same tinted capsules as StatusChip: no outline, the mark and text
    // in the state's own colour.
    const Theme& theme = Theme::instance();
    const QColor none = Qt::transparent;
    switch (kind)
    {
    case port_state_kind::open:
    case port_state_kind::connected:
        return {theme.colour(colour_role::okSoft),
                none,
                theme.colour(colour_role::ok),
                theme.colour(colour_role::ok),
                false,
                false};
    case port_state_kind::waiting:
        return {theme.colour(colour_role::amberSoft),
                none,
                theme.colour(colour_role::amber),
                theme.colour(colour_role::amber),
                true,
                false};
    case port_state_kind::studio_only:
    case port_state_kind::closed:
        return {theme.colour(colour_role::fill),
                none,
                theme.colour(colour_role::ink2),
                theme.colour(colour_role::ink3),
                true,
                false};
    case port_state_kind::failed:
        return {theme.colour(colour_role::badSoft),
                none,
                theme.colour(colour_role::bad),
                theme.colour(colour_role::bad),
                false,
                true};
    case port_state_kind::fixture:
        return {theme.colour(colour_role::violetSoft),
                none,
                theme.colour(colour_role::violet),
                theme.colour(colour_role::violet),
                true,
                false};
    }
    return {};
}

int chip_width(const QString& text)
{
    const QFont font = Theme::instance().font(text_style::caption);
    QFont bold = font;
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(bold);
    return metrics.horizontalAdvance(text) + 26;
}

void paint_chip(QPainter* painter, const QRect& rect, const QString& text,
                port_state_kind kind)
{
    const chip_style style = style_for(kind);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(style.fill);
    painter->drawRoundedRect(QRectF(rect), rect.height() / 2.0,
                             rect.height() / 2.0);

    QFont font = Theme::instance().font(text_style::caption);
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(style.text);
    const int dot_space = 14;
    const QRect text_rect = rect.adjusted(dot_space, 0, -6, 0);
    painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, text);

    const QPointF centre(rect.left() + 8.0, rect.center().y() + 0.5);
    painter->setPen(QPen(style.mark, 1.2));
    painter->setBrush(style.hollow ? Qt::NoBrush : style.mark);
    if (style.cross)
    {
        const qreal arm = 3.0;
        painter->drawLine(QPointF(centre.x() - arm, centre.y() - arm),
                          QPointF(centre.x() + arm, centre.y() + arm));
        painter->drawLine(QPointF(centre.x() - arm, centre.y() + arm),
                          QPointF(centre.x() + arm, centre.y() - arm));
    }
    else
    {
        painter->drawEllipse(centre, 3.0, 3.0);
    }
    painter->restore();
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
    const int left = option.rect.left() + (table_ ? 12 : 16);
    const int top = table_ ? option.rect.top() + 20
                           : option.rect.bottom() - (compact_ ? 28 : 30);
    return QRect(left + metrics.horizontalAdvance(path) + 4, top, 24, 24);
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
            painter->setFont(theme.font(text_style::data));
            painter->setPen(theme.colour(colour_role::ink2));
            painter->drawText(option.rect.adjusted(6, 0, -8, 0),
                              Qt::AlignRight | Qt::AlignVCenter, text);
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
    painter->drawText(
        QRect(left, top, name_width, 20), Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(name_font).elidedText(name, Qt::ElideRight, name_width));
    if (!table_)
    {
        paint_chip(
            painter,
            QRect(right - chip_width(state), top - 1, chip_width(state), 20),
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
    painter->drawText(QRect(left, path_y, right - left - 34, 24),
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
                             14,
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
