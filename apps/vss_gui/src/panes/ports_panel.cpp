//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/ports_panel.hpp"

#include "backend/other_ports.hpp"
#include "panes/port_delegate.hpp"
#include "panes/port_model.hpp"
#include "theme/theme.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{

PortsPanel::PortsPanel(StudioBackend& backend, SelectionService& selection,
                       QWidget* parent)
    : QWidget(parent), backend_(backend), selection_(selection)
{
    build();
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { refresh(); });
    connect(&backend_, &StudioBackend::links_changed, this,
            [this] { refresh(); });
    // Traffic counters change many times a second; one refresh covers every
    // signal that arrives before the event loop gets back to it.
    connect(&backend_, &StudioBackend::stats_changed, this,
            [this] { schedule_refresh(); });
    connect(&selection_, &SelectionService::port_changed, this,
            [this](vss::endpoint_id id) { select_port(id); });
    refresh();
    refresh_others();

    // Ports another program made raise no event, and listing them reads the
    // device list, so look again now and then rather than on every refresh.
    others_timer_ = new QTimer(this);
    others_timer_->setInterval(5000);
    connect(others_timer_, &QTimer::timeout, this,
            [this] { refresh_others(); });
    others_timer_->start();
}

void PortsPanel::build()
{
    // Top to bottom: the filter box, the port views, and a note about ports
    // held by other programs.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Only shown in advanced mode (set_advanced). Filtering can drop the
    // selected row from view, so it is selected again afterwards.
    // The box sits in a row that carries the space around it. Margins on the
    // line edit itself would shrink its text area and misplace the clear
    // button, so it keeps the height and padding every input has.
    filter_row_ = new QWidget(this);
    filter_row_->setVisible(false);
    auto* filter_layout = new QHBoxLayout(filter_row_);
    const int gap = Theme::instance().space(2);
    filter_layout->setContentsMargins(gap, gap, gap, gap);
    filter_layout->setSpacing(0);
    filter_ = new QLineEdit(filter_row_);
    filter_->setPlaceholderText(tr("Filter ports"));
    filter_->setAccessibleName(tr("Filter ports"));
    filter_->setClearButtonEnabled(true);
    filter_->setFont(Theme::instance().font(text_style::body));
    filter_->installEventFilter(this);
    // Qt places the clear button as if it were 18 px tall; the style sheet
    // makes it 24 px (the smallest target), which leaves it 3 px low. It is
    // centred again whenever Qt moves it.
    clear_button_ = filter_->findChild<QToolButton*>();
    if (clear_button_ != nullptr)
        clear_button_->installEventFilter(this);
    filter_layout->addWidget(filter_);
    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString& text)
            {
                const vss::endpoint_id selected = selection_.port();
                model_->set_filter(text);
                if (selected != 0)
                    select_port(selected);
            });
    layout->addWidget(filter_row_);

    // Both views share one model; the stack shows either the plain list or
    // the table.
    model_ = new PortModel(this);
    stack_ = new QStackedWidget(this);

    simple_ = new QListView(stack_);
    simple_->setModel(model_);
    simple_->setAccessibleName(tr("Ports"));
    simple_->setSelectionMode(QAbstractItemView::SingleSelection);
    simple_->setSelectionBehavior(QAbstractItemView::SelectRows);
    simple_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    simple_->setUniformItemSizes(true);
    simple_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    simple_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    simple_->setMouseTracking(true);
    simple_delegate_ = new PortDelegate(simple_);
    simple_delegate_->set_copy_handler(
        [this](vss::endpoint_id id)
        {
            select_port(id);
            copy_selected_path();
        });
    simple_->setItemDelegate(simple_delegate_);
    simple_->installEventFilter(this);
    simple_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(simple_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { open_menu(simple_, position); });
    connect(simple_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&)
            { on_current_changed(current); });

    // Every column is resized by dragging its header divider, and a double
    // click on a divider fits the column to its contents. The last column
    // takes whatever space is left, so there is no dead space on the right.
    table_ = new QTableView(stack_);
    table_->setModel(model_);
    table_->setAccessibleName(tr("Ports table"));
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setAlternatingRowColors(false);
    table_->verticalHeader()->hide();
    QHeaderView* header = table_->horizontalHeader();
    header->setHighlightSections(false);
    // Drag a heading to reorder the columns; right-click one to choose which
    // are shown.
    header->setSectionsMovable(true);
    // A click on a heading sorts by it, ascending then descending then back
    // to the engine's order.
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicatorClearable(true);
    header->setSortIndicator(-1, Qt::AscendingOrder);
    connect(header, &QHeaderView::sortIndicatorChanged, this,
            [this](int column, Qt::SortOrder order)
            {
                if (applying_widths_)
                    return;
                sort_by(column, order);
            });
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this,
            [this, header](const QPoint& position)
            { open_column_menu(header->mapToGlobal(position)); });
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setMinimumSectionSize(kMinimumColumnWidth);
    header->setStretchLastSection(true);
    header->setAccessibleName(tr("Ports table columns"));
    set_default_column_widths();
    restore_column_layout();
    restore_sort();
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    column_save_timer_ = new QTimer(this);
    column_save_timer_->setSingleShot(true);
    column_save_timer_->setInterval(400);
    connect(column_save_timer_, &QTimer::timeout, this,
            [this] { save_column_layout(); });
    connect(header, &QHeaderView::sectionResized, this,
            [this](int, int, int)
            {
                if (!applying_widths_)
                    column_save_timer_->start();
            });
    connect(header, &QHeaderView::sectionMoved, this,
            [this](int, int, int)
            {
                if (!applying_widths_)
                    column_save_timer_->start();
            });
    table_->verticalHeader()->setDefaultSectionSize(44);
    // The same delegate class paints both views; table mode paints each
    // cell for its own column instead of a whole row.
    table_delegate_ = new PortDelegate(table_);
    table_delegate_->set_table_mode(true);
    table_delegate_->set_copy_handler(
        [this](vss::endpoint_id id)
        {
            select_port(id);
            copy_selected_path();
        });
    table_->setItemDelegate(table_delegate_);
    table_->installEventFilter(this);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { open_menu(table_, position); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&)
            { on_current_changed(current); });

    // The views take all spare height (stretch 1); the note below stays
    // hidden until refresh_others() finds a reason to show it.
    stack_->addWidget(simple_);
    stack_->addWidget(table_);
    layout->addWidget(stack_, 1);

    others_ = new QLabel(this);
    others_->setObjectName(QStringLiteral("portsOtherProgram"));
    others_->setWordWrap(true);
    others_->setFont(Theme::instance().font(text_style::body));
    others_->setProperty("muted", true);
    others_->setContentsMargins(12, 8, 12, 8);
    others_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    others_->setVisible(false);
    layout->addWidget(others_);
}

