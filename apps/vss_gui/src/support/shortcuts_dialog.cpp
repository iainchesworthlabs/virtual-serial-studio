//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/shortcuts_dialog.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

QStringList key_parts(const QKeySequence& shortcut)
{
    return shortcut.toString(QKeySequence::PortableText)
        .split(QLatin1Char('+'), Qt::SkipEmptyParts);
}

QString joined(const QStringList& parts)
{
    return parts.join(QLatin1Char('+'));
}

QWidget* key_cell(const QString& text, QWidget* parent)
{
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(0);
    layout->addWidget(new KeyCap(text, holder));
    layout->addStretch(1);
    return holder;
}

QStringList action_texts_of(const QVector<shortcut_entry>& entries)
{
    QStringList texts;
    for (const shortcut_entry& entry : entries)
    {
        if (!entry.shortcut.isEmpty())
            texts.push_back(entry.action);
    }
    return texts;
}

} // namespace

ShortcutsDialog::ShortcutsDialog(const QVector<shortcut_entry>& entries,
                                 QWidget* parent)
    : QDialog(parent), entries_(entries)
{
    build();
    rebuild();
}

QString ShortcutsDialog::windows_linux_text(const QKeySequence& shortcut)
{
    QStringList parts = key_parts(shortcut);
    for (QString& part : parts)
    {
        if (part == QLatin1String("Del"))
            part = tr("Delete");
    }
    return joined(parts);
}

QString ShortcutsDialog::macos_text(const QKeySequence& shortcut)
{
    QStringList parts = key_parts(shortcut);
    for (QString& part : parts)
    {
        if (part == QLatin1String("Ctrl") || part == QLatin1String("Meta"))
            part = QStringLiteral("Cmd");
        else if (part == QLatin1String("Del"))
            part = tr("Delete");
    }
    return joined(parts);
}

void ShortcutsDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.shortcuts.build");
    VSS_PROFILE_MESSAGE("gui.dialog.shortcuts.opened");
    // Window chrome: accessible name, panel surface and the wide dialog
    // width, which is also the minimum.
    setWindowTitle(
        QCoreApplication::translate("Shortcuts", "Keyboard shortcuts"));
    setAccessibleName(
        QCoreApplication::translate("Shortcuts", "Keyboard shortcuts"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide")),
           700);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide")));

    // The outer layout has no margins so the header, body and footer can set
    // their own padding. The header holds the heading and the filter box side
    // by side.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(16);
    auto* heading = new QVBoxLayout();
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(4);
    add_dialog_heading(
        heading, QCoreApplication::translate("Shortcuts", "Keyboard shortcuts"),
        QCoreApplication::translate(
            "Shortcuts",
            "Every action has a menu item. These are the quick ways."),
        header);
    header_layout->addLayout(heading, 1);

    // The filter box has a leading search icon and a clear button. Typing
    // re-filters the table through set_filter().
    filter_ = new QLineEdit(header);
    filter_->setObjectName(QStringLiteral("shortcutFilter"));
    filter_->setPlaceholderText(
        QCoreApplication::translate("Shortcuts", "Filter shortcuts"));
    filter_->setAccessibleName(
        QCoreApplication::translate("Shortcuts", "Filter shortcuts"));
    filter_->setClearButtonEnabled(true);
    filter_->setProperty("search", true);
    filter_->setFixedWidth(240);
    filter_
        ->addAction(IconProvider::instance().icon(QStringLiteral("search"),
                                                  colour_role::ink2, 16),
                    QLineEdit::LeadingPosition)
        ->setText(QCoreApplication::translate("Shortcuts", "Filter shortcuts"));
    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString& text) { set_filter(text); });
    header_layout->addWidget(filter_, 0, Qt::AlignVCenter);
    outer->addWidget(header);

    // Body: a group label over the shortcut table.
    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(8);
    body_layout->addWidget(new GroupLabel(
        QCoreApplication::translate("Shortcuts", "Available today"), body));

    // Three columns: the action stretches and the two shortcut columns keep
    // a fixed width. The table starts empty; rebuild() fills the rows.
    table_ = new QTableWidget(0, 3, body);
    table_->setObjectName(QStringLiteral("shortcutTable"));
    table_->setAccessibleName(
        QCoreApplication::translate("Shortcuts", "Keyboard shortcuts"));
    table_->setHorizontalHeaderLabels(
        {QCoreApplication::translate("Shortcuts", "Action"),
         QCoreApplication::translate("Shortcuts", "Windows and Linux"),
         QCoreApplication::translate("Shortcuts", "macOS")});
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                    Qt::AlignVCenter);
    table_->horizontalHeader()->setFont(
        Theme::instance().font(text_style::group));
    // The table is read-only: rows can be selected for reading, not edited.
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->verticalHeader()->setDefaultSectionSize(Theme::instance().metric(
        QStringLiteral("density.comfortable.rowHeight")));
    table_->setFocusPolicy(Qt::StrongFocus);
    body_layout->addWidget(table_, 1);
    outer->addWidget(body, 1);

    // Footer: a note on the left, inserted before the button, and Close.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* note = new QLabel(
        QCoreApplication::translate(
            "Shortcuts",
            "Qt maps Ctrl to Cmd on macOS. Shortcuts also appear in menus and "
            "tooltips. Open this list with Help, Keyboard shortcuts."),
        footer);
    note->setFont(Theme::instance().font(text_style::caption));
    note->setProperty("muted", true);
    note->setWordWrap(true);
    note->setMaximumWidth(620);
    polish(note);
    footer_layout->insertWidget(0, note);
    auto* close = make_dialog_button(
        QCoreApplication::translate("Shortcuts", "Close"), "primary", footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    outer->addWidget(footer);

    // Focus the filter now and again once the event loop runs, in case
    // showing the dialog moves focus elsewhere first.
    filter_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, filter_,
                       [this] { filter_->setFocus(Qt::OtherFocusReason); });
}

