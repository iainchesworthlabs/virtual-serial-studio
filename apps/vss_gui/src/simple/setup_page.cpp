//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/setup_page.hpp"
#include "language/bidi.hpp"

#include "theme/icon_provider.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

namespace vss::gui::simple
{

namespace
{

constexpr int kMaxLines = 500;

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

QLabel* muted(const QString& text, QWidget* parent)
{
    auto* label = make_label(text, text_style::caption, parent);
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QPlainTextEdit* text_view(const QString& name, QWidget* parent)
{
    auto* view = new QPlainTextEdit(parent);
    view->setReadOnly(true);
    view->setAccessibleName(name);
    keep_left_to_right(view);
    view->setFont(Theme::instance().font(text_style::data));
    view->setMinimumHeight(140);
    view->setMaximumBlockCount(kMaxLines);
    return view;
}

} // namespace

SetupPage::SetupPage(StudioBackend& backend, const setup_view& setup,
                     const setup_prefs& prefs, QWidget* parent)
    : QWidget(parent), backend_(backend), setup_(setup), prefs_(prefs)
{
    setObjectName(QStringLiteral("simpleSetupPage"));
    build();
}

QString SetupPage::shown(vss::endpoint_id id) const
{
    const auto port = backend_.port(id);
    if (!port)
        return {};
    const QString label = program_label(*port);
    const QString name = port_shown_name(*port);
    return label.isEmpty() ? name : tr("%1 (%2)").arg(name, label);
}

bool SetupPage::can_add_program() const
{
    switch (setup_.kind)
    {
    case job::share:
    case job::attach:
    case job::bus:
    case job::combine:
    case job::turns:
        return true;
    default:
        return false;
    }
}

void SetupPage::build()
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.build");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget(scroll);
    scroll->setWidget(body);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(12);

    top_ = new QWidget(body);
    top_layout_ = new QVBoxLayout(top_);
    top_layout_->setContentsMargins(0, 0, 0, 0);
    top_layout_->setSpacing(12);
    layout->addWidget(top_);

    // Typing.  The text goes to the device, or to the program on a lone
    // port; nothing extra is made for it.
    type_box_ = new QWidget(body);
    auto* type_layout = new QVBoxLayout(type_box_);
    type_layout->setContentsMargins(0, 8, 0, 0);
    type_layout->setSpacing(8);
    type_heading_ = group_label({}, type_box_);
    type_layout->addWidget(type_heading_);
    type_sentence_ = make_label({}, text_style::body, type_box_);
    type_layout->addWidget(type_sentence_);
    received_ = text_view(tr("Received text"), type_box_);
    received_->setObjectName(QStringLiteral("setupReceived"));
    type_layout->addWidget(received_);
    auto* type_row = new QHBoxLayout();
    input_ = new QLineEdit(type_box_);
    keep_left_to_right(input_);
    input_->setObjectName(QStringLiteral("setupInput"));
    input_->setPlaceholderText(tr("Type, then press Enter"));
    input_->setAccessibleName(tr("Text to send"));
    input_->setFont(Theme::instance().font(text_style::data));
    auto* send = new QPushButton(tr("Send"), type_box_);
    send->setIcon(IconProvider::instance().icon(QStringLiteral("send"),
                                                colour_role::onAccent));
    set_role(send, "primary");
    const auto send_now = [this]
    {
        if (send_typed(input_->text()))
            input_->clear();
    };
    connect(input_, &QLineEdit::returnPressed, this, send_now);
    connect(send, &QPushButton::clicked, this, send_now);
    line_ending_ = new Switch(tr("Enter adds a line ending"), type_box_);
    line_ending_->setChecked(true);
    type_row->addWidget(input_, 1);
    type_row->addWidget(send);
    type_layout->addLayout(type_row);
    type_layout->addWidget(line_ending_);
    layout->addWidget(type_box_);

