//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/event_log.hpp"

#include "advanced/chrome.hpp"
#include "theme/dense_table.hpp"
#include "theme/scaled_font.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/filter_controls.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

constexpr int kRowCap = 2000;

/// Item role: the row is an error or a bus fault.
constexpr int kProblemRole = Qt::UserRole + 1;

/// Item role: the kind the filters see.
constexpr int kCategoryRole = Qt::UserRole + 2;

/// The kinds the check boxes switch. A bus fault is an error to the person.
enum category : int
{
    category_error,
    category_started,
    category_stopped,
    category_other
};

int category_of(vss::event_kind kind)
{
    switch (kind)
    {
    case vss::event_kind::error:
    case vss::event_kind::bus_fault:
        return category_error;
    case vss::event_kind::started:
        return category_started;
    case vss::event_kind::stopped:
        return category_stopped;
    default:
        return category_other;
    }
}

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

/// The cells of the Log. The time is in the data font, as in the Monitor, and
/// the rest in the caption font, both from the theme times the table's own
/// scale. An error or bus fault row is in the bad colour and its Event word is
/// semi-bold, so the row is not told apart by colour alone.
class LogCellDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void set_scale(double scale) noexcept { scale_ = scale; }

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const Theme& theme = Theme::instance();
        option->font = scaled_font(
            theme.font(index.column() == 0 ? text_style::data
                                           : text_style::caption),
            scale_);
        if (index.data(kProblemRole).toBool())
        {
            option->palette.setColor(QPalette::Text,
                                     theme.colour(colour_role::bad));
            if (index.column() == 2)
                option->font.setWeight(QFont::DemiBold);
        }
    }

private:
    double scale_ = 1.0;
};

} // namespace

EventLogPanel::EventLogPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Clear. The shell moves it onto the pane's title row; on its own the
    // panel keeps it in a slim bar, so it is never lost.
    const Theme& theme = Theme::instance();
    bar_ = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar_);
    bar_layout->setContentsMargins(theme.space(2), theme.space(0) / 2,
                                   theme.space(2), theme.space(0) / 2);
    bar_layout->setSpacing(theme.space(1));
    port_filter_ = make_port_filter(tr("Filter by port"), bar_);
    port_filter_->addItem(tr("All ports"), QString());
    errors_ = new QCheckBox(tr("Errors"), bar_);
    started_ = new QCheckBox(tr("Started"), bar_);
    stopped_ = new QCheckBox(tr("Stopped"), bar_);
    other_ = new QCheckBox(tr("Other"), bar_);
    for (QCheckBox* box : {errors_, started_, stopped_, other_})
    {
        // Every kind starts shown, so the log is unfiltered at first.
        box->setChecked(true);
        box->setAccessibleName(tr("Show %1 events").arg(box->text()));
        connect(box, &QCheckBox::toggled, this,
                [this](bool) { apply_filters(); });
    }
    search_ = make_search_field(tr("Search events"), bar_);
    connect(port_filter_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { apply_filters(); });
    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString&) { apply_filters(); });
    bar_layout->addWidget(port_filter_);
    for (QCheckBox* box : {errors_, started_, stopped_, other_})
        bar_layout->addWidget(box);
    bar_layout->addWidget(search_);
    bar_layout->addStretch(1);
    bar_->installEventFilter(this);
    clear_ = make_button(tr("Clear"), nullptr, bar_);
    clear_->setProperty("size", QStringLiteral("small"));
    polish(clear_);
    clear_->setAccessibleName(tr("Clear the event log"));
    connect(clear_, &QPushButton::clicked, this, &EventLogPanel::clear);
    bar_layout->addWidget(clear_);
    layout->addWidget(bar_);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("logTable"));
    table_->setAccessibleName(tr("Event log"));
    table_->setAccessibleDescription(
        tr("State changes and errors since the setup was opened."));
    table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Port"), tr("Event"), tr("Detail")});
    table_->setItemDelegate(new LogCellDelegate(table_));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    QHeaderView* header = table_->horizontalHeader();
    header->setObjectName(QStringLiteral("logColumns"));
    header->setHighlightSections(false);
    header->setSectionsMovable(false);
    header->setStretchLastSection(true);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(table_, 1);

    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this,
            [this]
            {
                search_->setFocus();
                search_->selectAll();
            });

    empty_ = new QLabel(
        tr("No events yet. Start or stop a port, or apply a change."), this);
    empty_->setFont(theme.font(text_style::caption));
    empty_->setProperty("muted", true);
    empty_->setContentsMargins(theme.space(3), 4, theme.space(3), 8);
    layout->addWidget(empty_);

    // The density is the user's choice in View, Density and Settings, and
    // reaches the table through the theme.
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_metrics(); });
    apply_metrics();
    update_empty();
}

