//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/monitor_panel.hpp"
#include "language/bidi.hpp"

#include "panes/monitor_cell_delegate.hpp"
#include "panes/monitor_model.hpp"
#include "panes/port_model.hpp"
#include "panes/ports_panel.hpp"
#include "theme/dense_table.hpp"
#include "widgets/filter_controls.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vss::gui
{
namespace
{

static_assert(MonitorPanel::kMinimumColumnWidth ==
              PortsPanel::kMinimumColumnWidth);

/// A port name longer than this is not measured further when the Port column
/// is sized; the cell shows the rest cut with an ellipsis.
constexpr int kLongestPortName = 24;

/// Sets a flag for as long as it lives and puts it back as it was, so the
/// scopes may nest.
class flag_scope
{
public:
    explicit flag_scope(bool& flag) : flag_(flag), before_(flag)
    {
        flag_ = true;
    }
    flag_scope(const flag_scope&) = delete;
    flag_scope& operator=(const flag_scope&) = delete;
    ~flag_scope() { flag_ = before_; }

private:
    bool& flag_;
    bool before_;
};

/// The traffic table. It raises the panel's flag while it handles a key press
/// or a mouse press, move or release, because those are the moments a change
/// of the current row is the user's. Qt also changes the current row by itself
/// (it makes the first row current when focus comes into a table that has no
/// current row), and that is not the user choosing a row.
class TrafficTable final : public QTableView
{
public:
    TrafficTable(bool& user_input, QWidget* parent)
        : QTableView(parent), user_input_(user_input)
    {
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        const flag_scope scope(user_input_);
        QTableView::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        const flag_scope scope(user_input_);
        QTableView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const flag_scope scope(user_input_);
        QTableView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const flag_scope scope(user_input_);
        QTableView::mouseReleaseEvent(event);
    }

private:
    bool& user_input_;
};

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
    case vss::event_kind::line_break:
        return QObject::tr("break");
    }
    return {};
}

/// DirectionBadge, painted in the monitor delegate rather than a per-row
/// widget.
class DirectionDelegate final : public QStyledItemDelegate
{
public:
    DirectionDelegate(const monitor_cell_style* style, QObject* parent)
        : QStyledItemDelegate(parent), style_(style)
    {
    }

    /// The tag and the space at each side of it.
    QSize sizeHint(const QStyleOptionViewItem&,
                   const QModelIndex& index) const override
    {
        const QSize tag =
            style_->tag_size(index.data(Qt::DisplayRole).toString());
        return {tag.width() + 2 * style_->padding, tag.height()};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        VSS_PROFILE_ZONE_NAMED(zone, "gui.monitor.paint_cell");
        VSS_PROFILE_ZONE_TEXT(
            zone, MonitorCellDelegate::column_label(index.column()));
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
        // The tag is as wide as its word needs, so a longer word in another
        // language is not cut off.
        const QSize size = style_->tag_size(text);
        const QRect badge(QPoint(option.rect.left() + style_->padding,
                                 option.rect.center().y() - size.height() / 2),
                          size);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(badge), 5, 5);
        painter->setFont(style_->tag);
        painter->setPen(ink);
        painter->drawText(badge, Qt::AlignCenter, text);
        painter->restore();
    }

private:
    const monitor_cell_style* style_;
};

} // namespace