    // Data going through.
    layout->addWidget(group_label(tr("Data going through"), body));
    show_data_ = new QPushButton(tr("Show me the data going through"), body);
    show_data_->setObjectName(QStringLiteral("setupShowData"));
    set_role(show_data_, "secondary");
    connect(show_data_, &QPushButton::clicked, this,
            [this] { set_show_data(true); });
    auto* show_row = new QHBoxLayout();
    show_row->addWidget(show_data_);
    show_row->addWidget(
        muted(tr("Read only. Use it to check the data when a program reports "
                 "that nothing arrives."),
              body),
        1);
    layout->addLayout(show_row);
    data_box_ = new QWidget(body);
    auto* data_layout = new QVBoxLayout(data_box_);
    data_layout->setContentsMargins(0, 0, 0, 0);
    data_ = text_view(tr("Data going through"), data_box_);
    data_->setObjectName(QStringLiteral("setupData"));
    data_layout->addWidget(data_);
    auto* data_row = new QHBoxLayout();
    pause_ = new QPushButton(tr("Pause"), data_box_);
    set_role(pause_, "secondary");
    connect(pause_, &QPushButton::clicked, this,
            [this]
            {
                paused_ = !paused_;
                pause_->setText(paused_ ? tr("Resume") : tr("Pause"));
            });
    auto* clear = new QPushButton(tr("Clear"), data_box_);
    set_role(clear, "secondary");
    connect(clear, &QPushButton::clicked, this,
            [this]
            {
                lines_.clear();
                render_data();
            });
    hex_ = new Switch(tr("Show as hex"), data_box_);
    connect(hex_, &QCheckBox::toggled, this,
            [this]
            {
                render_data();
                render_received();
            });
    auto* hide = new QPushButton(tr("Hide"), data_box_);
    set_role(hide, "ghost");
    connect(hide, &QPushButton::clicked, this,
            [this] { set_show_data(false); });
    data_row->addWidget(pause_);
    data_row->addWidget(clear);
    data_row->addWidget(hex_);
    data_row->addStretch(1);
    data_row->addWidget(hide);
    data_layout->addLayout(data_row);
    layout->addWidget(data_box_);

    auto* canvas =
        new QPushButton(tr("See it on the canvas (Advanced mode)"), body);
    set_role(canvas, "ghost");
    connect(canvas, &QPushButton::clicked, this,
            [this] { Q_EMIT advanced_requested(); });
    layout->addWidget(canvas, 0, Qt::AlignLeft);
    layout->addStretch(1);