void ShortcutsDialog::rebuild()
{
    VSS_PROFILE_ZONE("gui.support.shortcuts.rebuild");
    const QVector<shortcut_entry> shown = [this]
    {
        QVector<shortcut_entry> out;
        for (const shortcut_entry& entry : entries_)
        {
            if (!entry.shortcut.isEmpty())
                out.push_back(entry);
        }
        return out;
    }();

    table_->setRowCount(0);
    table_->setRowCount(static_cast<int>(shown.size()));
    for (int row = 0; row < shown.size(); ++row)
    {
        const shortcut_entry& entry = shown.at(row);
        auto* action = new QTableWidgetItem(entry.action);
        action->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        action->setData(Qt::UserRole, entry.shortcut.toString());
        table_->setItem(row, 0, action);
        table_->setCellWidget(
            row, 1, key_cell(windows_linux_text(entry.shortcut), table_));
        table_->setCellWidget(row, 2,
                              key_cell(macos_text(entry.shortcut), table_));
    }
    fit_key_columns();
    set_filter(filter_->text());
}

void ShortcutsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // The caps take their final font, and the cells their place, once the
    // dialog is laid out for show, so measure again after that.
    QTimer::singleShot(0, this, [this] { fit_key_columns(); });
}

void ShortcutsDialog::fit_key_columns()
{
    // ResizeToContents measures items, not cell widgets, so it clipped the
    // longer key caps.  Size the key columns to their widest cap, plus the
    // item padding the view puts around a cell widget.
    QHeaderView* header = table_->horizontalHeader();
    for (int column = 1; column < table_->columnCount(); ++column)
    {
        int width = header->sectionSizeHint(column);
        for (int row = 0; row < table_->rowCount(); ++row)
        {
            QWidget* cell = table_->cellWidget(row, column);
            if (cell == nullptr)
                continue;
            for (QWidget* child : cell->findChildren<QWidget*>())
                child->ensurePolished();
            cell->ensurePolished();
            if (cell->layout() != nullptr)
                cell->layout()->invalidate();
            const int inset =
                cell->width() > 0
                    ? std::max(0, header->sectionSize(column) - cell->width())
                    : 0;
            width = std::max(width, cell->sizeHint().width() + inset);
        }
        header->resizeSection(column, width);
    }
}

void ShortcutsDialog::set_filter(const QString& text)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone, "gui.support.shortcuts.filter");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, text.toStdString());
    const QString needle = text.trimmed();
    for (int row = 0; row < table_->rowCount(); ++row)
    {
        bool match = needle.isEmpty();
        if (!match)
        {
            for (int column = 0; column < table_->columnCount() && !match;
                 ++column)
            {
                if (QTableWidgetItem* item = table_->item(row, column))
                    match = item->text().contains(needle, Qt::CaseInsensitive);
            }
        }
        if (!match)
        {
            match =
                windows_linux_text(
                    QKeySequence(
                        table_->item(row, 0)->data(Qt::UserRole).toString()))
                    .contains(needle, Qt::CaseInsensitive);
        }
        table_->setRowHidden(row, !match);
    }
}

int ShortcutsDialog::total_row_count() const
{
    return table_ != nullptr ? table_->rowCount() : 0;
}

int ShortcutsDialog::visible_row_count() const
{
    if (table_ == nullptr)
        return 0;
    int count = 0;
    for (int row = 0; row < table_->rowCount(); ++row)
    {
        if (!table_->isRowHidden(row))
            ++count;
    }
    return count;
}

QStringList ShortcutsDialog::action_texts() const
{
    return action_texts_of(entries_);
}

} // namespace vss::gui::support