MonitorPanel::MonitorPanel(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    build();
    commit_timer_ = new QTimer(this);
    commit_timer_->setSingleShot(true);
    connect(commit_timer_, &QTimer::timeout, this, [this] { commit_rows(); });
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
    // The density is the user's choice in View, Density and Settings, and
    // reaches the table through the theme.
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_metrics(); });
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
    // A minimum, not a fixed height, so larger text makes the bar taller.
    simple_bar_->setMinimumHeight(Theme::instance().row_height());
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
    port_filter_ = make_port_filter(tr("Filter by port"), advanced_bar_);
    rx_ = new QCheckBox(tr("RX"), advanced_bar_);
    tx_ = new QCheckBox(tr("TX"), advanced_bar_);
    drop_ = new QCheckBox(tr("Dropped"), advanced_bar_);
    // Every direction starts shown, so the table is unfiltered at first.
    rx_->setChecked(true);
    tx_->setChecked(true);
    drop_->setChecked(true);
    search_ = make_search_field(tr("Search payload"), advanced_bar_);
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
    advanced_follow_ = new QCheckBox(tr("Follow"), advanced_bar_);
    advanced_follow_->setChecked(true);
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
    advanced_layout->addWidget(advanced_follow_);
    advanced_layout->addWidget(export_);
    advanced_layout->addWidget(export_capture_);
    advanced_bar_->setVisible(false);
    page_layout->addWidget(advanced_bar_);

    // The traffic table. The first four columns are as wide as their headings
    // and contents ask (or as the user made them) and the last one takes the
    // remaining space. Every row has one height, so the view finds a row by
    // division and never measures one.
    model_ = new MonitorModel(this);
    // Rows leave the front of a full history in every batch. These two are
    // connected before the view and its selection model (which `setModel`
    // makes) so the first is told first, and the second runs after them
    // because it is connected after.
    connect(model_, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex&, int first, int last)
            { rows_leaving(first, last); });
    table_ = new TrafficTable(user_input_, monitor_page_);
    table_->setObjectName(QStringLiteral("monitorTable"));
    table_->setModel(model_);
    connect(model_, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex&, int first, int last)
            { keep_view_after_rows_left(first, last); });
    table_->setAccessibleName(tr("Traffic"));
    keep_left_to_right(table_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    QHeaderView* header = table_->horizontalHeader();
    header->setObjectName(QStringLiteral("monitorColumns"));
    header->setAccessibleName(tr("Traffic table columns"));
    header->setHighlightSections(false);
    header->setSectionsMovable(false);
    header->setMinimumSectionSize(kMinimumColumnWidth);
    header->setStretchLastSection(true);
    for (int column = 0; column < 4; ++column)
        header->setSectionResizeMode(column, QHeaderView::Interactive);
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    measure_model_ = new QStandardItemModel(1, 5, this);
    cell_delegate_ = new MonitorCellDelegate(4, table_);
    cell_delegate_->set_style(&cell_style_);
    table_->setItemDelegate(cell_delegate_);
    table_->setItemDelegateForColumn(
        2, new DirectionDelegate(&cell_style_, table_));
    table_->installEventFilter(this);
    header->installEventFilter(this);
    restore_column_widths();
    apply_metrics();
    column_save_timer_ = new QTimer(this);
    column_save_timer_->setSingleShot(true);
    column_save_timer_->setInterval(400);
    connect(column_save_timer_, &QTimer::timeout, this,
            [this] { save_column_widths(); });
    connect(header, &QHeaderView::sectionResized, this,
            [this](int column, int, int width)
            {
                if (applying_widths_ || column < 0 || column >= 4)
                    return;
                user_widths_[static_cast<std::size_t>(column)] =
                    static_cast<double>(width) / zero_width();
                column_save_timer_->start();
            });
    // A double click on a divider fits the column to its heading and contents.
    connect(header, &QHeaderView::sectionHandleDoubleClicked, this,
            [this](int column) { fit_column_by_key(column); });
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
    keep_left_to_right(log_);
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
    connect(advanced_follow_, &QCheckBox::toggled, this,
            [this](bool on) { set_follow(on); });
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
            [this](bool on) { set_follow(on); });
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
                // Choosing a row that is not the newest is leaving the end,
                // also when it was the current row already.
                if (index.row() != model_->rowCount() - 1)
                    set_follow(false);
                const auto id = model_->port_at(index.row());
                if (id != 0)
                    Q_EMIT port_selected(id);
            });
    // Follow is the user's: selecting a row that is not the newest, from the
    // mouse or the keyboard, or scrolling away from the end, turns it off, and
    // scrolling back to the end turns it on. The scroll bar's action signal
    // comes only from the user (the wheel, the arrows, a click in the groove,
    // a drag of the handle) and never from code that sets the value, so what
    // Follow itself does to the view is never taken for the user. A change of
    // the current row is the user's only while the table handles a key or the
    // mouse; Qt's own (the first row becomes current when focus comes into the
    // table) is not.
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& current, const QModelIndex&)
            {
                if (!user_input_ || rows_leaving_ || !current.isValid())
                    return;
                if (current.row() != model_->rowCount() - 1)
                    set_follow(false);
            });
    connect(table_->verticalScrollBar(), &QAbstractSlider::actionTriggered,
            this,
            [this](int)
            {
                const QScrollBar* bar = table_->verticalScrollBar();
                set_follow(bar->sliderPosition() >= bar->maximum());
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
    else if ((watched == table_ ||
              (table_ != nullptr && watched == table_->horizontalHeader())) &&
             type == QEvent::FontChange)
    {
        // Text made larger on the table or its header, as the system's text
        // size does through the theme.
        apply_metrics();
    }
    else if (table_ != nullptr && watched == table_ &&
             type == QEvent::KeyPress)
    {
        // Ctrl+Alt+Left and Right narrow and widen the column the cursor is in
        // (Left and Right move it); Ctrl+Alt+0 fits it to its heading and
        // contents. Preview takes the rest, so it has no width to change.
        auto* key = static_cast<QKeyEvent*>(event);
        const int column = std::max(table_->currentIndex().column(), 0);
        // Follow: Up and Page Up leave the end; Home and Ctrl+Home go to the
        // first row and leave it too; End and Ctrl+End go to the newest row
        // and follow again; Down and Page Down are the table's own, and
        // Follow is on again when they reach the end. The table's own Home
        // is "first column", so Home is taken here, as End is.
        switch (key->key())
        {
        case Qt::Key_Up:
        case Qt::Key_PageUp:
            set_follow(false);
            break;
        case Qt::Key_Home:
            set_follow(false);
            if (model_->rowCount() > 0)
                table_->setCurrentIndex(model_->index(0, column));
            return true;
        case Qt::Key_End:
            if (model_->rowCount() > 0)
            {
                set_follow(true);
                table_->setCurrentIndex(
                    model_->index(model_->rowCount() - 1, column));
            }
            return true;
        case Qt::Key_Down:
        case Qt::Key_PageDown:
            if (!passing_key_)
            {
                passing_key_ = true;
                QApplication::sendEvent(table_, event);
                passing_key_ = false;
                if (view_is_at_end())
                    set_follow(true);
                return true;
            }
            break;
        default:
            break;
        }
        if (key->modifiers() == (Qt::ControlModifier | Qt::AltModifier) &&
            column < 4)
        {
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

void MonitorPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::LanguageChange)
    {
        if (event->type() == QEvent::LanguageChange && model_ != nullptr)
            Q_EMIT model_->headerDataChanged(Qt::Horizontal, 0, 4);
        apply_metrics();
    }
}

void MonitorPanel::apply_metrics()
{
    if (table_ == nullptr || applying_metrics_)
        return;
    applying_metrics_ = true;
    const Theme& theme = Theme::instance();
    const bool compact = theme.density() == density_mode::compact;

    // D1: the cells' fonts are the theme's, multiplied by how much larger the
    // table's text is than the body text.
    const double body = theme.font(text_style::body).pointSizeF();
    const double own = table_->font().pointSizeF();
    const double scale = body > 0.0 && own > 0.0 ? own / body : 1.0;
    // D5: space at the sides of a cell.
    cell_style_.update(scale, compact ? theme.space(0) : theme.space(1));
    // The cell padding comes from the style sheet, which picks it by this
    // property.
    if (!table_->property("compact").isValid() ||
        table_->property("compact").toBool() != compact)
    {
        table_->setProperty("compact", compact);
        polish(table_);
    }

    // D2: the taller of the two cell fonts, and a little above and below.
    const int text = std::max(QFontMetrics(cell_style_.data).height(),
                              QFontMetrics(cell_style_.caption).height());
    const int row = dense_row_height(text);
    // The header's own least size depends on its font and would keep a row
    // taller than the rule gives.
    table_->verticalHeader()->setMinimumSectionSize(row);
    table_->verticalHeader()->setDefaultSectionSize(row);

    // D3: the header is its text and a little above and below, and at least a
    // target.
    QHeaderView* header = table_->horizontalHeader();
    header->setFixedHeight(dense_header_height(header->font()));

    apply_column_widths();
    table_->viewport()->update();
    applying_metrics_ = false;
    // Polishing the table can change its font (the style sheet puts back the
    // font a widget had before it was styled), and a change that came while
    // this ran was not acted on: look again with the font the table has now.
    const double now = table_->font().pointSizeF();
    if (now < own || own < now)
        apply_metrics();
}

double MonitorPanel::zero_width() const
{
    const double width =
        QFontMetricsF(cell_style_.data).horizontalAdvance(QLatin1Char('0'));
    return width > 0.0 ? width : 1.0;
}

void MonitorPanel::apply_column_widths()
{
    if (table_ == nullptr)
        return;
    table_->ensurePolished();
    QHeaderView* header = table_->horizontalHeader();
    header->ensurePolished();
    const QFontMetrics caption_metrics(cell_style_.caption);

    // D7: what each column has to show. Each is asked of the column's own
    // delegate, which knows the font, the padding and the room the style keeps
    // beside the text, so the cell never has to cut the text.
    QString longest_port;
    for (const auto& port : backend_.ports())
    {
        const QString name = port.name.left(kLongestPortName);
        if (caption_metrics.horizontalAdvance(name) >
            caption_metrics.horizontalAdvance(longest_port))
            longest_port = name;
    }
    QString widest_tag = MonitorModel::tr("RX");
    for (const QString& word :
         {MonitorModel::tr("TX"), MonitorModel::tr("DROP")})
    {
        if (cell_style_.tag_size(word).width() >
            cell_style_.tag_size(widest_tag).width())
            widest_tag = word;
    }
    const std::array<int, 4> content = {
        cell_width(0, QStringLiteral("00:00:00.000")),
        cell_width(1, longest_port), cell_width(2, widest_tag),
        cell_width(3, QLocale().toString(9999999))};

    applying_widths_ = true;
    for (int column = 0; column < 4; ++column)
    {
        const auto index = static_cast<std::size_t>(column);
        // The heading always fits, in the font and language in use.
        const int heading = header->sectionSizeHint(column);
        int width = std::max({heading, kMinimumColumnWidth, content[index]});
        if (user_widths_[index] > 0.0)
        {
            width = std::max({static_cast<int>(std::lround(user_widths_[index] *
                                                           zero_width())),
                              heading, kMinimumColumnWidth});
        }
        header->resizeSection(column, width);
    }
    applying_widths_ = false;
}

int MonitorPanel::cell_width(int column, const QString& sample)
{
    measure_model_->setItem(0, column, new QStandardItem(sample));
    QStyleOptionViewItem option;
    option.initFrom(table_);
    option.widget = table_;
    QAbstractItemDelegate* delegate = table_->itemDelegateForColumn(column);
    if (delegate == nullptr)
        delegate = table_->itemDelegate();
    return delegate->sizeHint(option, measure_model_->index(0, column)).width();
}

void MonitorPanel::restore_column_widths()
{
    const QStringList saved =
        QSettings()
            .value(QStringLiteral("ui/monitorColumnWidths"))
            .toStringList();
    if (saved.size() != static_cast<qsizetype>(user_widths_.size()))
        return;
    for (std::size_t column = 0; column < user_widths_.size(); ++column)
    {
        bool ok = false;
        const double width =
            saved.at(static_cast<qsizetype>(column)).toDouble(&ok);
        if (ok && std::isfinite(width) && width > 0.0 && width <= 400.0)
            user_widths_[column] = width;
    }
}

void MonitorPanel::save_column_widths() const
{
    QStringList widths;
    for (const double width : user_widths_)
        widths.push_back(QString::number(width, 'f', 2));
    QSettings().setValue(QStringLiteral("ui/monitorColumnWidths"), widths);
}

void MonitorPanel::report_column_width(int column)
{
    Q_EMIT status_message(
        tr("%1 column is %2 px wide.")
            .arg(model_->headerData(column, Qt::Horizontal).toString())
            .arg(table_->horizontalHeader()->sectionSize(column)));
}

void MonitorPanel::resize_column_by_key(int column, int delta)
{
    QHeaderView* header = table_->horizontalHeader();
    header->resizeSection(
        column,
        std::max(kMinimumColumnWidth, header->sectionSize(column) + delta));
    report_column_width(column);
}

void MonitorPanel::fit_column_by_key(int column)
{
    if (column < 0 || column >= 4)
        return;
    user_widths_[static_cast<std::size_t>(column)] = 0.0;
    apply_column_widths();
    column_save_timer_->start();
    report_column_width(column);
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

bool MonitorPanel::show_only_port(vss::endpoint_id id)
{
    model_->set_selected_port(id);
    if (!advanced_ || port_filter_ == nullptr)
        return false;
    // The list of ports may be a moment behind a port just made.
    refresh_ports();
    const int index =
        port_filter_->findData(QVariant::fromValue<qulonglong>(id));
    if (index < 0)
        return false;
    port_filter_->setCurrentIndex(index);
    return true;
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

    refreshing_ports_ = true;
    if (port_filter_ != nullptr)
    {
        std::vector<std::pair<QString, QVariant>> choices;
        for (const auto& port : backend_.ports())
        {
            choices.emplace_back(port.name,
                                 QVariant::fromValue<qulonglong>(port.id));
        }
        // The choice stays when its port is still there.
        fill_filter_choices(port_filter_, tr("All ports"),
                            QVariant::fromValue<qulonglong>(0), choices);
    }
    refreshing_ports_ = false;
    if (all_ports_ != nullptr)
        all_ports_->setText(tr("All ports"));
    // The Port column is as wide as the longest name.
    apply_column_widths();
    update_filters();
}

void MonitorPanel::handle_events(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.monitor.handle_events");
    model_->queue_events(events);
    for (const gui_event& event : events)
    {
        if (event.kind == vss::event_kind::data)
            continue;
        append_log(event);
    }
    if (!model_->has_queued())
        return;
    // The table takes new rows at most once every `kCommitIntervalMs`. Under
    // heavy traffic batches arrive every few milliseconds, and each one put
    // in the table at once is a layout, a scroll to the end and a repaint of
    // rows that are gone again before anyone could read them. Traffic that
    // arrives slower than that is shown as it arrives.
    schedule_commit();
}

void MonitorPanel::schedule_commit()
{
    if (!model_->has_queued())
        return;
    const qint64 since =
        last_commit_.isValid() ? last_commit_.elapsed() : commit_interval_ms_;
    if (since >= commit_interval_ms_)
        commit_rows();
    else
        commit_timer_->start(static_cast<int>(commit_interval_ms_ - since));
}

void MonitorPanel::set_commit_interval(int milliseconds)
{
    commit_interval_ms_ = std::max(milliseconds, 0);
    commit_timer_->stop();
    schedule_commit();
}

void MonitorPanel::commit_rows()
{
    commit_timer_->stop();
    if (!model_->has_queued())
        return;
    model_->commit_queued();
    last_commit_.start();
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
    // One port chosen, with no search: say what the port is doing, so that a
    // port that is connecting or has failed shows why nothing is here.
    const vss::endpoint_id chosen =
        advanced_ && port_filter_ != nullptr && port_filter_->currentIndex() >= 0
            ? static_cast<vss::endpoint_id>(
                  port_filter_->currentData().toULongLong())
            : 0;
    const auto chosen_port = chosen != 0 && search_->text().isEmpty()
                                 ? backend_.port(chosen)
                                 : std::optional<port_view>{};
    if (chosen_port)
    {
        QString text = tr("No traffic on %1 yet. %2.")
                           .arg(chosen_port->name,
                                PortModel::state_text(*chosen_port));
        const QString why = PortModel::status_text(*chosen_port);
        if (!why.isEmpty())
            text += QLatin1Char(' ') + why;
        empty_->setText(text);
    }
    else if (model_->total_rows() > 0)
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
    // What arrived before the click is shown; nothing after it is.
    commit_rows();
    model_->set_paused(!model_->paused());
    update_actions();
}

void MonitorPanel::export_visible()
{
    commit_rows();
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
    commit_rows();
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
                           // The user may have moved away since the rows came.
                           if (follow_enabled_ && model_->rowCount() > 0)
                               table_->scrollToBottom();
                       });
}

void MonitorPanel::set_follow(bool on)
{
    follow_enabled_ = on;
    for (QCheckBox* box : {follow_, advanced_follow_})
    {
        if (box == nullptr || box->isChecked() == on)
            continue;
        const QSignalBlocker blocker(box);
        box->setChecked(on);
    }
    if (on && model_->rowCount() > 0)
        table_->scrollToBottom();
}

bool MonitorPanel::view_is_at_end() const
{
    const QScrollBar* bar = table_->verticalScrollBar();
    return bar->value() >= bar->maximum();
}

void MonitorPanel::rows_leaving(int first, int last)
{
    rows_leaving_ = true;
    value_before_leaving_ = table_->verticalScrollBar()->value();
    const int current = table_->currentIndex().row();
    selected_row_leaving_ =
        table_->currentIndex().isValid() && current >= first && current <= last;
}

void MonitorPanel::keep_view_after_rows_left(int first, int last)
{
    rows_leaving_ = false;
    // The row the user chose is gone. The selection model would move the
    // selection to the row that is now first, which the user did not choose;
    // nothing is selected instead.
    if (selected_row_leaving_)
    {
        selected_row_leaving_ = false;
        table_->selectionModel()->clear();
    }
    // Following shows the newest rows, wherever the oldest are.
    if (follow_enabled_)
        return;
    // The scroll position is a row (or a number of pixels), so with rows gone
    // from the front the same position shows rows that are newer. Move it by
    // what left above the first row shown, so the same rows stay where they
    // were. If the first row shown is itself among those that left, the view
    // stays at the top of what is left.
    const int count = last - first + 1;
    const bool per_pixel =
        table_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel;
    const int unit =
        per_pixel ? std::max(table_->verticalHeader()->defaultSectionSize(), 1)
                  : 1;
    const int top = value_before_leaving_ / unit;
    int value = value_before_leaving_;
    if (top > last)
        value -= count * unit;
    else if (top >= first)
        value = first * unit;
    QScrollBar* bar = table_->verticalScrollBar();
    if (value != bar->value())
        bar->setValue(value);
}

} // namespace vss::gui