namespace
{

/// The columns shown until the user chooses others.
bool shown_by_default(int column)
{
    return column <= PortModel::dropped_column;
}

QStringList to_list(const QList<int>& numbers)
{
    QStringList list;
    for (const int number : numbers)
        list.push_back(QString::number(number));
    return list;
}

} // namespace

void PortsPanel::set_default_column_widths()
{
    // The State chip must show its longest text whole.
    const int state_width =
        PortDelegate::state_chip_width(PortModel::tr("Waiting for program")) +
        12;
    applying_widths_ = true;
    table_->setColumnWidth(PortModel::port_column, 200);
    table_->setColumnWidth(PortModel::state_column, state_width);
    table_->setColumnWidth(PortModel::rx_column, 64);
    table_->setColumnWidth(PortModel::tx_column, 64);
    table_->setColumnWidth(PortModel::dropped_column, 88);
    table_->setColumnWidth(PortModel::kind_column, 176);
    table_->setColumnWidth(PortModel::address_column, 200);
    table_->setColumnWidth(PortModel::settings_column, 112);
    table_->setColumnWidth(PortModel::baud_column, 88);
    table_->setColumnWidth(PortModel::data_bits_column, 88);
    table_->setColumnWidth(PortModel::parity_column, 72);
    table_->setColumnWidth(PortModel::stop_bits_column, 88);
    table_->setColumnWidth(PortModel::flow_control_column, 104);
    table_->setColumnWidth(PortModel::errors_column, 72);
    for (int column = 0; column < PortModel::column_count; ++column)
        table_->setColumnHidden(column, !shown_by_default(column));
    applying_widths_ = false;
}

