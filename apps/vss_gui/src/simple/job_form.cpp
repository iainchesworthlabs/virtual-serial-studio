//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/job_form.hpp"

#include "backend/com_numbers.hpp"
#include "backend/driver_error.hpp"
#include "theme/icon_provider.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::simple
{

namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QLabel* group_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    label->setProperty("group", true);
    label->setProperty("muted", true);
    label->setFont(semibold(Theme::instance().font(text_style::caption)));
    polish(label);
    return label;
}

QLabel* hint_label(const QString& text, QWidget* parent)
{
    auto* label = make_label(text, text_style::caption, parent);
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QString and_list(const QStringList& names)
{
    if (names.size() <= 1)
        return names.join(QString());
    return QCoreApplication::translate("vss::gui::simple::JobForm", "%1 and %2")
        .arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")),
             names.back());
}

std::string to_std(const QString& text)
{
    return text.toStdString();
}

} // namespace

// -- WireDiagram -------------------------------------------------------------

WireDiagram::WireDiagram(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("jobPicture"));
    setAccessibleName(tr("Picture of the setup"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void WireDiagram::set_ends(const QStringList& left, const QStringList& right,
                           arrows style)
{
    left_ = left;
    right_ = right;
    style_ = style;
    setAccessibleDescription(right.isEmpty()
                                 ? left.join(QStringLiteral(", "))
                                 : tr("%1 to %2")
                                       .arg(left.join(QStringLiteral(", ")),
                                            right.join(QStringLiteral(", "))));
    updateGeometry();
    update();
}

QSize WireDiagram::sizeHint() const
{
    const int rows = std::max<int>(
        1, static_cast<int>(std::max(left_.size(), right_.size())));
    const int row_height = fontMetrics().height() + 18;
    return {480, rows * (row_height + 8) + 24};
}

QSize WireDiagram::minimumSizeHint() const
{
    return {240, sizeHint().height()};
}

void WireDiagram::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Theme& theme = Theme::instance();
    const QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(theme.colour(colour_role::line), 1));
    painter.setBrush(theme.colour(colour_role::sunken));
    painter.drawRoundedRect(area, 12, 12);

    const int row_height = fontMetrics().height() + 18;
    const qreal box_width =
        right_.isEmpty() ? area.width() - 32 : (area.width() - 32) * 0.38;
    const auto column_rects = [&](const QStringList& names, qreal x)
    {
        QVector<QRectF> rects;
        const qreal total =
            static_cast<qreal>(names.size()) * (row_height + 8) - 8;
        qreal y = area.center().y() - total / 2;
        for (int index = 0; index < names.size(); ++index)
        {
            rects.push_back(QRectF(x, y, box_width, row_height));
            y += row_height + 8;
        }
        return rects;
    };
    const auto left = column_rects(left_, area.left() + 16);
    const auto right = column_rects(right_, area.right() - 16 - box_width);

    // Lines first, so the boxes sit on top of them.
    const QColor accent = theme.colour(colour_role::accent);
    if (style_ == arrows::line && left.size() > 1)
    {
        // A shared line: a bar the members hang from.
        const qreal bar_x = area.left() + 16 + box_width + 24;
        painter.setPen(QPen(accent, 3));
        painter.drawLine(QPointF(bar_x, left.front().center().y()),
                         QPointF(bar_x, left.back().center().y()));
        painter.setPen(QPen(accent, 2));
        for (const auto& box : left)
            painter.drawLine(QPointF(box.right(), box.center().y()),
                             QPointF(bar_x, box.center().y()));
    }
    const auto arrow_head = [&painter, &accent](QPointF tip, qreal dx)
    {
        QPainterPath head;
        head.moveTo(tip);
        head.lineTo(tip + QPointF(-8 * dx, -5));
        head.lineTo(tip + QPointF(-8 * dx, 5));
        head.closeSubpath();
        painter.fillPath(head, accent);
    };
    for (int a = 0; a < left.size(); ++a)
    {
        for (int b = 0; b < right.size(); ++b)
        {
            if (left.size() > 1 && right.size() > 1)
                continue;
            const QPointF from(left[a].right(), left[a].center().y());
            const QPointF to(right[b].left(), right[b].center().y());
            QPainterPath path(from);
            const qreal mid = (from.x() + to.x()) / 2;
            path.cubicTo(QPointF(mid, from.y()), QPointF(mid, to.y()), to);
            QPen pen(accent, 2);
            if (style_ == arrows::turns && a > 0)
            {
                pen.setStyle(Qt::DashLine);
                pen.setColor(theme.colour(colour_role::ink3));
            }
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            arrow_head(to, 1);
            if (style_ == arrows::both)
                arrow_head(from, -1);
        }
    }

    const auto draw_boxes =
        [&](const QVector<QRectF>& rects, const QStringList& names)
    {
        for (int index = 0; index < rects.size(); ++index)
        {
            painter.setPen(QPen(theme.colour(colour_role::line2), 1));
            painter.setBrush(theme.colour(colour_role::panel));
            painter.drawRoundedRect(rects[index], 8, 8);
            painter.setPen(theme.colour(colour_role::ink));
            painter.drawText(rects[index].adjusted(10, 0, -10, 0),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             fontMetrics().elidedText(
                                 names[index], Qt::ElideRight,
                                 static_cast<int>(rects[index].width() - 20)));
        }
    };
    draw_boxes(left, left_);
    draw_boxes(right, right_);
}

// -- JobForm ----------------------------------------------------------------

