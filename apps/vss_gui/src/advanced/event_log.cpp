//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/event_log.hpp"

#include "advanced/chrome.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

constexpr int kRowCap = 2000;

QString kind_text(vss::event_kind kind)
{
    switch (kind)
    {
    case vss::event_kind::started:
        return QCoreApplication::translate("EventLog", "Started");
    case vss::event_kind::stopped:
        return QCoreApplication::translate("EventLog", "Stopped");
    case vss::event_kind::lines:
        return QCoreApplication::translate("EventLog", "Control lines");
    case vss::event_kind::error:
        return QCoreApplication::translate("EventLog", "Error");
    case vss::event_kind::stats:
        return QCoreApplication::translate("EventLog", "Counters");
    case vss::event_kind::log_message:
        return QCoreApplication::translate("EventLog", "Message");
    case vss::event_kind::data:
        return QCoreApplication::translate("EventLog", "Traffic");
    case vss::event_kind::bus_fault:
        return QCoreApplication::translate("EventLog", "Bus");
    }
    return QCoreApplication::translate("EventLog", "Message");
}

} // namespace

EventLogPanel::EventLogPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(12, 6, 12, 6);
    toolbar_layout->setSpacing(8);
    auto* title = new QLabel(tr("Event log"), toolbar);
    QFont title_font = Theme::instance().font(text_style::body);
    title_font.setWeight(QFont::DemiBold);
    title->setFont(title_font);
    toolbar_layout->addWidget(title);
    auto* caption = new QLabel(
        tr("State changes and errors since the setup was opened."), toolbar);
    caption->setFont(Theme::instance().font(text_style::caption));
    caption->setProperty("muted", true);
    caption->setWordWrap(true);
    toolbar_layout->addWidget(caption, 1);
    auto* clear = make_button(tr("Clear"), nullptr, toolbar);
    clear->setProperty("size", QStringLiteral("small"));
    polish(clear);
    clear->setAccessibleName(tr("Clear the event log"));
    connect(clear, &QPushButton::clicked, this, &EventLogPanel::clear);
    toolbar_layout->addWidget(clear);
    layout->addWidget(toolbar);

    table_ = new QTableWidget(0, 4, this);
    table_->setAccessibleName(tr("Event log"));
    table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Port"), tr("Event"), tr("Detail")});
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(0, 110);
    table_->setColumnWidth(1, 140);
    table_->setColumnWidth(2, 110);
    layout->addWidget(table_, 1);

    empty_ = new QLabel(
        tr("No events yet. Start or stop a port, or apply a change."), this);
    empty_->setFont(Theme::instance().font(text_style::caption));
    empty_->setProperty("muted", true);
    empty_->setContentsMargins(12, 4, 12, 8);
    layout->addWidget(empty_);

    update_empty();
}

void EventLogPanel::append(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.advanced.event_log.append");
    const QString now =
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    for (const gui_event& event : events)
    {
        if (event.kind == vss::event_kind::data)
            continue;
        QString detail = event.message;
        if (!event.error.isEmpty())
            detail = event.error;
        if (event.kind == vss::event_kind::lines)
            detail = QString::fromStdString(vss::to_string(event.lines));
        if (detail.isEmpty())
            detail = tr("No details.");
        add_row(now, event.name, kind_text(event.kind), detail);
    }
    update_empty();
}

void EventLogPanel::add_local(const QString& text)
{
    add_row(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), {},
            tr("Studio"), text);
    update_empty();
}

void EventLogPanel::add_row(const QString& time, const QString& port,
                            const QString& event, const QString& detail)
{
    while (table_->rowCount() >= kRowCap)
        table_->removeRow(0);
    const int row = table_->rowCount();
    table_->insertRow(row);
    const QStringList values = {time, port, event, detail};
    for (int column = 0; column < values.size(); ++column)
    {
        auto* item = new QTableWidgetItem(values.at(column));
        item->setToolTip(values.at(column));
        table_->setItem(row, column, item);
    }
    table_->scrollToBottom();
}

void EventLogPanel::clear()
{
    table_->setRowCount(0);
    update_empty();
}

int EventLogPanel::row_count() const
{
    return table_->rowCount();
}

void EventLogPanel::update_empty()
{
    empty_->setVisible(table_->rowCount() == 0);
}

} // namespace vss::gui