void PortsPanel::restore_column_layout()
{
    const QSettings settings;
    QHeaderView* header = table_->horizontalHeader();
    applying_widths_ = true;

    // Widths, one per column, a hidden column's as 0.
    const QStringList widths =
        settings.value(QStringLiteral("ui/portsColumnWidths")).toStringList();
    if (widths.size() == PortModel::column_count)
    {
        for (int column = 0; column < PortModel::column_count; ++column)
        {
            bool ok = false;
            const int width = widths.at(column).toInt(&ok);
            if (ok && width >= kMinimumColumnWidth && width <= 2000)
                table_->setColumnWidth(column, width);
        }
    }

    // Visibility: the columns that are shown, by number. Nothing saved, or
    // nothing that makes sense, keeps the defaults.
    if (settings.contains(QStringLiteral("ui/portsColumnsShown")))
    {
        QList<int> shown;
        for (const QString& text : settings
                 .value(QStringLiteral("ui/portsColumnsShown"))
                 .toStringList())
        {
            bool ok = false;
            const int column = text.toInt(&ok);
            if (ok && column >= 0 && column < PortModel::column_count)
                shown.push_back(column);
        }
        for (int column = 0; column < PortModel::column_count; ++column)
            table_->setColumnHidden(column,
                                    column != PortModel::port_column &&
                                        !shown.contains(column));
    }

    // Order: every column once, left to right. Anything else is ignored.
    const QStringList order =
        settings.value(QStringLiteral("ui/portsColumnOrder")).toStringList();
    QList<int> columns;
    for (const QString& text : order)
    {
        bool ok = false;
        const int column = text.toInt(&ok);
        if (ok && column >= 0 && column < PortModel::column_count &&
            !columns.contains(column))
            columns.push_back(column);
    }
    if (columns.size() == PortModel::column_count)
    {
        for (int visual = 0; visual < columns.size(); ++visual)
            header->moveSection(header->visualIndex(columns.at(visual)),
                                visual);
    }
    applying_widths_ = false;
}

void PortsPanel::save_column_layout() const
{
    const QHeaderView* header = table_->horizontalHeader();
    QStringList widths;
    QList<int> shown;
    QList<int> order;
    for (int column = 0; column < PortModel::column_count; ++column)
    {
        const bool hidden = table_->isColumnHidden(column);
        widths.push_back(QString::number(
            hidden ? 0 : table_->columnWidth(column)));
        if (!hidden)
            shown.push_back(column);
    }
    for (int visual = 0; visual < header->count(); ++visual)
        order.push_back(header->logicalIndex(visual));
    QSettings settings;
    settings.setValue(QStringLiteral("ui/portsColumnWidths"), widths);
    settings.setValue(QStringLiteral("ui/portsColumnsShown"), to_list(shown));
    settings.setValue(QStringLiteral("ui/portsColumnOrder"), to_list(order));
}

void PortsPanel::set_column_visible(int column, bool visible)
{
    if (column < 0 || column >= PortModel::column_count ||
        column == PortModel::port_column)
        return;
    table_->setColumnHidden(column, !visible);
    column_save_timer_->start();
}

void PortsPanel::reset_columns()
{
    QHeaderView* header = table_->horizontalHeader();
    applying_widths_ = true;
    for (int column = 0; column < PortModel::column_count; ++column)
        header->moveSection(header->visualIndex(column), column);
    applying_widths_ = false;
    set_default_column_widths();
    column_save_timer_->start();
}

void PortsPanel::sort_by(int column, Qt::SortOrder order)
{
    model_->set_sort(column, order);
    // The header and the model agree, whoever asked.
    QHeaderView* header = table_->horizontalHeader();
    if (header->sortIndicatorSection() != model_->sort_column() ||
        (model_->sort_column() >= 0 &&
         header->sortIndicatorOrder() != model_->sort_order()))
    {
        applying_widths_ = true;
        header->setSortIndicator(model_->sort_column(), model_->sort_order());
        applying_widths_ = false;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("ui/portsSortColumn"),
                      model_->sort_column());
    settings.setValue(QStringLiteral("ui/portsSortDescending"),
                      model_->sort_order() == Qt::DescendingOrder);
    if (model_->sort_column() < 0)
        Q_EMIT status_message(tr("Ports are not sorted."));
    else
        Q_EMIT status_message(
            (model_->sort_order() == Qt::AscendingOrder
                 ? tr("Sorted by %1, first to last.")
                 : tr("Sorted by %1, last to first."))
                .arg(model_->headerData(model_->sort_column(), Qt::Horizontal)
                         .toString()));
}