JobForm::JobForm(StudioBackend& backend, job kind,
                 const std::optional<setup_view>& previous,
                 const setup_prefs& prefs, QWidget* parent)
    : QWidget(parent), backend_(backend), kind_(kind), previous_(previous),
      prefs_(prefs)
{
    setObjectName(QStringLiteral("simpleJobForm"));
    if (previous_)
    {
        plan_ = plan_from(*previous_, backend_.ports());
        for (const auto* ends : {&plan_.first, &plan_.second})
        {
            for (const auto& end : *ends)
            {
                if (end.existing != 0)
                    original_labels_.insert(end.existing, end.label);
            }
        }
    }
    else
    {
        plan_.kind = kind_;
        if (const auto first = spec(false))
        {
            for (int index = 0; index < first->min; ++index)
                plan_.first.push_back(first->device ? new_device()
                                                    : new_program());
        }
        if (const auto second = spec(true))
        {
            for (int index = 0; index < second->min; ++index)
                plan_.second.push_back(second->device ? new_device()
                                                      : new_program());
        }
    }
    build();
    update_all();
}

std::optional<JobForm::list_spec> JobForm::spec(bool second) const
{
    switch (kind_)
    {
    case job::port:
        if (!second)
            return list_spec{tr("Your program"), false, 1, 1, false};
        break;
    case job::pair:
        return list_spec{second ? tr("Second program") : tr("First program"),
                         false, 1, 1, false};
    case job::attach:
        return second ? list_spec{tr("Program"), false, 1, 1, false}
                      : list_spec{tr("Device"), true, 1, 1, false};
    case job::share:
        return second ? list_spec{tr("Programs"), false, 2, 32, false}
                      : list_spec{tr("Device"), true, 1, 1, false};
    case job::talk:
    case job::network:
        if (!second)
            return list_spec{tr("Device"), true, 1, 1, false};
        break;
    case job::bus:
        if (!second)
            return list_spec{tr("On the line"), false, 2, 64, true};
        break;
    case job::combine:
        return second ? list_spec{tr("Receiver"), false, 1, 1, true}
                      : list_spec{tr("Senders"), false, 2, 32, true};
    case job::turns:
        return second ? list_spec{tr("Device"), true, 1, 1, false}
                      : list_spec{tr("Programs"), false, 2, 32, false};
    case job::custom:
        break;
    }
    return std::nullopt;
}

std::vector<end_plan>& JobForm::list(bool second)
{
    return second ? plan_.second : plan_.first;
}

const std::vector<end_plan>& JobForm::list(bool second) const
{
    return second ? plan_.second : plan_.first;
}

bool JobForm::com_numbers_shown() const
{
    return backend_.caps().com_numbers;
}

std::vector<unsigned> JobForm::free_com_numbers(const end_plan* keep) const
{
    std::vector<std::string> paths;
    for (const auto& port : backend_.ports())
        paths.push_back(to_std(port.path));
    for (const auto& device : backend_.devices())
        paths.push_back(to_std(device.path));
    std::vector<unsigned> in_use = com_numbers_in_use(paths);
    for (const auto* ends : {&plan_.first, &plan_.second})
    {
        for (const auto& end : *ends)
        {
            if (&end != keep && end.com_number != 0)
                in_use.push_back(end.com_number);
        }
    }
    std::vector<unsigned> free;
    for (unsigned number = kFirstGuiComNumber;
         number <= kLastGuiComNumber && free.size() < 8; ++number)
    {
        if (std::find(in_use.begin(), in_use.end(), number) == in_use.end())
            free.push_back(number);
    }
    return free;
}

QStringList JobForm::free_devices() const
{
    QSet<QString> held;
    for (const auto& port : backend_.ports())
    {
        if (port.kind == vss::endpoint_kind::physical)
            held.insert(port_shown_name(port));
    }
    if (previous_)
    {
        for (const auto id : previous_->group.ports)
        {
            if (const auto port = backend_.port(id))
                held.remove(port_shown_name(*port));
        }
    }
    QStringList free;
    for (const auto& device : backend_.devices())
    {
        if (device.virtual_device || device.com0com || device.in_use ||
            held.contains(device.path))
            continue;
        bool chosen = false;
        for (const auto* ends : {&plan_.first, &plan_.second})
        {
            for (const auto& end : *ends)
            {
                if (end.what == end_plan::kind::device &&
                    end.device_path == device.path)
                    chosen = true;
            }
        }
        if (!chosen)
            free << device.path;
    }
    return free;
}

end_plan JobForm::new_program()
{
    end_plan end;
    end.what = end_plan::kind::program;
    if (com_numbers_shown())
    {
        const auto free = free_com_numbers(nullptr);
        if (!free.empty())
            end.com_number = free.front();
    }
    return end;
}

end_plan JobForm::new_device()
{
    end_plan end;
    end.what = end_plan::kind::device;
    end.device_path = free_devices().value(0);
    return end;
}

QString JobForm::end_name(const end_plan& end) const
{
    if (end.existing != 0)
    {
        if (const auto port = backend_.port(end.existing))
        {
            QString shown = port_shown_name(*port);
            if (end.what == end_plan::kind::program && !end.label.isEmpty())
                return tr("%1 (%2)").arg(shown, end.label);
            return shown;
        }
    }
    switch (end.what)
    {
    case end_plan::kind::program:
    {
        const QString port = end.com_number != 0
                                 ? QStringLiteral("COM%1").arg(end.com_number)
                                 : tr("a new port");
        return end.label.isEmpty() ? port : tr("%1 (%2)").arg(port, end.label);
    }
    case end_plan::kind::device:
        return end.device_path.isEmpty() ? tr("the device") : end.device_path;
    case end_plan::kind::network_device:
        return end.address.isEmpty() ? tr("the network device")
                                     : QStringLiteral("%1:%2")
                                           .arg(end.address)
                                           .arg(end.network_port);
    }
    return {};
}

QStringList JobForm::end_names(const std::vector<end_plan>& ends) const
{
    QStringList names;
    for (const auto& end : ends)
        names << end_name(end);
    return names;
}