void EventLogPanel::attach_header(PaneHeader* header)
{
    if (header_ != nullptr)
        header_->removeEventFilter(this);
    header_ = header;
    if (header_ == nullptr)
        return;
    header_->installEventFilter(this);
    header_->title_label()->setToolTip(
        tr("State changes and errors since the setup was opened."));
    place_controls();
}

bool EventLogPanel::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (watched == header_ &&
        (type == QEvent::Resize || type == QEvent::Show ||
         type == QEvent::FontChange || type == QEvent::StyleChange))
    {
        place_controls();
    }
    else if (watched == bar_ &&
             (type == QEvent::FontChange || type == QEvent::StyleChange))
    {
        // The controls got wider or narrower: the same test decides again.
        QTimer::singleShot(0, this, [this] { place_controls(); });
    }
    return QWidget::eventFilter(watched, event);
}

void EventLogPanel::place_controls()
{
    if (header_ == nullptr)
        return;
    // As in the Monitor: on the title row when it is wide enough, otherwise on
    // a row of their own under it, so no control is cut off.
    const bool want_header =
        header_->isVisible() &&
        header_->width() >= header_->lead_width() + bar_->sizeHint().width();
    if (want_header == in_header_)
        return;
    in_header_ = want_header;
    if (want_header)
    {
        layout()->removeWidget(bar_);
        header_->set_fill(bar_);
    }
    else
    {
        header_->set_fill(nullptr);
        static_cast<QVBoxLayout*>(layout())->insertWidget(0, bar_);
    }
    bar_->show();
}

void EventLogPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::LanguageChange)
        apply_metrics();
}

void EventLogPanel::apply_metrics()
{
    if (table_ == nullptr || applying_)
        return;
    applying_ = true;
    const Theme& theme = Theme::instance();
    const bool compact = theme.density() == density_mode::compact;

    // The cells' fonts are the theme's times how much larger the table's text
    // is than the body text, as in the Monitor.
    const double body = theme.font(text_style::body).pointSizeF();
    const double own = font().pointSizeF();
    const double scale = body > 0.0 && own > 0.0 ? own / body : 1.0;
    static_cast<LogCellDelegate*>(table_->itemDelegate())->set_scale(scale);
    const QFont data_font = scaled_font(theme.font(text_style::data), scale);
    const QFont caption = scaled_font(theme.font(text_style::caption), scale);
    QFont heavy = caption;
    heavy.setWeight(QFont::DemiBold);

    // The padding comes from the style sheet, which picks it by this property.
    if (!table_->property("compact").isValid() ||
        table_->property("compact").toBool() != compact)
    {
        table_->setProperty("compact", compact);
        polish(table_);
    }

    const int row = dense_row_height(std::max(QFontMetrics(data_font).height(),
                                              QFontMetrics(caption).height()));
    table_->verticalHeader()->setMinimumSectionSize(row);
    table_->verticalHeader()->setDefaultSectionSize(row);
    QHeaderView* header = table_->horizontalHeader();
    header->setFixedHeight(dense_header_height(header->font()));

    // Time, Port and Event are as wide as their heading or content, whichever
    // is wider, as the style draws them (padding and text margin included, so
    // no cell is cut); Detail takes the rest.
    table_->ensurePolished();
    header->ensurePolished();
    const auto cell = [this](const QFont& cell_font, const QString& text)
    {
        QStyleOptionViewItem option;
        option.initFrom(table_);
        option.widget = table_;
        option.font = cell_font;
        option.fontMetrics = QFontMetrics(cell_font);
        option.text = text;
        option.features = QStyleOptionViewItem::HasDisplay;
        return table_->style()
            ->sizeFromContents(QStyle::CT_ItemViewItem, &option, QSize(),
                               table_)
            .width();
    };
    int port = cell(caption, QStringLiteral("0000000000"));
    for (int r = 0; r < table_->rowCount(); ++r)
    {
        if (const auto* item = table_->item(r, 1))
            port = std::max(port, cell(caption, item->text().left(24)));
    }
    int event = 0;
    for (const QString& word :
         {tr("Started"), tr("Stopped"), tr("Control lines"), tr("Error"),
          tr("Counters"), tr("Message"), tr("Traffic"), tr("Bus"),
          tr("Studio")})
        event = std::max(event, cell(heavy, word));
    const int content[3] = {cell(data_font, QStringLiteral("00:00:00.000")),
                            port, event};
    for (int column = 0; column < 3; ++column)
    {
        header->resizeSection(
            column, std::max(header->sectionSizeHint(column), content[column]));
    }

    table_->viewport()->update();
    applying_ = false;
}