void PortsPanel::restore_sort()
{
    const QSettings settings;
    bool ok = false;
    const int column =
        settings.value(QStringLiteral("ui/portsSortColumn"), -1).toInt(&ok);
    if (!ok || column < 0 || column >= PortModel::column_count)
        return;
    const Qt::SortOrder order =
        settings.value(QStringLiteral("ui/portsSortDescending"), false)
                .toBool()
            ? Qt::DescendingOrder
            : Qt::AscendingOrder;
    applying_widths_ = true;
    model_->set_sort(column, order);
    table_->horizontalHeader()->setSortIndicator(column, order);
    applying_widths_ = false;
}

void PortsPanel::cycle_sort_by_key(int column)
{
    // The same order a click on the heading gives.
    if (model_->sort_column() != column)
        sort_by(column, Qt::AscendingOrder);
    else if (model_->sort_order() == Qt::AscendingOrder)
        sort_by(column, Qt::DescendingOrder);
    else
        sort_by(-1, Qt::AscendingOrder);
}

void PortsPanel::move_column_by_key(int column, int direction)
{
    QHeaderView* header = table_->horizontalHeader();
    // Hidden columns are skipped, so one press moves past one shown column.
    int target = header->visualIndex(column) + direction;
    while (target >= 0 && target < header->count() &&
           header->isSectionHidden(header->logicalIndex(target)))
        target += direction;
    if (target < 0 || target >= header->count())
        return;
    header->moveSection(header->visualIndex(column), target);
    Q_EMIT status_message(
        tr("%1 column moved.")
            .arg(model_->headerData(column, Qt::Horizontal).toString()));
}

void PortsPanel::open_column_menu(const QPoint& global)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(tr("Columns"));
    QHeaderView* header = table_->horizontalHeader();
    // Listed left to right as they are in the table.
    for (int visual = 0; visual < header->count(); ++visual)
    {
        const int column = header->logicalIndex(visual);
        QAction* action = menu->addAction(
            model_->headerData(column, Qt::Horizontal).toString());
        action->setCheckable(true);
        action->setChecked(!table_->isColumnHidden(column));
        // A table with no Port column would have nothing to name a row by.
        action->setEnabled(column != PortModel::port_column);
        connect(action, &QAction::toggled, this,
                [this, column](bool checked)
                { set_column_visible(column, checked); });
    }
    menu->addSeparator();
    menu->addAction(tr("Reset columns"), this, [this] { reset_columns(); });
    menu->popup(global);
}

void PortsPanel::resize_column_by_key(int column, int delta)
{
    QHeaderView* header = table_->horizontalHeader();
    const int width =
        std::max(kMinimumColumnWidth, header->sectionSize(column) + delta);
    header->resizeSection(column, width);
    Q_EMIT status_message(
        tr("%1 column is %2 px wide.")
            .arg(model_->headerData(column, Qt::Horizontal).toString())
            .arg(header->sectionSize(column)));
}

void PortsPanel::fit_column_by_key(int column)
{
    QHeaderView* header = table_->horizontalHeader();
    // The view's own fit is protected, so ask the delegate for each cell.
    int width = std::max(kMinimumColumnWidth, header->sectionSizeHint(column));
    const QStyleOptionViewItem option;
    for (int row = 0; row < model_->rowCount(); ++row)
        width = std::max(
            width, table_delegate_->sizeHint(option, model_->index(row, column))
                       .width());
    header->resizeSection(column, width);
    Q_EMIT status_message(
        tr("%1 column is %2 px wide.")
            .arg(model_->headerData(column, Qt::Horizontal).toString())
            .arg(header->sectionSize(column)));
}

void PortsPanel::refresh_others()
{
    const QString reason = other_ports_reason(backend_.other_program_ports());
    others_->setText(reason);
    others_->setAccessibleName(reason);
    others_->setVisible(!reason.isEmpty());
}

void PortsPanel::schedule_refresh()
{
    if (refresh_queued_)
        return;
    refresh_queued_ = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           refresh_queued_ = false;
                           refresh();
                       });
}