void JobForm::build()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.build");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The header stays put while the body scrolls, so Create is always in
    // reach whatever the window height or text size.
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("jobFormHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 16, 24, 12);
    header_layout->setSpacing(12);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(4);
    auto* back = new QPushButton(
        editing() ? tr("Back to the setup") : tr("All jobs"), header);
    set_role(back, "ghost");
    back->setObjectName(QStringLiteral("jobFormBack"));
    connect(back, &QPushButton::clicked, this, [this] { Q_EMIT cancelled(); });
    titles->addWidget(back, 0, Qt::AlignLeft);
    QString heading = job_title(kind_);
    if (previous_)
        heading =
            prefs_.name.isEmpty()
                ? tr("Change %1").arg(setup_title(*previous_, backend_.ports()))
                : tr("Change %1").arg(prefs_.name);
    auto* title = make_label(heading, text_style::title, header);
    title->setObjectName(QStringLiteral("jobFormTitle"));
    titles->addWidget(title);
    titles->addWidget(
        make_label(job_description(kind_), text_style::body, header));
    header_layout->addLayout(titles, 1);
    auto* cancel = new QPushButton(tr("Cancel"), header);
    set_role(cancel, "secondary");
    connect(cancel, &QPushButton::clicked, this,
            [this] { Q_EMIT cancelled(); });
    create_ = new QPushButton(editing() ? tr("Apply") : tr("Create"), header);
    create_->setObjectName(QStringLiteral("jobFormCreate"));
    set_role(create_, "primary");
    create_->setDefault(true);
    connect(create_, &QPushButton::clicked, this, [this] { submit(); });
    header_layout->addWidget(cancel, 0, Qt::AlignVCenter);
    header_layout->addWidget(create_, 0, Qt::AlignVCenter);
    outer->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("jobFormScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll, 1);
    auto* body = new QWidget(scroll);
    scroll->setWidget(body);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(24, 8, 24, 24);
    layout->setSpacing(12);

    blocker_label_ = hint_label({}, body);
    blocker_label_->setObjectName(QStringLiteral("jobFormBlocker"));
    layout->addWidget(blocker_label_);

    diagram_ = new WireDiagram(body);
    layout->addWidget(diagram_);

    ends_ = new QWidget(body);
    ends_layout_ = new QVBoxLayout(ends_);
    ends_layout_->setContentsMargins(0, 0, 0, 0);
    ends_layout_->setSpacing(8);
    layout->addWidget(ends_);

    if (kind_ == job::port)
    {
        layout->addWidget(group_label(tr("On the other end"), body));
        answer_type_ = new PickCard(
            tr("I'll type to it from this window"),
            tr("What your program sends shows here, and what you type goes "
               "to your program. Use it to see that your program sends what "
               "you expect."),
            body);
        answer_echo_ = new PickCard(
            tr("Send everything straight back"),
            tr("Whatever your program sends comes back to it at once. A quick "
               "test that its sending and receiving both work."),
            body);
        auto* group = new QButtonGroup(this);
        group->addButton(answer_type_->radio());
        group->addButton(answer_echo_->radio());
        answer_type_->set_selected(!plan_.echo);
        answer_echo_->set_selected(plan_.echo);
        if (!backend_.caps().fixture_echo)
        {
            answer_echo_->setEnabled(false);
            answer_echo_->setToolTip(tr("Not available in this build"));
        }
        connect(answer_type_, &PickCard::selected, this,
                [this] { set_echo(false); });
        connect(answer_echo_, &PickCard::selected, this,
                [this] { set_echo(true); });
        layout->addWidget(answer_type_);
        layout->addWidget(answer_echo_);
        layout->addWidget(hint_label(
            tr("You can connect it to something else later with Change."),
            body));
    }

    if (kind_ == job::network)
    {
        layout->addWidget(group_label(tr("Other computers connect to"), body));
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(tr("Network port"), body);
        listen_ = new QSpinBox(body);
        listen_->setRange(1, 65535);
        listen_->setValue(static_cast<int>(plan_.listen_port));
        listen_->setAccessibleName(tr("Network port"));
        label->setBuddy(listen_);
        connect(listen_, &QSpinBox::valueChanged, this,
                [this](int value)
                {
                    plan_.listen_port = static_cast<unsigned>(value);
                    update_all();
                });
        row->addWidget(label);
        row->addWidget(listen_);
        row->addStretch(1);
        layout->addLayout(row);
        layout->addWidget(hint_label(
            tr("Other computers connect to this PC's address on this port. "
               "4001 is common for serial over the network."),
            body));
    }

    // Device settings, for jobs with a device.
    device_fields_ = new QWidget(body);
    auto* fields = new QFormLayout(device_fields_);
    fields->setContentsMargins(0, 8, 0, 0);
    baud_ = new QComboBox(device_fields_);
    baud_->setObjectName(QStringLiteral("jobFormBaud"));
    for (const unsigned rate : {1200U, 2400U, 4800U, 9600U, 19200U, 38400U,
                                57600U, 115200U, 230400U, 460800U, 921600U})
        baud_->addItem(QString::number(rate), rate);
    baud_->setCurrentIndex(
        std::max(0, baud_->findData(plan_.serial.baud_rate)));
    connect(baud_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                plan_.serial.baud_rate = baud_->currentData().toUInt();
                update_all();
            });
    fields->addRow(tr("Device speed (baud rate)"), baud_);
    fields->addRow(QString(),
                   hint_label(tr("The device's manual gives it. If text looks "
                                 "garbled later, this is usually why."),
                              device_fields_));

    auto* more = new Disclosure(tr("Other device settings"), device_fields_);
    auto* more_body = new QWidget(more);
    auto* more_layout = new QFormLayout(more_body);
    data_bits_ = new QComboBox(more_body);
    for (const unsigned bits : {8U, 7U, 6U, 5U})
        data_bits_->addItem(QString::number(bits), bits);
    data_bits_->setCurrentIndex(
        std::max(0, data_bits_->findData(plan_.serial.data_bits)));
    parity_ = new QComboBox(more_body);
    parity_->addItem(tr("None"), static_cast<int>(vss::parity_mode::none));
    parity_->addItem(tr("Odd"), static_cast<int>(vss::parity_mode::odd));
    parity_->addItem(tr("Even"), static_cast<int>(vss::parity_mode::even));
    parity_->setCurrentIndex(
        std::max(0, parity_->findData(static_cast<int>(plan_.serial.parity))));
    stop_bits_ = new QComboBox(more_body);
    stop_bits_->addItem(QStringLiteral("1"),
                        static_cast<int>(vss::stop_bits_mode::one));
    stop_bits_->addItem(QStringLiteral("2"),
                        static_cast<int>(vss::stop_bits_mode::two));
    stop_bits_->setCurrentIndex(std::max(
        0, stop_bits_->findData(static_cast<int>(plan_.serial.stop_bits))));
    flow_ = new QComboBox(more_body);
    flow_->addItem(tr("None"), static_cast<int>(vss::flow_control_mode::none));
    flow_->addItem(tr("Hardware (RTS and CTS)"),
                   static_cast<int>(vss::flow_control_mode::hardware));
    flow_->addItem(tr("Software (XON and XOFF)"),
                   static_cast<int>(vss::flow_control_mode::software));
    flow_->setCurrentIndex(std::max(
        0, flow_->findData(static_cast<int>(plan_.serial.flow_control))));
    more_layout->addRow(tr("Data bits"), data_bits_);
    more_layout->addRow(tr("Parity"), parity_);
    more_layout->addRow(tr("Stop bits"), stop_bits_);
    more_layout->addRow(tr("Flow control"), flow_);
    more->set_content(more_body);
    fields->addRow(more);
    const auto settings_changed = [this]
    {
        plan_.serial.data_bits = data_bits_->currentData().toUInt();
        plan_.serial.parity =
            static_cast<vss::parity_mode>(parity_->currentData().toInt());
        plan_.serial.stop_bits =
            static_cast<vss::stop_bits_mode>(stop_bits_->currentData().toInt());
        plan_.serial.flow_control =
            static_cast<vss::flow_control_mode>(flow_->currentData().toInt());
    };
    for (auto* combo : {data_bits_, parity_, stop_bits_, flow_})
        connect(combo, &QComboBox::currentIndexChanged, this, settings_changed);
    layout->addWidget(device_fields_);

    auto* name_row = new QFormLayout();
    name_ = new QLineEdit(prefs_.name, body);
    name_->setObjectName(QStringLiteral("jobFormName"));
    name_->setAccessibleName(tr("Name (optional)"));
    connect(name_, &QLineEdit::textChanged, this,
            [this](const QString& text) { prefs_.name = text.trimmed(); });
    name_row->addRow(tr("Name (optional)"), name_);
    layout->addLayout(name_row);

    // "Also": typing to the device and seeing the data, said plainly.
    const bool has_device_job =
        kind_ == job::attach || kind_ == job::share || kind_ == job::turns;
    const bool offers_data = kind_ != job::port && kind_ != job::talk;
    if (has_device_job || offers_data)
    {
        layout->addWidget(group_label(tr("Also"), body));
        if (has_device_job)
        {
            type_here_ = new QCheckBox(body);
            type_here_->setObjectName(QStringLiteral("jobFormTypeHere"));
            type_here_->setChecked(prefs_.type_here);
            connect(type_here_, &QCheckBox::toggled, this,
                    [this](bool on) { prefs_.type_here = on; });
            layout->addWidget(type_here_);
            layout->addWidget(hint_label(
                tr("Handy for checking the device works before your own "
                   "program is ready. What you type goes to the device, and "
                   "its replies show here and in your programs."),
                body));
        }
        if (offers_data)
        {
            show_data_ =
                new QCheckBox(tr("Show me the data going through"), body);
            show_data_->setObjectName(QStringLiteral("jobFormShowData"));
            show_data_->setChecked(prefs_.show_data);
            connect(show_data_, &QCheckBox::toggled, this,
                    [this](bool on) { prefs_.show_data = on; });
            layout->addWidget(show_data_);
            layout->addWidget(hint_label(
                tr("A live list of what is sent each way, on the setup's "
                   "page. It only looks, so it changes nothing. You can turn "
                   "it on later too."),
                body));
        }
    }

    changes_ = new NoteBanner(NoteBanner::kind::info, {}, body);
    changes_->setObjectName(QStringLiteral("jobFormChanges"));
    changes_->setVisible(editing());
    layout->addWidget(changes_);

    result_ = new ResultSentence(body);
    result_->setObjectName(QStringLiteral("jobFormResult"));
    layout->addWidget(result_);

    error_ = new NoteBanner(NoteBanner::kind::danger, {}, body);
    error_->setObjectName(QStringLiteral("jobFormError"));
    error_->setVisible(false);
    layout->addWidget(error_);
    layout->addStretch(1);

    rebuild_ends();
}