    rebuild_top();
    set_show_data(prefs_.show_data);
}

void SetupPage::rebuild_top()
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.rebuild_top");
    while (QLayoutItem* item = top_layout_->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    port_lines_.clear();
    const auto ports = backend_.ports();

    // Header.
    auto* header = new QWidget(top_);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(12);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(4);
    auto* title = make_label(prefs_.name.isEmpty() ? setup_title(setup_, ports)
                                                   : prefs_.name,
                             text_style::title, header);
    title->setObjectName(QStringLiteral("setupTitle"));
    titles->addWidget(title);
    titles->addWidget(
        make_label(job_description(setup_.kind), text_style::body, header));
    header_layout->addLayout(titles, 1);
    auto* change = new QPushButton(setup_.kind == job::custom
                                       ? tr("Change in Advanced mode")
                                       : tr("Change…"),
                                   header);
    change->setObjectName(QStringLiteral("setupChange"));
    set_role(change, "secondary");
    connect(change, &QPushButton::clicked, this,
            [this]
            {
                if (setup_.kind == job::custom)
                    Q_EMIT advanced_requested();
                else
                    Q_EMIT change_requested();
            });
    auto* remove = new QPushButton(tr("Remove setup"), header);
    remove->setObjectName(QStringLiteral("setupRemove"));
    set_role(remove, "danger");
    connect(remove, &QPushButton::clicked, this,
            [this] { Q_EMIT remove_requested(); });
    header_layout->addWidget(change, 0, Qt::AlignTop);
    header_layout->addWidget(remove, 0, Qt::AlignTop);
    top_layout_->addWidget(header);

    // What to do now.
    const std::optional<port_view> device =
        setup_.device != 0 ? backend_.port(setup_.device) : std::nullopt;
    const bool program_ports = !setup_.programs.isEmpty();
    steps_ = nullptr;
    if (program_ports)
    {
        QStringList steps;
        for (const auto id : setup_.programs)
        {
            const auto port = backend_.port(id);
            if (!port)
                continue;
            const QString label = program_label(*port);
            const QString who = label.isEmpty() ? tr("your program") : label;
            if (device && device->kind == vss::endpoint_kind::physical)
                steps << tr("In %1, open <b>%2</b> at %3 baud.")
                             .arg(who.toHtmlEscaped(),
                                  port_shown_name(*port).toHtmlEscaped())
                             .arg(device->serial.baud_rate);
            else
                steps << tr("In %1, open <b>%2</b>.")
                             .arg(who.toHtmlEscaped(),
                                  port_shown_name(*port).toHtmlEscaped());
        }
        if (device && device->kind == vss::endpoint_kind::physical)
            steps << tr("Do not open <b>%1</b> itself in your programs. Studio "
                        "has it open.")
                         .arg(port_shown_name(*device).toHtmlEscaped());
        if (setup_.kind == job::turns)
            steps << tr("Choose below which program sends. The others only "
                        "listen.");
        top_layout_->addWidget(group_label(tr("What to do now"), top_));
        QString html = QStringLiteral("<ol style=\"margin-left:16px\">");
        for (const QString& step : steps)
            html += QStringLiteral("<li>%1</li>").arg(step);
        html += QStringLiteral("</ol>");
        steps_ = new QLabel(html, top_);
        steps_->setObjectName(QStringLiteral("setupSteps"));
        steps_->setTextFormat(Qt::RichText);
        steps_->setWordWrap(true);
        steps_->setFont(Theme::instance().font(text_style::bodyLarge));
        top_layout_->addWidget(steps_);
    }

    // Ports.
    top_layout_->addWidget(group_label(tr("Ports"), top_));
    for (const auto id : setup_.group.ports)
    {
        const auto port = backend_.port(id);
        if (!port || id == setup_.echo)
            continue;
        if (id == setup_.listener)
            top_layout_->addWidget(listener_row(*port, top_));
        else if (port->kind == vss::endpoint_kind::virtual_port)
            top_layout_->addWidget(program_row(*port, top_));
        else
            top_layout_->addWidget(device_row(*port, top_));
    }
    if (setup_.echo != 0)
    {
        top_layout_->addWidget(make_label(
            tr("On the other end: everything your program sends comes "
               "straight back to it."),
            text_style::body, top_));
        port_lines_ << tr("Echo");
    }
    if (can_add_program())
    {
        auto* add = new QPushButton(tr("Add a program"), top_);
        add->setObjectName(QStringLiteral("setupAddProgram"));
        add->setIcon(IconProvider::instance().icon(QStringLiteral("add"),
                                                   colour_role::ink));
        set_role(add, "secondary");
        connect(add, &QPushButton::clicked, this,
                [this] { Q_EMIT add_program_requested(); });
        top_layout_->addWidget(add, 0, Qt::AlignLeft);
    }

    for (int index = 0; index < top_layout_->count(); ++index)
    {
        if (QWidget* widget = top_layout_->itemAt(index)->widget())
            widget->show();
    }

    // Where typing goes.
    type_target_ = 0;
    if (setup_.kind == job::port && setup_.echo == 0)
        type_target_ = setup_.programs.value(0);
    else if (setup_.kind == job::talk ||
             (prefs_.type_here && setup_.device != 0))
        type_target_ = setup_.device;
    type_box_->setVisible(type_target_ != 0);
    if (type_target_ != 0)
    {
        const QString target = shown(type_target_);
        type_heading_->setText(tr("Type to %1").arg(target).toUpper());
        if (setup_.kind == job::port)
            type_sentence_->setText(
                tr("What your program sends on %1 shows here. What you type "
                   "goes to your program.")
                    .arg(target));
        else if (setup_.programs.isEmpty())
            type_sentence_->setText(
                tr("What you type goes to %1. Its replies show here.")
                    .arg(target));
        else
            type_sentence_->setText(
                tr("What you type goes to %1. Its replies show here, and your "
                   "programs get them too.")
                    .arg(target));
    }
}

QWidget* SetupPage::program_row(const port_view& port, QWidget* parent)
{
    // One card for a port that a program opens: its name, whether a program
    // has it open, then any warnings and actions.
    auto* row = new QFrame(parent);
    row->setProperty("group", true);
    polish(row);
    row->setAccessibleName(shown(port.id));
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);
    const QString label = program_label(port);
    layout->addWidget(
        muted(label.isEmpty() ? tr("Program opens") : tr("%1 opens").arg(label),
              row));
    // The top line: name on the left, then the state chip, a copy button and,
    // where allowed, a close button.
    auto* line = new QHBoxLayout();
    auto* name = new QLabel(port_shown_name(port), row);
    name->setFont(Theme::instance().font(text_style::portName));
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    line->addWidget(name, 1);

