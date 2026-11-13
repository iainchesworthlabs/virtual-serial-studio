//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/monitor_panel.hpp"

#include "panes/monitor_model.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString kind_name(vss::event_kind kind)
{
    switch (kind)
    {
    case vss::event_kind::started:
        return QObject::tr("started");
    case vss::event_kind::stopped:
        return QObject::tr("stopped");
    case vss::event_kind::data:
        return QObject::tr("data");
    case vss::event_kind::lines:
        return QObject::tr("lines");
    case vss::event_kind::error:
        return QObject::tr("error");
    case vss::event_kind::stats:
        return QObject::tr("stats");
    case vss::event_kind::log_message:
        return QObject::tr("log");
    case vss::event_kind::bus_fault:
        return QObject::tr("bus");
    }
    return {};
}

/// DirectionBadge, painted in the monitor delegate rather than a per-row
/// widget.
class DirectionDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const Theme& theme = Theme::instance();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        painter->save();
        painter->fillRect(option.rect,
                          selected ? theme.colour(colour_role::accentSoft)
                                   : theme.colour(colour_role::panel));
        const auto direction = static_cast<traffic_direction>(
            index.data(MonitorModel::direction_role).toInt());
        const QString text = index.data(Qt::DisplayRole).toString();
        // RX in the accent tint, TX in violet and DROP in the danger tint:
        // tinted tags with no outline, as in DirectionBadge.
        QColor fill = theme.colour(colour_role::accentSoft);
        QColor ink = theme.colour(colour_role::accentText);
        if (direction == traffic_direction::tx)
        {
            fill = theme.colour(colour_role::violetSoft);
            ink = theme.colour(colour_role::violet);
        }
        else if (direction == traffic_direction::drop)
        {
            fill = theme.colour(colour_role::badSoft);
            ink = theme.colour(colour_role::bad);
        }
        const QRect badge(option.rect.left() + 6, option.rect.center().y() - 9,
                          40, 18);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(badge), 5, 5);
        QFont font = theme.font(text_style::caption);
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(ink);
        painter->drawText(badge, Qt::AlignCenter, text);
        painter->restore();
    }
};

} // namespace

MonitorPanel::MonitorPanel(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    build();
    connect(&backend_, &StudioBackend::batch_ready, this,
            [this](const event_batch& events) { handle_events(events); });
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this]
            {
                refresh_ports();
                update_drop_rows();
                update_empty();
            });
    connect(&backend_, &StudioBackend::links_changed, this,
            [this]
            {
                update_drop_rows();
                refresh_ports();
            });
    // Drops are counted per link, and they are what the counters change for.
    connect(&backend_, &StudioBackend::stats_changed, this,
            [this] { update_drop_rows(); });
    refresh_ports();
    update_actions();
    update_empty();
}