void JobForm::rebuild_ends()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.rebuild_ends");
    QWidget* focused = QApplication::focusWidget();
    const bool focus_inside =
        focused != nullptr && ends_->isAncestorOf(focused);
    while (QLayoutItem* item = ends_layout_->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            // Hidden at once, so the old rows never sit under the new ones
            // while they wait to be deleted.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    build_list(false);
    build_list(true);
    for (int index = 0; index < ends_layout_->count(); ++index)
    {
        if (QWidget* widget = ends_layout_->itemAt(index)->widget())
            widget->show();
    }
    if (focus_inside)
        ends_->setFocus();
}

void JobForm::build_list(bool second)
{
    VSS_PROFILE_ZONE("gui.simple.job_form.build_list");
    const auto list_spec_value = spec(second);
    if (!list_spec_value)
        return;
    auto* box = new QWidget(ends_);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(group_label(list_spec_value->heading, box));
    auto& ends = list(second);
    for (int index = 0; index < static_cast<int>(ends.size()); ++index)
    {
        if (ends[static_cast<std::size_t>(index)].what ==
            end_plan::kind::program)
            layout->addWidget(program_row(second, index));
        else
            layout->addWidget(
                device_row(second, index,
                           list_spec_value->device && kind_ != job::network));
    }
    if (list_spec_value->max > 1)
    {
        auto* adders = new QHBoxLayout();
        adders->setSpacing(8);
        const bool full = static_cast<int>(ends.size()) >= list_spec_value->max;
        auto* add = new QPushButton(tr("Add a program"), box);
        add->setObjectName(QStringLiteral("jobFormAddProgram"));
        add->setIcon(IconProvider::instance().icon(QStringLiteral("add"),
                                                   colour_role::ink));
        set_role(add, "secondary");
        add->setEnabled(!full);
        connect(add, &QPushButton::clicked, this, [this] { add_program(); });
        adders->addWidget(add);
        if (list_spec_value->devices_too)
        {
            auto* add_device_button = new QPushButton(tr("Add a device"), box);
            add_device_button->setIcon(IconProvider::instance().icon(
                QStringLiteral("add"), colour_role::ink));
            set_role(add_device_button, "secondary");
            add_device_button->setEnabled(!full && !free_devices().isEmpty());
            connect(add_device_button, &QPushButton::clicked, this,
                    [this] { add_device(); });
            adders->addWidget(add_device_button);
        }
        adders->addWidget(hint_label(tr("Up to %1. You can add more later.")
                                         .arg(list_spec_value->max),
                                     box),
                          1);
        layout->addLayout(adders);
    }
    ends_layout_->addWidget(box);
}