    // The chip says how far this port has got: closed, open with a program on
    // the other side, open with none there yet, or open with the other side
    // unknown. The same words are kept for port_lines() below.
    QString state;
    auto* chip = new StatusChip(row);
    if (!port.open)
    {
        chip->set_kind(StatusChip::kind::failed);
        state = tr("Not open");
    }
    else if (port.peer == peer_state::open)
    {
        chip->set_kind(StatusChip::kind::connected);
        state = tr("Open in a program");
    }
    else if (port.peer == peer_state::not_open)
    {
        chip->set_kind(StatusChip::kind::waiting);
        state = tr("No program yet");
    }
    else
    {
        chip->set_kind(StatusChip::kind::open);
        state = tr("Open");
    }
    chip->set_text(state);
    line->addWidget(chip);
    auto* copy = new CopyButton(row);
    copy->set_copy_text(port_shown_name(port), port_shown_name(port));
    connect(copy, &CopyButton::copied, this, [this, port]
            { Q_EMIT message(tr("%1 copied.").arg(port_shown_name(port))); });
    line->addWidget(copy);
    // These jobs need at least two programs and the others one, so a port can
    // only be closed while the setup stays above that.
    const int minimum = setup_.kind == job::share || setup_.kind == job::bus ||
                                setup_.kind == job::combine ||
                                setup_.kind == job::turns
                            ? 2
                            : 1;
    // The receiver cannot be closed from here.
    if (can_add_program() && setup_.programs.size() > minimum &&
        setup_.programs.contains(port.id) && port.id != setup_.receiver)
    {
        auto* close = new QPushButton(row);
        close->setIcon(IconProvider::instance().icon(QStringLiteral("remove"),
                                                     colour_role::ink2));
        close->setAccessibleName(tr("Close %1").arg(port_shown_name(port)));
        close->setToolTip(tr("Close this port"));
        set_role(close, "ghost");
        connect(close, &QPushButton::clicked, this,
                [this, id = port.id] { Q_EMIT close_port_requested(id); });
        line->addWidget(close);
    }
    layout->addLayout(line);
    port_lines_ << tr("%1: %2").arg(shown(port.id), state);

    // Until a program opens the port, say exactly which name it must choose.
    if (port.peer == peer_state::not_open)
    {
        auto* waiting = new QLabel(
            tr("No program has opened %1 yet. In %2, choose exactly %1. This "
               "changes as soon as one does. <a href=\"help\">Nothing "
               "arriving?</a>")
                .arg(port_shown_name(port).toHtmlEscaped(),
                     (label.isEmpty() ? tr("your program") : label)
                         .toHtmlEscaped()),
            row);
        waiting->setTextFormat(Qt::RichText);
        waiting->setWordWrap(true);
        waiting->setFont(Theme::instance().font(text_style::caption));
        connect(waiting, &QLabel::linkActivated, this,
                [this] { Q_EMIT troubleshoot_requested(); });
        layout->addWidget(waiting);
    }

