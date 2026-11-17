//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/decoded_dialog.hpp"

#include "support/plugin_text.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace vss::gui::support
{
namespace
{

constexpr int kMaxRows = 5000;

} // namespace

DecodedDialog::DecodedDialog(PluginBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend)
{
    setObjectName(QStringLiteral("decodedDialog"));
    const QString title = tr("Decoded traffic");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(900, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto* sub = new QLabel(
        tr("What the decoders found on your ports. Decoding only reads. The "
           "words in the table come from the plugin."),
        this);
    sub->setWordWrap(true);
    sub->setFont(Theme::instance().font(text_style::caption));
    sub->setProperty("muted", true);
    layout->addWidget(sub);

    triggers_only_ = new QCheckBox(tr("Triggers only"), this);
    triggers_only_->setObjectName(QStringLiteral("decodedTriggersOnly"));
    triggers_only_->setAccessibleName(tr("Triggers only"));
    layout->addWidget(triggers_only_);

    empty_ = new QLabel(this);
    empty_->setObjectName(QStringLiteral("decodedEmpty"));
    empty_->setWordWrap(true);
    empty_->setProperty("muted", true);
    layout->addWidget(empty_);

    model_ = new QStandardItemModel(0, 6, this);
    model_->setHorizontalHeaderLabels({tr("Time"), tr("Place"), tr("Direction"),
                                       tr("Type"), tr("Message"), tr("What")});
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("decodedTable"));
    table_->setAccessibleName(tr("Decoded traffic"));
    table_->setModel(model_);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_, 1);

    counts_ = new QLabel(this);
    counts_->setObjectName(QStringLiteral("decodedCounts"));
    counts_->setFont(Theme::instance().font(text_style::caption));
    counts_->setProperty("muted", true);
    layout->addWidget(counts_);

    auto* close = new QPushButton(tr("Close"), this);
    close->setAccessibleName(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    footer->addWidget(close);
    layout->addLayout(footer);

    connect(triggers_only_, &QCheckBox::toggled, this,
            [this]
            {
                signature_.clear();
                refresh();
            });
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_->start();
    refresh();
}

void DecodedDialog::refresh()
{
    const plugin_snapshot_ptr snapshot = backend_.snapshot();
    std::vector<plugin_found_row> lines;
    QString signature = triggers_only_->isChecked() ? QStringLiteral("t")
                                                    : QStringLiteral("a");
    int instances = 0;
    quint64 held = 0;
    quint64 dropped = 0;
    if (snapshot)
    {
        for (const plugin_view& plugin : snapshot->plugins)
        {
            for (const plugin_instance_view& instance : plugin.instances)
            {
                if (!instance.decoder)
                    continue;
                ++instances;
                const plugin_found_ptr found =
                    backend_.found(plugin.id, instance.number);
                if (!found)
                    continue;
                signature += QStringLiteral("|%1:%2:%3")
                                 .arg(plugin.id)
                                 .arg(instance.number)
                                 .arg(found->revision);
                held += found->held != 0 ? found->held : found->rows.size();
                dropped += found->dropped + found->refused + found->over_budget;
                for (const plugin_found_row& each : found->rows)
                {
                    if (triggers_only_->isChecked() && !each.trigger)
                        continue;
                    lines.push_back(each);
                }
            }
        }
    }
    if (signature == signature_)
        return;
    signature_ = signature;

    std::stable_sort(lines.begin(), lines.end(),
                     [](const plugin_found_row& left,
                        const plugin_found_row& right)
                     { return left.time_ns > right.time_ns; });
    if (lines.size() > static_cast<std::size_t>(kMaxRows))
        lines.resize(kMaxRows);

    model_->setRowCount(0);
    model_->setRowCount(static_cast<int>(lines.size()));
    int index = 0;
    for (const plugin_found_row& found : lines)
    {
        const QStringList cells = {
            tr("%1 s").arg(static_cast<double>(found.time_ns) / 1e9, 0, 'f', 3),
            plain_text(found.place),
            found.received ? tr("Received") : tr("Sent"),
            found.trigger ? tr("Trigger") : tr("Frame"),
            plain_text(found.kind),
            plain_text(found.what)};
        for (int column = 0; column < cells.size(); ++column)
            model_->setItem(index, column, new QStandardItem(cells[column]));
        ++index;
    }
    const bool any = !lines.empty();
    table_->setVisible(any);
    empty_->setVisible(!any);
    if (instances == 0)
        empty_->setText(tr("Nothing is being decoded. Choose Decode as... in "
                           "the menu of a port to start."));
    else if (triggers_only_->isChecked())
        empty_->setText(
            tr("No triggers. Everything decoded so far has fitted the "
               "protocol."));
    else
        empty_->setText(tr("Nothing found yet."));
    QStringList parts;
    if (held > static_cast<quint64>(lines.size()) &&
        !triggers_only_->isChecked())
        parts << tr("Showing the newest %1 of %2.").arg(lines.size()).arg(held);
    if (dropped != 0)
        parts << tr("%1 left out to stay within the limits.").arg(dropped);
    counts_->setText(parts.join(QLatin1Char(' ')));
    counts_->setVisible(!parts.isEmpty());
}

int DecodedDialog::row_count() const
{
    return model_->rowCount();
}

QString DecodedDialog::cell(int row, int column) const
{
    const QStandardItem* item = model_->item(row, column);
    return item == nullptr ? QString() : item->text();
}

} // namespace vss::gui::support