QWidget* JobForm::program_row(bool second, int index)
{
    auto* row = new QFrame(ends_);
    row->setProperty("group", true);
    polish(row);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);
    auto* line = new QHBoxLayout();
    line->setSpacing(8);
    end_plan& end = list(second)[static_cast<std::size_t>(index)];

    if (end.existing != 0)
    {
        const auto port = backend_.port(end.existing);
        auto* name = new QLabel(port ? port_shown_name(*port) : QString(), row);
        name->setFont(Theme::instance().font(text_style::data));
        line->addWidget(name);
    }
    else if (com_numbers_shown())
    {
        auto* com = new QComboBox(row);
        com->setObjectName(QStringLiteral("jobFormPort"));
        com->setAccessibleName(tr("Port name"));
        for (const unsigned number : free_com_numbers(&end))
            com->addItem(QStringLiteral("COM%1").arg(number), number);
        com->setCurrentIndex(std::max(0, com->findData(end.com_number)));
        connect(com, &QComboBox::activated, this,
                [this, second, index, com]
                {
                    list(second)[static_cast<std::size_t>(index)].com_number =
                        com->currentData().toUInt();
                    rebuild_ends();
                    update_all();
                });
        line->addWidget(com);
    }
    else
    {
        auto* auto_name = new QLabel(tr("New port"), row);
        auto_name->setFont(Theme::instance().font(text_style::data));
        line->addWidget(auto_name);
    }

    auto* label = new QLineEdit(end.label, row);
    label->setObjectName(QStringLiteral("jobFormLabel"));
    label->setPlaceholderText(tr("Program, e.g. PuTTY (optional)"));
    label->setAccessibleName(tr("Which program, optional"));
    auto* completer = new QCompleter(
        QStringList{QStringLiteral("PuTTY"), QStringLiteral("Tera Term"),
                    QStringLiteral("Arduino IDE"), QStringLiteral("RealTerm"),
                    QStringLiteral("HyperTerminal")},
        label);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    label->setCompleter(completer);
    connect(label, &QLineEdit::textEdited, this,
            [this, second, index](const QString& text)
            {
                list(second)[static_cast<std::size_t>(index)].label =
                    text.trimmed();
                update_all();
            });
    line->addWidget(label, 1);

    const auto list_spec_value = spec(second);
    if (list_spec_value &&
        static_cast<int>(list(second).size()) > list_spec_value->min)
    {
        auto* remove = new QPushButton(row);
        remove->setIcon(IconProvider::instance().icon(QStringLiteral("remove"),
                                                      colour_role::ink2));
        remove->setAccessibleName(tr("Remove %1").arg(end_name(end)));
        remove->setToolTip(tr("Remove"));
        set_role(remove, "ghost");
        connect(remove, &QPushButton::clicked, this,
                [this, second, index] { remove_end(second, index); });
        line->addWidget(remove);
    }
    layout->addLayout(line);

    if (end.existing == 0 && com_numbers_shown() && end.com_number > 9)
        layout->addWidget(
            hint_label(tr("Some older programs only list COM1 to COM9."), row));
    if (end.existing == 0 && !com_numbers_shown())
        layout->addWidget(hint_label(
            tr("Studio picks the name, such as /dev/pts/3 or COM7, and shows "
               "it when the port is ready."),
            row));
    return row;
}