void MonitorPanel::build()
{
    setAccessibleName(tr("Monitor"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The panel is a tab widget whose tab bar stays hidden unless the extra
    // tabs are enabled; the traffic page below is the one normally seen.
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setTabBarAutoHide(true);
    tabs_->setAccessibleName(tr("Monitor tabs"));
    root->addWidget(tabs_);

    monitor_page_ = new QWidget(tabs_);
    auto* page_layout = new QVBoxLayout(monitor_page_);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);

    // Simple toolbar: pause, clear and the Hex and Follow options. Its Hex
    // box is kept in step with the advanced preview selector below.
    simple_bar_ = new QWidget(monitor_page_);
    simple_bar_->setFixedHeight(36);
    auto* simple_layout = new QHBoxLayout(simple_bar_);
    simple_layout->setContentsMargins(12, 0, 12, 0);
    simple_layout->setSpacing(12);
    simple_pause_ = new QPushButton(tr("Pause"), simple_bar_);
    simple_pause_->setIcon(IconProvider::instance().icon(
        QStringLiteral("pause"), colour_role::ink, 16));
    simple_pause_->setProperty("size", QStringLiteral("small"));
    polish(simple_pause_);
    clear_ = new QPushButton(tr("Clear"), simple_bar_);
    clear_->setProperty("size", QStringLiteral("small"));
    polish(clear_);
    hex_ = new QCheckBox(tr("Hex"), simple_bar_);
    follow_ = new QCheckBox(tr("Follow"), simple_bar_);
    follow_->setChecked(true);
    all_ports_ = new QLabel(tr("All ports"), simple_bar_);
    all_ports_->setFont(Theme::instance().font(text_style::caption));
    all_ports_->setProperty("muted", true);
    polish(all_ports_);
    simple_layout->addWidget(simple_pause_);
    simple_layout->addWidget(clear_);
    simple_layout->addWidget(hex_);
    simple_layout->addWidget(follow_);
    simple_layout->addStretch(1);
    simple_layout->addWidget(all_ports_);
    page_layout->addWidget(simple_bar_);

    // Advanced toolbar: the filters (port, direction, payload search), the
    // preview format, and the export buttons. Hidden until set_advanced().
    advanced_bar_ = new QWidget(monitor_page_);
    advanced_bar_->setMinimumHeight(36);
    advanced_layout_ = new QHBoxLayout(advanced_bar_);
    advanced_layout_->setContentsMargins(12, 0, 12, 0);
    advanced_layout_->setSpacing(8);
    QHBoxLayout* advanced_layout = advanced_layout_;
    page_layout_ = page_layout;
    advanced_bar_->installEventFilter(this);
    port_filter_ = new QComboBox(advanced_bar_);
    port_filter_->setAccessibleName(tr("Filter by port"));
    port_filter_->setMinimumWidth(120);
    port_filter_->setMaximumWidth(160);
    rx_ = new QCheckBox(tr("RX"), advanced_bar_);
    tx_ = new QCheckBox(tr("TX"), advanced_bar_);
    drop_ = new QCheckBox(tr("Dropped"), advanced_bar_);
    // Every direction starts shown, so the table is unfiltered at first.
    rx_->setChecked(true);
    tx_->setChecked(true);
    drop_->setChecked(true);
    search_ = new QLineEdit(advanced_bar_);
    search_->setPlaceholderText(tr("Search payload"));
    search_->setAccessibleName(tr("Search payload"));
    search_->setClearButtonEnabled(true);
    search_->setMaximumWidth(190);
    mode_ = new SegmentedControl(advanced_bar_);
    mode_->set_compact(true);
    mode_->setAccessibleName(tr("Preview format"));
    mode_->add_item(tr("ASCII"));
    mode_->add_item(tr("Hex"), 1);
    pause_ = new QPushButton(tr("Pause"), advanced_bar_);
    pause_->setIcon(IconProvider::instance().icon(QStringLiteral("pause"),
                                                  colour_role::ink, 16));
    pause_->setProperty("size", QStringLiteral("small"));
    polish(pause_);
    auto* advanced_clear = new QPushButton(tr("Clear"), advanced_bar_);
    advanced_clear->setProperty("size", QStringLiteral("small"));
    polish(advanced_clear);
    auto* advanced_follow = new QCheckBox(tr("Follow"), advanced_bar_);
    advanced_follow->setChecked(true);
    export_ = new QPushButton(tr("Export"), advanced_bar_);
    export_->setProperty("size", QStringLiteral("small"));
    polish(export_);
    // Export saves the visible rows as text; Export capture writes the traffic
    // with every byte and its exact time to a capture file.
    export_capture_ = new QPushButton(tr("Export capture"), advanced_bar_);
    export_capture_->setProperty("size", QStringLiteral("small"));
    export_capture_->setAccessibleName(tr("Export capture"));
    export_capture_->setAccessibleDescription(
        tr("Save the traffic shown with every byte and its exact time to a "
           "capture file."));
    export_capture_->setToolTip(export_capture_->accessibleDescription());
    polish(export_capture_);
    advanced_layout->addWidget(port_filter_);
    advanced_layout->addWidget(rx_);
    advanced_layout->addWidget(tx_);
    advanced_layout->addWidget(drop_);
    advanced_layout->addWidget(search_);
    advanced_layout->addWidget(mode_);
    advanced_layout->addStretch(1);
    advanced_layout->addWidget(pause_);
    advanced_layout->addWidget(advanced_clear);
    advanced_layout->addWidget(advanced_follow);
    advanced_layout->addWidget(export_);
    advanced_layout->addWidget(export_capture_);
    advanced_bar_->setVisible(false);
    page_layout->addWidget(advanced_bar_);

    // The traffic table. The first four columns have fixed widths and the
    // last one takes the remaining space.
    model_ = new MonitorModel(this);
    table_ = new QTableView(monitor_page_);
    table_->setModel(model_);
    table_->setAccessibleName(tr("Traffic"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(28);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setFixedHeight(28);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table_->setColumnWidth(0, 130);
    table_->setColumnWidth(1, 140);
    table_->setColumnWidth(2, 70);
    table_->setColumnWidth(3, 70);
    table_->setItemDelegateForColumn(2, new DirectionDelegate(table_));
    table_->setFocusPolicy(Qt::StrongFocus);
    table_->setMinimumHeight(60);
    table_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // A placeholder is shown in place of the table while there is no traffic;
    // update_empty() switches between the two.
    empty_ = new QLabel(monitor_page_);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setFont(Theme::instance().font(text_style::body));
    empty_->setProperty("muted", true);
    empty_->setContentsMargins(16, 16, 16, 16);
    empty_->setText(tr("No traffic yet. Send some bytes, or open the port in a "
                       "program."));

    content_ = new QStackedWidget(monitor_page_);
    content_->setMinimumHeight(60);
    content_->addWidget(table_);
    content_->addWidget(empty_);
    page_layout->addWidget(content_, 1);
    tabs_->addTab(monitor_page_, tr("Monitor"));

    // The log tab keeps only the newest 10,000 lines so it cannot grow without
    // bound.
    log_ = new QPlainTextEdit(tabs_);
    log_->setReadOnly(true);
    log_->setAccessibleName(tr("Log"));
    log_->setFont(Theme::instance().font(text_style::data));
    log_->setMaximumBlockCount(10'000);
    tabs_->addTab(log_, tr("Log"));

    // The Log and Topology JSON tabs are hidden until set_advanced() shows
    // them, and the tab bar with them.
    json_ = new QLabel(tr("The topology JSON editor is not available yet."),
                       tabs_);
    json_->setAlignment(Qt::AlignCenter);
    json_->setWordWrap(true);
    json_->setProperty("muted", true);
    polish(json_);
    tabs_->addTab(json_, tr("Topology JSON"));
    tabs_->setTabVisible(1, false);
    tabs_->setTabVisible(2, false);
    tabs_->tabBar()->hide();

    // Space pauses only while the table has focus, so it cannot steal the key
    // from the search box or other widgets. Find jumps to the search box,
    // which exists only in the advanced toolbar.
    auto* pause_shortcut = new QShortcut(QKeySequence(Qt::Key_Space), table_);
    pause_shortcut->setContext(Qt::WidgetShortcut);
    connect(pause_shortcut, &QShortcut::activated, this,
            [this] { toggle_pause(); });
    auto* search_shortcut = new QShortcut(QKeySequence::Find, this);
    search_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(search_shortcut, &QShortcut::activated, this,
            [this]
            {
                if (advanced_)
                    search_->setFocus();
            });

    // Toolbar wiring. Both toolbars drive the same state: pause, clear and
    // follow are shared, so either bar can be used.
    connect(pause_, &QPushButton::clicked, this, [this] { toggle_pause(); });
    connect(simple_pause_, &QPushButton::clicked, this,
            [this] { toggle_pause(); });
    connect(advanced_clear, &QPushButton::clicked, this,
            [this]
            {
                model_->clear();
                update_actions();
                update_empty();
            });
    connect(advanced_follow, &QCheckBox::toggled, this,
            [this](bool on) { follow_enabled_ = on; });
    connect(clear_, &QPushButton::clicked, this,
            [this]
            {
                model_->clear();
                update_actions();
                update_empty();
            });
    connect(export_, &QPushButton::clicked, this, [this] { export_visible(); });
    connect(export_capture_, &QPushButton::clicked, this,
            [this] { export_capture(); });
    // The Hex checkbox and the ASCII/Hex selector show one setting. Each
    // handler blocks the other's signals so they do not call each other back.
    connect(hex_, &QCheckBox::toggled, this,
            [this](bool on)
            {
                model_->set_hex(on);
                const QSignalBlocker blocker(mode_);
                mode_->set_current(on ? 1 : 0);
            });
    connect(follow_, &QCheckBox::toggled, this,
            [this](bool on) { follow_enabled_ = on; });
    connect(mode_, &SegmentedControl::current_changed, this,
            [this](int id)
            {
                model_->set_hex(id == 1);
                const QSignalBlocker blocker(hex_);
                hex_->setChecked(id == 1);
            });
    // Any change to a filter control re-applies all the filters together.
    connect(port_filter_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { update_filters(); });
    for (QCheckBox* box : {rx_, tx_, drop_})
        connect(box, &QCheckBox::toggled, this,
                [this](bool) { update_filters(); });
    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString&) { update_filters(); });
    // Clicking a row selects the port that row came from.
    connect(table_, &QTableView::clicked, this,
            [this](const QModelIndex& index)
            {
                const auto id = model_->port_at(index.row());
                if (id != 0)
                    Q_EMIT port_selected(id);
            });
    // A model reset (such as Clear) refreshes the toolbar state and the
    // placeholder, and scrolls to the end if following.
    connect(model_, &QAbstractItemModel::modelReset, this,
            [this]
            {
                update_actions();
                update_empty();
                follow_if_enabled();
            });
}

void MonitorPanel::set_advanced(bool advanced)
{
    if (advanced_ == advanced)
        return;
    advanced_ = advanced;
    simple_bar_->setVisible(!advanced);
    advanced_bar_->setVisible(advanced);
    place_controls();
    tabs_->setTabVisible(1, advanced && show_advanced_tabs_);
    tabs_->setTabVisible(2, advanced && show_advanced_tabs_);
    tabs_->tabBar()->setVisible(advanced && show_advanced_tabs_);
    if (!advanced)
        tabs_->setCurrentIndex(0);
    update_filters();
    update_actions();
}

void MonitorPanel::attach_header(PaneHeader* header)
{
    if (header_ != nullptr)
        header_->removeEventFilter(this);
    header_ = header;
    if (header_ != nullptr)
        header_->installEventFilter(this);
    place_controls();
}

bool MonitorPanel::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (watched == header_ &&
        (type == QEvent::Resize || type == QEvent::Show ||
         type == QEvent::FontChange || type == QEvent::StyleChange))
    {
        place_controls();
    }
    else if (watched == advanced_bar_ &&
             (type == QEvent::FontChange || type == QEvent::StyleChange))
    {
        // The controls got wider or narrower: the same test decides again.
        QTimer::singleShot(0, this, [this] { place_controls(); });
    }
    return QWidget::eventFilter(watched, event);
}

void MonitorPanel::place_controls()
{
    if (advanced_bar_ == nullptr || page_layout_ == nullptr)
        return;
    // The test uses size hints only, so the answer does not depend on where
    // the controls are now and cannot flip back and forth.
    const bool want_header =
        advanced_ && header_ != nullptr && header_->isVisible() &&
        header_->width() >=
            header_->lead_width() + advanced_bar_->sizeHint().width();
    if (want_header == in_header_)
        return;
    in_header_ = want_header;
    if (want_header)
    {
        page_layout_->removeWidget(advanced_bar_);
        advanced_layout_->setContentsMargins(0, 0, 0, 0);
        advanced_bar_->setMinimumHeight(0);
        header_->set_fill(advanced_bar_);
    }
    else
    {
        if (header_ != nullptr)
            header_->set_fill(nullptr);
        advanced_layout_->setContentsMargins(12, 0, 12, 0);
        advanced_bar_->setMinimumHeight(36);
        page_layout_->insertWidget(1, advanced_bar_);
    }
    advanced_bar_->setVisible(advanced_);
}

void MonitorPanel::set_compact(bool compact)
{
    table_->verticalHeader()->setDefaultSectionSize(compact ? 24 : 28);
}

void MonitorPanel::set_show_advanced_tabs(bool show)
{
    if (show_advanced_tabs_ == show)
        return;
    show_advanced_tabs_ = show;
    const bool advanced = advanced_;
    advanced_ = !advanced;
    set_advanced(advanced);
}

void MonitorPanel::set_selected_port(vss::endpoint_id id)
{
    model_->set_selected_port(id);
}

void MonitorPanel::refresh_ports()
{
    QStringList signature;
    for (const auto& port : backend_.ports())
    {
        signature << QStringLiteral("%1|%2")
                         .arg(static_cast<qulonglong>(port.id))
                         .arg(port.name);
    }
    if (signature == port_signature_)
        return;
    port_signature_ = signature;

    const vss::endpoint_id previous =
        port_filter_ != nullptr && port_filter_->currentIndex() >= 0
            ? static_cast<vss::endpoint_id>(
                  port_filter_->currentData().toULongLong())
            : 0;
    refreshing_ports_ = true;
    if (port_filter_ != nullptr)
    {
        const QSignalBlocker blocker(port_filter_);
        port_filter_->clear();
        port_filter_->addItem(tr("All ports"),
                              QVariant::fromValue<qulonglong>(0));
        for (const auto& port : backend_.ports())
        {
            port_filter_->addItem(port.name,
                                  QVariant::fromValue<qulonglong>(port.id));
        }
        const int index =
            port_filter_->findData(QVariant::fromValue<qulonglong>(previous));
        port_filter_->setCurrentIndex(index >= 0 ? index : 0);
    }
    refreshing_ports_ = false;
    if (all_ports_ != nullptr)
        all_ports_->setText(tr("All ports"));
    update_filters();
}

void MonitorPanel::handle_events(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.monitor.handle_events");
    model_->append_events(events);
    for (const gui_event& event : events)
    {
        if (event.kind == vss::event_kind::data)
            continue;
        append_log(event);
    }
    // New rows change only whether there is anything to export. The rest of
    // `update_actions` sets the Pause button's icon, and Qt repaints a button
    // and lays its row out again whenever an icon is set, changed or not:
    // that was a relayout of the bar and a repaint of its controls for every
    // batch of traffic.
    const bool any_rows = model_->rowCount() > 0;
    if (export_ != nullptr)
        export_->setEnabled(any_rows);
    if (export_capture_ != nullptr)
        export_capture_->setEnabled(any_rows);
    update_empty();
    follow_if_enabled();
}

void MonitorPanel::update_filters()
{
    if (model_ == nullptr || port_filter_ == nullptr)
        return;
    const vss::endpoint_id port =
        refreshing_ports_ ? 0
                          : static_cast<vss::endpoint_id>(
                                port_filter_->currentData().toULongLong());
    if (advanced_)
    {
        model_->set_filters(port, rx_->isChecked(), tx_->isChecked(),
                            drop_->isChecked(), search_->text());
    }
    else
    {
        model_->set_filters(0, true, true, true, {});
    }
    update_empty();
}

void MonitorPanel::update_actions()
{
    if (model_ == nullptr)
        return;
    const QString pause_text = model_->paused() ? tr("Resume") : tr("Pause");
    // Resume is the play triangle; Pause keeps its bars.
    const QIcon pause_icon = IconProvider::instance().icon(
        model_->paused() ? QStringLiteral("run") : QStringLiteral("pause"),
        colour_role::ink, 16);
    pause_->setText(pause_text);
    pause_->setIcon(pause_icon);
    if (simple_pause_ != nullptr)
    {
        simple_pause_->setText(pause_text);
        simple_pause_->setIcon(pause_icon);
    }
    if (export_ != nullptr)
        export_->setEnabled(model_->rowCount() > 0);
    if (export_capture_ != nullptr)
        export_capture_->setEnabled(model_->rowCount() > 0);
    if (hex_ != nullptr && hex_->isChecked() != model_->hex())
    {
        const QSignalBlocker blocker(hex_);
        hex_->setChecked(model_->hex());
    }
    if (mode_ != nullptr)
    {
        const QSignalBlocker blocker(mode_);
        mode_->set_current(model_->hex() ? 1 : 0);
    }
}

void MonitorPanel::update_empty()
{
    if (content_ == nullptr || model_ == nullptr)
        return;
    if (model_->rowCount() > 0)
    {
        content_->setCurrentWidget(table_);
        return;
    }
    if (model_->total_rows() > 0)
    {
        empty_->setText(tr("No traffic matches these filters."));
    }
    else
    {
        empty_->setText(tr("No traffic yet. Send some bytes, or open the port "
                           "in a program."));
    }
    content_->setCurrentWidget(empty_);
}

void MonitorPanel::update_drop_rows()
{
    const auto links = backend_.links();
    const auto ports = backend_.ports();
    QHash<vss::link_id, std::uint64_t> current;
    for (const auto& link : links)
    {
        current.insert(link.id, link.stats.dropped);
        const auto previous = previous_drops_.constFind(link.id);
        if (previous == previous_drops_.constEnd())
            continue;
        if (link.stats.dropped <= *previous)
            continue;
        const auto name_of = [&ports](vss::endpoint_id id) -> QString
        {
            const auto it = std::find_if(ports.begin(), ports.end(),
                                         [id](const port_view& view)
                                         { return view.id == id; });
            return it == ports.end() ? tr("link") : it->name;
        };
        const vss::endpoint_id id = !link.sources.empty() ? link.sources.front()
                                    : !link.targets.empty()
                                        ? link.targets.front()
                                        : 0;
        model_->append_drop(name_of(id), link.stats.dropped - *previous,
                            link.shape == link_shape::bus);
    }
    previous_drops_ = std::move(current);
}

void MonitorPanel::append_log(const gui_event& event)
{
    if (log_ == nullptr)
        return;
    const QString time =
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    QString text = event.error;
    if (text.isEmpty())
        text = event.message;
    if (text.isEmpty())
        text = kind_name(event.kind);
    log_->appendPlainText(
        QStringLiteral("%1  %2  %3  %4")
            .arg(time, event.name, kind_name(event.kind), text));
}

void MonitorPanel::toggle_pause()
{
    model_->set_paused(!model_->paused());
    update_actions();
}

void MonitorPanel::export_visible()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export visible traffic"), QStringLiteral("traffic.txt"),
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        Q_EMIT error_message(tr("Could not save %1.").arg(path));
        return;
    }
    file.write(model_->export_text().toUtf8());
    Q_EMIT status_message(tr("Exported %1.").arg(path));
}