void PortsPanel::refresh()
{
    const auto ports = backend_.ports();
    // A port added or removed here can change which ports are another
    // program's, so look again then; a traffic count alone cannot.
    QStringList paths;
    for (const auto& port : ports)
        paths.push_back(port.path);
    if (paths != known_paths_)
    {
        known_paths_ = paths;
        refresh_others();
    }
    const auto links = backend_.links();
    const vss::endpoint_id wanted = selection_.port();
    // Rows are updated in place where they can be, and the horizontal scroll
    // position survives even when the model has to reset.
    const int scrolled = table_->horizontalScrollBar()->value();
    syncing_ = true;
    model_->set_snapshot(ports, links);
    syncing_ = false;
    table_->horizontalScrollBar()->setValue(scrolled);

    Q_EMIT count_changed(static_cast<int>(ports.size()));

    if (ports.empty())
    {
        // The next setup that appears gets its own first selection.
        setup_shown_ = false;
        if (selection_.port() != 0)
            selection_.select_port(0);
        return;
    }

    // A refresh never changes what is selected. The one exception is the
    // first time a setup shows: with nothing selected, the first port is.
    // After that, no port selected is a choice (a link may be selected on the
    // canvas), and a port the filter hides is still the selected port.
    const bool appeared = !setup_shown_;
    setup_shown_ = true;
    if (wanted == 0)
    {
        if (appeared && selection_.link() == 0 && model_->rowCount() > 0)
            selection_.select_port(model_->port_id(0));
        return;
    }
    const bool exists =
        std::any_of(ports.begin(), ports.end(),
                    [wanted](const auto& port) { return port.id == wanted; });
    if (!exists)
    {
        // The selected port is gone.
        selection_.select_port(0);
        return;
    }
    select_port(wanted);
}

void PortsPanel::set_advanced(bool advanced)
{
    if (advanced_ == advanced)
        return;
    advanced_ = advanced;
    filter_row_->setVisible(advanced);
    stack_->setCurrentWidget(advanced ? static_cast<QWidget*>(table_)
                                      : static_cast<QWidget*>(simple_));
    if (advanced)
    {
        model_->set_filter(filter_->text());
        table_delegate_->set_compact(compact_);
    }
    select_port(selection_.port());
}

void PortsPanel::set_compact(bool compact)
{
    compact_ = compact;
    if (simple_delegate_ != nullptr)
        simple_delegate_->set_compact(compact);
    if (table_delegate_ != nullptr)
        table_delegate_->set_compact(compact);
    if (simple_ != nullptr)
    {
        simple_->setSpacing(0);
        simple_->doItemsLayout();
    }
}

void PortsPanel::select_port(vss::endpoint_id id)
{
    if (syncing_ || model_ == nullptr)
        return;
    if (id == 0)
    {
        syncing_ = true;
        simple_->selectionModel()->clearCurrentIndex();
        table_->selectionModel()->clearCurrentIndex();
        syncing_ = false;
        return;
    }
    if (selection_.port() != id)
    {
        syncing_ = true;
        selection_.select_port(id);
        syncing_ = false;
    }
    sync_views(id);
}

void PortsPanel::on_current_changed(const QModelIndex& current)
{
    if (syncing_ || !current.isValid())
        return;
    const auto id = static_cast<vss::endpoint_id>(
        current.data(PortModel::port_id_role).toULongLong());
    if (id == 0)
        return;

    syncing_ = true;
    if (selection_.port() != id)
        selection_.select_port(id);
    sync_views(id);
    syncing_ = false;
}

void PortsPanel::sync_views(vss::endpoint_id id)
{
    const int row = model_->row_for_id(id);
    if (row < 0)
        return;
    const bool previous = syncing_;
    syncing_ = true;
    // Making the current row visible must not move the table sideways: the
    // user may have scrolled to a column to the right, and a counter changing
    // is no reason to take them back to the first one.
    QScrollBar* across = table_->horizontalScrollBar();
    const int scrolled = across->value();
    const QModelIndex index = model_->index(row, 0);
    if (simple_->currentIndex() != index)
        simple_->setCurrentIndex(index);
    // Keep the column the keyboard is on (it names the column to resize).
    if (table_->currentIndex().row() != row)
        table_->setCurrentIndex(
            model_->index(row, std::max(0, table_->currentIndex().column())));
    simple_->scrollTo(index, QAbstractItemView::EnsureVisible);
    table_->scrollTo(index, QAbstractItemView::EnsureVisible);
    across->setValue(scrolled);
    syncing_ = previous;
}