QWidget* JobForm::device_row(bool second, int index, bool allow_network)
{
    auto* row = new QFrame(ends_);
    row->setProperty("group", true);
    polish(row);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);
    end_plan& end = list(second)[static_cast<std::size_t>(index)];

    if (allow_network)
    {
        auto* where = new SegmentedControl(row);
        where->setObjectName(QStringLiteral("jobFormWhere"));
        where->setAccessibleName(tr("Where is the device"));
        where->add_item(tr("Plugged into this PC"), 0);
        where->add_item(tr("On the network"), 1);
        where->set_current(end.what == end_plan::kind::network_device ? 1 : 0);
        connect(where, &SegmentedControl::current_changed, this,
                [this, second, index](int id)
                {
                    end_plan& changed =
                        list(second)[static_cast<std::size_t>(index)];
                    changed.existing = 0;
                    changed.what = id == 1 ? end_plan::kind::network_device
                                           : end_plan::kind::device;
                    if (id == 0 && changed.device_path.isEmpty())
                        changed.device_path = free_devices().value(0);
                    QTimer::singleShot(0, this,
                                       [this]
                                       {
                                           rebuild_ends();
                                           update_all();
                                       });
                });
        layout->addWidget(where, 0, Qt::AlignLeft);
    }

    if (end.what == end_plan::kind::network_device)
    {
        auto* line = new QHBoxLayout();
        auto* address = new QLineEdit(end.address, row);
        address->setPlaceholderText(tr("Address, such as 192.168.1.50"));
        address->setAccessibleName(tr("Device address"));
        connect(address, &QLineEdit::textEdited, this,
                [this, second, index](const QString& text)
                {
                    auto& changed =
                        list(second)[static_cast<std::size_t>(index)];
                    changed.address = text.trimmed();
                    changed.existing = 0;
                    update_all();
                });
        auto* port = new QSpinBox(row);
        port->setRange(1, 65535);
        port->setValue(static_cast<int>(end.network_port));
        port->setAccessibleName(tr("Network port"));
        connect(port, &QSpinBox::valueChanged, this,
                [this, second, index](int value)
                {
                    auto& changed =
                        list(second)[static_cast<std::size_t>(index)];
                    changed.network_port = static_cast<unsigned>(value);
                    changed.existing = 0;
                    update_all();
                });
        auto* protocol = new QComboBox(row);
        protocol->setAccessibleName(tr("How it talks"));
        protocol->addItem(QStringLiteral("TCP"), false);
        protocol->addItem(QStringLiteral("RFC 2217"), true);
        protocol->setCurrentIndex(end.rfc2217 ? 1 : 0);
        connect(protocol, &QComboBox::activated, this,
                [this, second, index, protocol]
                {
                    auto& changed =
                        list(second)[static_cast<std::size_t>(index)];
                    changed.rfc2217 = protocol->currentData().toBool();
                    changed.existing = 0;
                });
        line->addWidget(address, 1);
        line->addWidget(port);
        line->addWidget(protocol);
        layout->addLayout(line);
        layout->addWidget(hint_label(
            tr("For a serial server box, such as a Moxa or Lantronix. Its "
               "manual gives the address and port. Choose RFC 2217 if the box "
               "supports it, so programs can change the baud rate."),
            row));
    }
    else
    {
        auto* line = new QHBoxLayout();
        auto* device = new QComboBox(row);
        device->setObjectName(QStringLiteral("jobFormDevice"));
        device->setAccessibleName(tr("Device"));
        QStringList choices = free_devices();
        if (!end.device_path.isEmpty() && !choices.contains(end.device_path))
            choices.prepend(end.device_path);
        const auto devices = backend_.devices();
        for (const QString& path : choices)
        {
            QString text = path;
            for (const auto& known : devices)
            {
                if (known.path == path && !known.description.isEmpty())
                    text = tr("%1  %2").arg(path, known.description);
            }
            device->addItem(text, path);
        }
        if (choices.isEmpty())
            device->addItem(tr("No devices found"), QString());
        device->setCurrentIndex(std::max(0, device->findData(end.device_path)));
        connect(device, &QComboBox::activated, this,
                [this, second, index, device]
                {
                    auto& changed =
                        list(second)[static_cast<std::size_t>(index)];
                    const QString path = device->currentData().toString();
                    if (path != changed.device_path)
                        changed.existing = 0;
                    changed.device_path = path;
                    update_all();
                });
        auto* refresh = new QPushButton(tr("Refresh"), row);
        refresh->setIcon(IconProvider::instance().icon(
            QStringLiteral("refresh"), colour_role::ink));
        set_role(refresh, "secondary");
        connect(refresh, &QPushButton::clicked, this,
                [this]
                {
                    rebuild_ends();
                    update_all();
                });
        line->addWidget(device, 1);
        line->addWidget(refresh);
        const auto list_spec_value = spec(second);
        if (list_spec_value &&
            static_cast<int>(list(second).size()) > list_spec_value->min)
        {
            auto* remove = new QPushButton(row);
            remove->setIcon(IconProvider::instance().icon(
                QStringLiteral("remove"), colour_role::ink2));
            remove->setAccessibleName(tr("Remove %1").arg(end_name(end)));
            set_role(remove, "ghost");
            connect(remove, &QPushButton::clicked, this,
                    [this, second, index] { remove_end(second, index); });
            line->addWidget(remove);
        }
        layout->addLayout(line);
        layout->addWidget(hint_label(
            tr("Not listed? Plug it in, then choose Refresh."), row));
    }
    return row;
}

void JobForm::add_program()
{
    for (const bool second : {false, true})
    {
        const auto list_spec_value = spec(second);
        if (list_spec_value && list_spec_value->max > 1 &&
            !list_spec_value->device &&
            static_cast<int>(list(second).size()) < list_spec_value->max)
        {
            list(second).push_back(new_program());
            break;
        }
    }
    rebuild_ends();
    update_all();
}