void MonitorPanel::export_capture()
{
    const QString cap_filter = tr("Capture text (*.cap)");
    const QString csv_filter = tr("Comma separated values (*.csv)");
    const QString jsonl_filter = tr("JSON Lines (*.jsonl)");
    QString selected = cap_filter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export capture"), QStringLiteral("capture.cap"),
        cap_filter + QStringLiteral(";;") + csv_filter + QStringLiteral(";;") +
            jsonl_filter,
        &selected);
    if (path.isEmpty())
        return;

    // The extension decides when there is one; otherwise the filter does, and
    // the file gets the extension that goes with it.
    vss::capture_format format = vss::capture_format::cap;
    if (selected == csv_filter)
        format = vss::capture_format::csv;
    else if (selected == jsonl_filter)
        format = vss::capture_format::jsonl;
    if (const auto named = vss::capture_format_for_path(path.toStdString()))
        format = *named;
    else
        path += QLatin1Char('.') + QString::fromLatin1(vss::to_string(format));

    const capture_export rows = model_->export_capture();
    vss::capture_header header;
    header.started = rows.started;
    const auto written = vss::write_capture_file(path.toStdString(), format,
                                                 header, rows.records);
    if (!written)
    {
        Q_EMIT error_message(tr("Could not save %1.").arg(path));
        return;
    }
    Q_EMIT status_message(tr("Exported %1.").arg(path));
}

void MonitorPanel::follow_if_enabled()
{
    if (!follow_enabled_ || table_ == nullptr)
        return;
    QTimer::singleShot(0, table_,
                       [this]
                       {
                           if (model_->rowCount() > 0)
                               table_->scrollToBottom();
                       });
}

} // namespace vss::gui