void EventLogPanel::append(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.advanced.event_log.append");
    const QString now =
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    bool wider_port = false;
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
        add_row(now, event.name, kind_text(event.kind), detail,
                category_of(event.kind));
        wider_port = wider_port || !event.name.isEmpty();
    }
    // A longer port name than any before widens the Port column.
    if (wider_port)
        apply_metrics();
    update_empty();
}

void EventLogPanel::add_local(const QString& text)
{
    add_row(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), {},
            tr("Studio"), text, category_other);
    update_empty();
}

void EventLogPanel::add_row(const QString& time, const QString& port,
                            const QString& event, const QString& detail,
                            int category)
{
    // New rows keep the view at the end only when it was at the end, so a
    // person reading older rows is not moved, as in the Monitor.
    QScrollBar* scroll = table_->verticalScrollBar();
    const bool at_end = scroll->value() >= scroll->maximum();
    const bool problem = category == category_error;
    while (table_->rowCount() >= kRowCap)
        table_->removeRow(0);
    note_port(port);
    const int row = table_->rowCount();
    table_->insertRow(row);
    const QStringList values = {time, port, event, detail};
    for (int column = 0; column < values.size(); ++column)
    {
        auto* item = new QTableWidgetItem(values.at(column));
        item->setToolTip(values.at(column));
        item->setData(kProblemRole, problem);
        item->setData(kCategoryRole, category);
        table_->setItem(row, column, item);
    }
    table_->setRowHidden(row, !row_shown(row));
    if (at_end)
        table_->scrollToBottom();
}

void EventLogPanel::note_port(const QString& port)
{
    if (port.isEmpty() || ports_.contains(port))
        return;
    ports_.push_back(port);
    ports_.sort(Qt::CaseInsensitive);
    std::vector<std::pair<QString, QVariant>> choices;
    for (const QString& name : ports_)
        choices.emplace_back(name, name);
    fill_filter_choices(port_filter_, tr("All ports"), QString(), choices);
}

bool EventLogPanel::row_shown(int row) const
{
    const QString port = port_filter_->currentData().toString();
    if (!port.isEmpty() && table_->item(row, 1)->text() != port)
        return false;
    const bool shown[] = {errors_->isChecked(), started_->isChecked(),
                          stopped_->isChecked(), other_->isChecked()};
    const int category = table_->item(row, 0)->data(kCategoryRole).toInt();
    if (!shown[category])
        return false;
    const QString search = search_->text();
    if (search.isEmpty())
        return true;
    for (int column = 1; column < 4; ++column)
    {
        if (matches_search(table_->item(row, column)->text(), search))
            return true;
    }
    return false;
}

void EventLogPanel::apply_filters()
{
    if (table_ == nullptr || search_ == nullptr)
        return;
    for (int row = 0; row < table_->rowCount(); ++row)
        table_->setRowHidden(row, !row_shown(row));
    update_empty();
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
    bool any_shown = false;
    for (int row = 0; row < table_->rowCount() && !any_shown; ++row)
        any_shown = !table_->isRowHidden(row);
    empty_->setText(table_->rowCount() == 0
                        ? tr("No events yet. Start or stop a port, or apply a "
                             "change.")
                        : tr("No events match the filters."));
    empty_->setVisible(!any_shown);
}

} // namespace vss::gui