void JobForm::add_device()
{
    const auto list_spec_value = spec(false);
    if (list_spec_value && list_spec_value->devices_too &&
        static_cast<int>(plan_.first.size()) < list_spec_value->max)
        plan_.first.push_back(new_device());
    rebuild_ends();
    update_all();
}

void JobForm::remove_end(bool second, int index)
{
    auto& ends = list(second);
    if (index < 0 || index >= static_cast<int>(ends.size()))
        return;
    ends.erase(ends.begin() + index);
    QTimer::singleShot(0, this,
                       [this]
                       {
                           rebuild_ends();
                           update_all();
                       });
}

void JobForm::set_label(bool second, int index, const QString& label)
{
    auto& ends = list(second);
    if (index >= 0 && index < static_cast<int>(ends.size()))
        ends[static_cast<std::size_t>(index)].label = label;
    rebuild_ends();
    update_all();
}

void JobForm::set_device(int index, const QString& path)
{
    for (const bool second : {false, true})
    {
        int seen = 0;
        for (auto& end : list(second))
        {
            if (end.what != end_plan::kind::device)
                continue;
            if (seen++ == index)
            {
                if (end.device_path != path)
                    end.existing = 0;
                end.device_path = path;
                rebuild_ends();
                update_all();
                return;
            }
        }
    }
}

void JobForm::set_echo(bool on)
{
    plan_.echo = on;
    if (answer_type_ != nullptr)
    {
        answer_type_->set_selected(!on);
        answer_echo_->set_selected(on);
    }
    update_all();
}

void JobForm::set_extras(bool type_here, bool show_data)
{
    if (type_here_ != nullptr)
        type_here_->setChecked(type_here);
    if (show_data_ != nullptr)
        show_data_->setChecked(show_data);
}

void JobForm::update_all()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.update_all");
    bool has_device = false;
    for (const auto* ends : {&plan_.first, &plan_.second})
    {
        for (const auto& end : *ends)
        {
            if (end.what == end_plan::kind::device)
                has_device = true;
        }
    }
    device_fields_->setVisible(has_device);
    if (type_here_ != nullptr)
    {
        const end_plan* device = nullptr;
        for (const auto* ends : {&plan_.first, &plan_.second})
        {
            for (const auto& end : *ends)
            {
                if (end.what != end_plan::kind::program && device == nullptr)
                    device = &end;
            }
        }
        type_here_->setText(
            tr("Let me type to %1 from this window")
                .arg(device != nullptr ? end_name(*device) : tr("the device")));
    }
    if (name_ != nullptr && previous_.has_value() == false)
        name_->setPlaceholderText(tr("Studio names it for you"));
    update_diagram();
    update_result();
    update_changes();
    validate();
}

void JobForm::update_diagram()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.update_diagram");
    QStringList left = end_names(plan_.first);
    QStringList right = end_names(plan_.second);
    auto style = WireDiagram::arrows::both;
    switch (kind_)
    {
    case job::port:
        right = {plan_.echo ? tr("Sends it straight back")
                            : tr("You, in this window")};
        break;
    case job::talk:
        right = {tr("You, in this window")};
        break;
    case job::network:
        right = {tr("Other computers, port %1").arg(plan_.listen_port)};
        break;
    case job::bus:
        style = WireDiagram::arrows::line;
        break;
    case job::combine:
        style = WireDiagram::arrows::right;
        break;
    case job::turns:
        style = WireDiagram::arrows::turns;
        break;
    default:
        break;
    }
    diagram_->set_ends(left, right, style);
}

QString JobForm::result_text() const
{
    const auto programs_open = [this](const std::vector<end_plan>& ends,
                                      const end_plan* device) -> QString
    {
        QStringList ports;
        for (const auto& end : ends)
        {
            if (end.what != end_plan::kind::program)
                continue;
            if (end.existing != 0)
            {
                if (const auto port = backend_.port(end.existing))
                    ports << port_shown_name(*port);
            }
            else if (end.com_number != 0)
            {
                ports << QStringLiteral("COM%1").arg(end.com_number);
            }
        }
        if (ports.isEmpty())
            return {};
        QString text =
            ports.size() == 1
                ? tr(" Your program opens <b>%1</b>").arg(ports.front())
                : tr(" Your programs open <b>%1</b>").arg(and_list(ports));
        if (device != nullptr && device->what == end_plan::kind::device)
            text += tr(", not %1").arg(device->device_path);
        return text + QStringLiteral(".");
    };
    const auto first_name = [this]
    { return plan_.first.empty() ? QString() : end_name(plan_.first.front()); };
    const auto second_name = [this]
    {
        return plan_.second.empty() ? QString()
                                    : end_name(plan_.second.front());
    };
    switch (kind_)
    {
    case job::port:
        return tr("Studio makes <b>%1</b> for your program to open. ")
                   .arg(first_name()) +
               (plan_.echo ? tr("Whatever it sends comes straight back to it.")
                           : tr("What it sends shows in this window, and what "
                                "you type here goes to it."));
    case job::pair:
        return tr("What <b>%1</b> sends arrives at <b>%2</b>, and the other "
                  "way round. Open one in each program.")
            .arg(first_name(), second_name());
    case job::attach:
        return tr("Studio opens <b>%1</b> and passes its data to and from "
                  "<b>%2</b>.")
                   .arg(first_name(), second_name()) +
               programs_open(plan_.second, plan_.first.data());
    case job::share:
        return tr("Everything <b>%1</b> sends goes to <b>%2</b>. Whatever any "
                  "of them sends goes to the device.")
                   .arg(first_name(), and_list(end_names(plan_.second))) +
               programs_open(plan_.second, plan_.first.data());
    case job::talk:
        return tr("Studio opens <b>%1</b>. On the next page, type to it and "
                  "read its replies.")
            .arg(first_name());
    case job::network:
        return tr("Other computers connect to this PC on network port "
                  "<b>%1</b> and talk to <b>%2</b>. Data goes both ways.")
            .arg(plan_.listen_port)
            .arg(first_name());
    case job::bus:
        return tr("<b>%1</b> share one line. Each hears what the others send, "
                  "and none hears itself.")
                   .arg(and_list(end_names(plan_.first))) +
               programs_open(plan_.first, nullptr);
    case job::combine:
        return tr("Everything <b>%1</b> send arrives at <b>%2</b>, in the "
                  "order it arrives. Nothing goes back to them.")
                   .arg(and_list(end_names(plan_.first)), second_name()) +
               programs_open(plan_.first, nullptr);
    case job::turns:
        return tr("<b>%1</b> can all send to <b>%2</b>, but only one at a "
                  "time. You choose which on the next page. What the device "
                  "sends back does not reach them.")
                   .arg(and_list(end_names(plan_.first)), second_name()) +
               programs_open(plan_.first, plan_.second.data());
    case job::custom:
        break;
    }
    return {};
}