    // If the program opened the port at a different baud rate from the real
    // device, the text will look garbled, so warn and say what to change.
    if (setup_.device != 0 && port.peer_format &&
        port.peer_format->baud_rate != 0)
    {
        if (const auto device = backend_.port(setup_.device))
        {
            if (device->kind == vss::endpoint_kind::physical &&
                device->serial.baud_rate != port.peer_format->baud_rate)
            {
                const QString who = label.isEmpty() ? tr("The program") : label;
                auto* warn = new NoteBanner(
                    NoteBanner::kind::warn,
                    tr("%1 opened %2 at %3 baud, but the device runs at %4. "
                       "Text will look garbled. Set %4 in %5.")
                        .arg(who, port_shown_name(port))
                        .arg(port.peer_format->baud_rate)
                        .arg(device->serial.baud_rate)
                        .arg(label.isEmpty() ? tr("the program") : label),
                    row);
                warn->setObjectName(QStringLiteral("setupBaudWarning"));
                layout->addWidget(warn);
            }
        }
    }

    // In a take-turns setup only one program sends. Mark that one, and give
    // each of the others a button to hand sending to it.
    if (setup_.kind == job::turns && setup_.link != 0)
    {
        if (setup_.owner == port.id)
        {
            layout->addWidget(new StatusChip(tr("Sending now"),
                                             StatusChip::kind::healthy, row),
                              0, Qt::AlignLeft);
        }
        else
        {
            auto* take = new QPushButton(tr("Let this one send"), row);
            set_role(take, "secondary");
            connect(take, &QPushButton::clicked, this,
                    [this, id = port.id]
                    {
                        const auto owned = backend_.set_owner(setup_.link, id);
                        Q_EMIT message(
                            owned ? tr("%1 sends now. The others only listen.")
                                        .arg(shown(id))
                                  : error_text(owned.error()));
                    });
            layout->addWidget(take, 0, Qt::AlignLeft);
        }
    }
    return row;
}

QWidget* SetupPage::device_row(const port_view& port, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setProperty("group", true);
    polish(row);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);
    const bool physical = port.kind == vss::endpoint_kind::physical;
    layout->addWidget(muted(
        physical ? tr("Device. Studio has it open, so your programs use their "
                      "own ports instead.")
                 : tr("Network device. Studio is connected to it."),
        row));
    auto* line = new QHBoxLayout();
    auto* name = new QLabel(port_shown_name(port), row);
    name->setFont(Theme::instance().font(text_style::portName));
    line->addWidget(name, 1);
    QString state;
    auto* chip = new StatusChip(row);
    if (port.open)
    {
        chip->set_kind(StatusChip::kind::healthy);
        state = physical ? tr("Open at %1 baud").arg(port.serial.baud_rate)
                         : tr("Connected");
    }
    else
    {
        chip->set_kind(StatusChip::kind::failed);
        state = tr("Not open");
    }
    chip->set_text(state);
    line->addWidget(chip);
    layout->addLayout(line);
    port_lines_ << tr("%1: %2").arg(port_shown_name(port), state);
    return row;
}

QWidget* SetupPage::listener_row(const port_view& port, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setProperty("group", true);
    polish(row);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);
    layout->addWidget(
        muted(tr("Other computers connect to this PC on network port"), row));
    auto* line = new QHBoxLayout();
    const QString number =
        port.path.section(QLatin1Char(':'), -1).section(QLatin1Char('?'), 0, 0);
    auto* name = new QLabel(number.isEmpty() ? port.path : number, row);
    name->setFont(Theme::instance().font(text_style::portName));
    line->addWidget(name, 1);
    const std::size_t clients = port.clients.value_or(0);
    const QString state = clients == 0 ? tr("No computer connected")
                                       : tr("%n computer(s) connected", nullptr,
                                            static_cast<int>(clients));
    line->addWidget(new StatusChip(state,
                                   clients == 0 ? StatusChip::kind::waiting
                                                : StatusChip::kind::connected,
                                   row));
    layout->addLayout(line);
    port_lines_ << tr("Network port %1: %2").arg(number, state);
    return row;
}

void SetupPage::refresh(const setup_view& setup)
{
    setup_ = setup;
    rebuild_top();
}