void PortsPanel::open_menu(QAbstractItemView* view, const QPoint& position)
{
    // A click names the row under the pointer; the Menu key has no pointer, so
    // it names the current row.
    QModelIndex index = view->indexAt(position);
    QPoint global = view->viewport()->mapToGlobal(position);
    if (!index.isValid())
    {
        index = view->currentIndex();
        if (!index.isValid())
            return;
        global =
            view->viewport()->mapToGlobal(view->visualRect(index).center());
    }
    const vss::endpoint_id id = model_->port_id(index.row());
    selection_.select_port(id);

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(tr("Port actions"));
    menu->addAction(tr("Inspect"), this,
                    [this, id] { Q_EMIT inspect_requested(id); });
    add_port_menu_actions(menu, this,
                          menu_state_ ? menu_state_(id) : port_menu_state{},
                          [this, id](port_menu_action action)
                          { Q_EMIT port_action_requested(id, action); });
    const auto port = model_->port_at(index.row());
    if (port && !port->path.isEmpty())
    {
        menu->addAction(tr("Copy path"), this,
                        [this] { copy_selected_path(); });
    }
    menu->popup(global);
}

int PortsPanel::current_row() const
{
    const QModelIndex current =
        advanced_ ? table_->currentIndex() : simple_->currentIndex();
    return current.isValid() ? current.row() : -1;
}

void PortsPanel::copy_selected_path()
{
    const int row = current_row();
    if (row < 0)
        return;
    const auto port = model_->port_at(row);
    if (!port || port->path.isEmpty())
        return;
    QApplication::clipboard()->setText(port->path);
    Q_EMIT path_copied(port->path);
    Q_EMIT status_message(tr("Copied %1.").arg(port->path));
}

bool PortsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == clear_button_ && event->type() == QEvent::Move &&
        !centring_clear_button_)
    {
        const int top = (filter_->height() - clear_button_->height()) / 2;
        if (clear_button_->y() != top)
        {
            centring_clear_button_ = true;
            clear_button_->move(clear_button_->x(), top);
            centring_clear_button_ = false;
        }
        return false;
    }
    // Escape in the filter box clears it; with it empty, Escape goes on.
    if (watched == filter_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        !filter_->text().isEmpty())
    {
        filter_->clear();
        return true;
    }
    if ((watched == simple_ || watched == table_) &&
        event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        const int row = current_row();
        if (row < 0)
            return QWidget::eventFilter(watched, event);
        // Ctrl+Alt+Shift+Left and Right move the column the cursor is in, and
        // Ctrl+Alt+M opens the list of columns to show.
        if (watched == table_ &&
            key->modifiers() ==
                (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier) &&
            (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right))
        {
            const bool left = key->key() == Qt::Key_Left;
            const bool rtl = table_->layoutDirection() == Qt::RightToLeft;
            move_column_by_key(table_->currentIndex().column(),
                               left != rtl ? -1 : 1);
            return true;
        }
        if (watched == table_ &&
            key->modifiers() == (Qt::ControlModifier | Qt::AltModifier) &&
            key->key() == Qt::Key_S)
        {
            cycle_sort_by_key(table_->currentIndex().column());
            return true;
        }
        if (watched == table_ &&
            key->modifiers() == (Qt::ControlModifier | Qt::AltModifier) &&
            key->key() == Qt::Key_M)
        {
            QHeaderView* header = table_->horizontalHeader();
            open_column_menu(header->mapToGlobal(header->rect().center()));
            return true;
        }
        // Ctrl+Alt+Left and Right narrow and widen the column the cursor is
        // in (Left and Right move it); Ctrl+Alt+0 fits it to its contents.
        if (watched == table_ &&
            key->modifiers() == (Qt::ControlModifier | Qt::AltModifier))
        {
            const int column = table_->currentIndex().column();
            if (key->key() == Qt::Key_Left)
            {
                resize_column_by_key(column, -16);
                return true;
            }
            if (key->key() == Qt::Key_Right)
            {
                resize_column_by_key(column, 16);
                return true;
            }
            if (key->key() == Qt::Key_0)
            {
                fit_column_by_key(column);
                return true;
            }
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            Q_EMIT inspect_requested(model_->port_id(row));
            return true;
        }
        if (key->key() == Qt::Key_Delete)
        {
            Q_EMIT remove_requested(model_->port_id(row));
            return true;
        }
        if (QKeySequence(key->keyCombination()) ==
            QKeySequence(QStringLiteral("Ctrl+Shift+C")))
        {
            copy_selected_path();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace vss::gui