void JobForm::update_result()
{
    const QString text = result_text();
    QString plain = text;
    plain.remove(QStringLiteral("<b>")).remove(QStringLiteral("</b>"));
    result_->set_result_text(text, plain);
}

void JobForm::update_changes()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.update_changes");
    if (!previous_ || changes_ == nullptr)
        return;
    const plan_changes changes = compare(plan_, *previous_);
    const auto names = [this](const std::vector<vss::endpoint_id>& ids)
    {
        QStringList result;
        for (const auto id : ids)
        {
            if (const auto port = backend_.port(id))
                result << port_shown_name(*port);
        }
        return result;
    };
    QStringList lines;
    const QStringList kept = names(changes.kept);
    const QStringList closed = names(changes.closed);
    if (!kept.isEmpty())
        lines << tr("%1 stay open. Programs using them are not interrupted.")
                     .arg(and_list(kept));
    if (changes.added > 0)
        lines << tr("%n new port(s) or device(s).", nullptr, changes.added);
    if (!closed.isEmpty())
        lines << tr("Closes %1. A program using one of them loses it.")
                     .arg(and_list(closed));
    if (changes.added == 0 && closed.isEmpty())
        lines << tr("No ports open or close.");
    changes_->set_text(
        tr("What Apply changes: %1").arg(lines.join(QLatin1Char(' '))));
}

void JobForm::validate()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.validate");
    blocker_.clear();
    QSet<QString> devices;
    QSet<unsigned> numbers;
    for (const auto* ends : {&plan_.first, &plan_.second})
    {
        for (const auto& end : *ends)
        {
            if (end.what == end_plan::kind::device)
            {
                if (end.device_path.isEmpty())
                    blocker_ = tr("Choose a device. Plug it in, then choose "
                                  "Refresh.");
                else if (devices.contains(end.device_path))
                    blocker_ = tr("%1 is chosen twice. Choose each device "
                                  "once.")
                                   .arg(end.device_path);
                devices.insert(end.device_path);
            }
            else if (end.what == end_plan::kind::network_device)
            {
                if (end.address.isEmpty())
                    blocker_ = tr("Enter the device's network address.");
            }
            else if (end.existing == 0 && end.com_number != 0)
            {
                if (numbers.contains(end.com_number))
                    blocker_ = tr("COM%1 is chosen twice. Choose another "
                                  "name.")
                                   .arg(end.com_number);
                numbers.insert(end.com_number);
            }
        }
    }
    if (kind_ == job::port && plan_.echo && !backend_.caps().fixture_echo)
        blocker_ = tr("Sending everything back is not available in this "
                      "build.");
    create_->setEnabled(blocker_.isEmpty());
    blocker_label_->setText(blocker_);
    blocker_label_->setVisible(!blocker_.isEmpty());
}

bool JobForm::submit()
{
    VSS_PROFILE_ZONE("gui.simple.job_form.submit");
    validate();
    if (!blocker_.isEmpty())
        return false;
    error_->setVisible(false);
    const setup_view* previous = previous_ ? &*previous_ : nullptr;
    const auto built = build_setup(backend_, plan_, previous);
    if (!built)
    {
        if (const auto problem = driver_problem_of(built.error()))
        {
            // The sentence already says that nothing was changed and what to do.
            error_->set_text(driver_problem_body(*problem));
            if (driver_problem_offers_setup(*problem))
            {
                error_->set_action(tr("Set up Virtual Serial Studio"),
                                   [this] { Q_EMIT driver_setup_requested(); });
            }
            else
            {
                // Nothing to set up: the underlying code goes behind a button.
                error_->set_action(
                    tr("Copy details"),
                    [] {
                        QGuiApplication::clipboard()->setText(
                            last_driver_details());
                    });
            }
            error_->setVisible(true);
            return false;
        }
        error_->clear_action();
        error_->set_text(
            editing()
                ? tr("Could not change the setup. %1 What was there before "
                     "was put back where Studio could.")
                      .arg(error_text(built.error()))
                : tr("Could not set it up. %1 Nothing was changed.")
                      .arg(error_text(built.error())));
        error_->setVisible(true);
        return false;
    }
    // A program's new name for a port that stays is a rename.
    if (backend_.caps().rename_port)
    {
        for (const auto* ends : {&plan_.first, &plan_.second})
        {
            for (const auto& end : *ends)
            {
                if (end.existing != 0 &&
                    original_labels_.contains(end.existing) &&
                    original_labels_.value(end.existing) != end.label &&
                    !end.label.isEmpty())
                    static_cast<void>(
                        backend_.rename_port(end.existing, end.label));
            }
        }
    }
    Q_EMIT done(*built, prefs_);
    return true;
}

} // namespace vss::gui::simple