void SetupPage::set_show_data(bool on)
{
    prefs_.show_data = on;
    data_box_->setVisible(on);
    show_data_->setVisible(!on);
    if (on)
        render_data();
    Q_EMIT prefs_changed(prefs_);
}

QString SetupPage::render(const QByteArray& bytes) const
{
    if (hex_ != nullptr && hex_->isChecked())
        return QString::fromLatin1(bytes.toHex(' ')).toUpper();
    QString text;
    for (const char raw : bytes)
    {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '\r')
            text += QStringLiteral("\\r");
        else if (byte == '\n')
            text += QStringLiteral("\\n");
        else if (byte == '\t')
            text += QStringLiteral("\\t");
        else if (byte >= 0x20 && byte < 0x7f)
            text += QLatin1Char(static_cast<char>(byte));
        else
            text += QStringLiteral("\\x%1").arg(byte, 2, 16, QLatin1Char('0'));
    }
    return text;
}

void SetupPage::render_data()
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.render_data");
    if (data_ == nullptr || !prefs_.show_data)
        return;
    QStringList text;
    for (const auto& line : lines_)
        text << tr("%1  %2 sent  %3")
                    .arg(line.when.toString(QStringLiteral("HH:mm:ss.zzz")),
                         line.from, render(line.bytes));
    data_->setPlainText(text.join(QLatin1Char('\n')));
    data_->verticalScrollBar()->setValue(data_->verticalScrollBar()->maximum());
}

void SetupPage::render_received()
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.render_received");
    if (received_ == nullptr)
        return;
    QStringList text;
    for (const auto& line : conversation_)
        text << (line.sent ? tr("sent      %1") : tr("received  %1"))
                    .arg(render(line.bytes));
    received_->setPlainText(text.join(QLatin1Char('\n')));
    received_->verticalScrollBar()->setValue(
        received_->verticalScrollBar()->maximum());
}

void SetupPage::add_events(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.add_events");
    bool data_changed = false;
    bool conversation_changed = false;
    for (const auto& event : events)
    {
        if (event.kind != vss::event_kind::data ||
            event.direction != vss::data_direction::rx ||
            !setup_.group.ports.contains(event.endpoint))
            continue;
        if (!paused_)
        {
            lines_.push_back({QDateTime::currentDateTime(),
                              shown(event.endpoint), event.data.to_byte_array(),
                              false});
            if (lines_.size() > kMaxLines)
                lines_.remove(0, lines_.size() - kMaxLines);
            data_changed = true;
        }
        if (event.endpoint == type_target_)
        {
            conversation_.push_back({QDateTime::currentDateTime(),
                                     {},
                                     event.data.to_byte_array(),
                                     false});
            if (conversation_.size() > kMaxLines)
                conversation_.remove(0, conversation_.size() - kMaxLines);
            conversation_changed = true;
        }
    }
    if (data_changed)
        render_data();
    if (conversation_changed)
        render_received();
}

bool SetupPage::send_typed(const QString& text)
{
    VSS_PROFILE_ZONE("gui.simple.setup_page.send_typed");
    if (type_target_ == 0 || text.isEmpty())
        return false;
    QByteArray bytes = text.toUtf8();
    if (line_ending_ != nullptr && line_ending_->isChecked())
        bytes += "\r\n";
    const auto sent = backend_.send(type_target_, bytes);
    if (!sent)
    {
        Q_EMIT message(tr("Could not send to %1. %2")
                           .arg(shown(type_target_), error_text(sent.error())));
        return false;
    }
    conversation_.push_back({QDateTime::currentDateTime(), {}, bytes, true});
    render_received();
    return true;
}

QString SetupPage::steps_text() const
{
    return steps_ != nullptr ? steps_->text() : QString();
}

QString SetupPage::received_text() const
{
    return received_->toPlainText();
}

QString SetupPage::data_text() const
{
    return data_->toPlainText();
}

QStringList SetupPage::port_lines() const
{
    return port_lines_;
}

} // namespace vss::gui::simple
