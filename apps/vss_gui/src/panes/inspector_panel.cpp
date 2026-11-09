//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/inspector_panel.hpp"

#include "panes/fixture_behaviour.hpp"
#include "panes/fixture_panel.hpp"
#include "panes/format.hpp"
#include "panes/port_model.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/serial_settings.hpp>

#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <array>

namespace vss::gui
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QString line_name(vss::control_line line)
{
    switch (line)
    {
    case vss::control_line::dtr:
        return QObject::tr("DTR");
    case vss::control_line::rts:
        return QObject::tr("RTS");
    case vss::control_line::cts:
        return QObject::tr("CTS");
    case vss::control_line::dsr:
        return QObject::tr("DSR");
    case vss::control_line::dcd:
        return QObject::tr("DCD");
    case vss::control_line::ri:
        return QObject::tr("RI");
    case vss::control_line::break_signal:
        return QObject::tr("Break");
    case vss::control_line::count:
        break;
    }
    return {};
}

StatusChip::kind chip_kind(port_state_kind kind)
{
    switch (kind)
    {
    case port_state_kind::open:
    case port_state_kind::connected:
        return StatusChip::kind::healthy;
    case port_state_kind::waiting:
        return StatusChip::kind::waiting;
    case port_state_kind::studio_only:
        return StatusChip::kind::studio_only;
    case port_state_kind::closed:
        return StatusChip::kind::closed;
    case port_state_kind::failed:
        return StatusChip::kind::failed;
    case port_state_kind::fixture:
        return StatusChip::kind::fixture;
    }
    return StatusChip::kind::closed;
}

QString flow_text(vss::flow_control_mode mode)
{
    switch (mode)
    {
    case vss::flow_control_mode::none:
        return QObject::tr("no flow control");
    case vss::flow_control_mode::software:
        return QObject::tr("software flow control");
    case vss::flow_control_mode::hardware:
        return QObject::tr("hardware flow control");
    }
    return {};
}

} // namespace

InspectorPanel::InspectorPanel(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Inspector"));
    build();
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { refresh(); });
    connect(&backend_, &StudioBackend::stats_changed, this,
            [this] { refresh(); });
    connect(&backend_, &StudioBackend::lines_changed, this,
            [this](vss::endpoint_id id)
            {
                if (id != selected_)
                    return;
                // The adapter rebuilds its cache after the batch signal.
                QTimer::singleShot(0, this, [this] { refresh(); });
            });
    update_content();
}

void InspectorPanel::build()
{
    // Two mutually exclusive children: a hint shown when no port is selected,
    // and a scroll area holding the details. update_content() picks one.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    empty_ = new QLabel(tr("Select a port to inspect it."), this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setFont(Theme::instance().font(text_style::body));
    empty_->setProperty("muted", true);
    empty_->setContentsMargins(16, 16, 16, 16);
    root->addWidget(empty_, 1);

    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_ = new QWidget(scroll_);
    scroll_->setWidget(content_);
    root->addWidget(scroll_, 1);

    // Everything below goes into the scrolled content, top to bottom: name,
    // details form, help text, release button, then the control lines.
    auto* layout = new QVBoxLayout(content_);
    content_layout_ = layout;
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    name_ = new QLabel(content_);
    name_->setFont(semibold(Theme::instance().font(text_style::bodyLarge)));
    name_->setWordWrap(true);
    layout->addWidget(name_);

    // Details form: captions in column 0, values in column 1, which stretches.
    auto* form = new QGridLayout();
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    form->setColumnStretch(1, 1);

    int row = 0;
    form->addWidget(caption(tr("State"), content_), row, 0, Qt::AlignTop);
    state_ = new StatusChip(content_);
    form->addWidget(state_, row++, 1, Qt::AlignLeft);

    form->addWidget(caption(tr("Path"), content_), row, 0, Qt::AlignTop);
    // The path can be selected with the mouse or copied with its own button.
    auto* path_row = new QWidget(content_);
    auto* path_layout = new QHBoxLayout(path_row);
    path_layout->setContentsMargins(0, 0, 0, 0);
    path_layout->setSpacing(6);
    path_ = new QLabel(path_row);
    path_->setFont(Theme::instance().font(text_style::data));
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_->setWordWrap(true);
    copy_ = new CopyButton(path_row);
    copy_->set_copy_text({}, tr("path"));
    path_layout->addWidget(path_, 1);
    path_layout->addWidget(copy_, 0, Qt::AlignTop);
    form->addWidget(path_row, row++, 1);

    // The COM number row exists for every port but is only shown for driver
    // ports that have one (see update_content).
    com_caption_ = caption(tr("COM number"), content_);
    form->addWidget(com_caption_, row, 0, Qt::AlignTop);
    com_row_ = new QWidget(content_);
    auto* com_layout = new QHBoxLayout(com_row_);
    com_layout->setContentsMargins(0, 0, 0, 0);
    com_layout->setSpacing(6);
    com_value_ = new QLabel(com_row_);
    com_value_->setFont(Theme::instance().font(text_style::data));
    com_change_ = new QPushButton(tr("Change..."), com_row_);
    com_change_->setObjectName(QStringLiteral("inspectorComChange"));
    com_change_->setProperty("size", QStringLiteral("small"));
    com_change_->setAccessibleName(tr("Change COM number"));
    polish(com_change_);
    connect(com_change_, &QPushButton::clicked, this,
            [this]
            {
                if (selected_ != 0)
                    Q_EMIT com_number_requested(selected_);
            });
    com_layout->addWidget(com_value_);
    com_layout->addWidget(com_change_);
    com_layout->addStretch(1);
    form->addWidget(com_row_, row++, 1);

    form->addWidget(caption(tr("Traffic"), content_), row, 0);
    traffic_ = new QLabel(content_);
    traffic_->setFont(Theme::instance().font(text_style::data));
    form->addWidget(traffic_, row++, 1);

    // The serial number spans both columns and is advanced-only (see
    // set_advanced), and only for physical ports.
    serial_row_ = new QWidget(content_);
    auto* serial_layout = new QHBoxLayout(serial_row_);
    serial_layout->setContentsMargins(0, 0, 0, 0);
    serial_layout->setSpacing(8);
    serial_layout->addWidget(caption(tr("Serial"), content_));
    serial_label_ = new QLabel(content_);
    serial_label_->setFont(Theme::instance().font(text_style::data));
    serial_label_->setWordWrap(true);
    serial_layout->addWidget(serial_label_, 1);
    form->addWidget(serial_row_, row++, 0, 1, 2);

    layout->addLayout(form);

    // Help block: a note about the port's state, a button to copy its name for
    // the other program, and a link to troubleshooting.
    auto* help = new QWidget(content_);
    auto* help_layout = new QVBoxLayout(help);
    help_layout->setContentsMargins(0, 0, 0, 0);
    help_layout->setSpacing(2);
    help_text_ = new QLabel(help);
    help_text_->setFont(Theme::instance().font(text_style::body));
    help_text_->setWordWrap(true);
    help_link_ = new QLabel(
        tr("<a href=\"troubleshoot\">Why is nothing arriving?</a>"), help);
    help_link_->setTextFormat(Qt::RichText);
    help_link_->setFont(Theme::instance().font(text_style::body));
    connect(help_link_, &QLabel::linkActivated, this,
            [this](const QString&) { Q_EMIT troubleshoot_requested(); });
    help_copy_ = new QPushButton(tr("Copy name"), help);
    help_copy_->setObjectName(QStringLiteral("inspectorCopyName"));
    help_copy_->setProperty("size", QStringLiteral("small"));
    help_copy_->setCursor(Qt::PointingHandCursor);
    polish(help_copy_);
    connect(help_copy_, &QPushButton::clicked, this,
            [this]
            {
                if (!port_)
                    return;
                QGuiApplication::clipboard()->setText(port_->path);
                ToastManager::instance().show(
                    this, tr("%1 copied.").arg(port_->path));
            });
    help_layout->addWidget(help_text_);
    help_layout->addWidget(help_copy_, 0, Qt::AlignLeft);
    help_layout->addWidget(help_link_);
    layout->addWidget(help);

    // Releasing is not done here: the request is emitted for whoever
    // handles release_port_requested to carry out.
    release_ = new QPushButton(tr("Release port..."), content_);
    release_->setObjectName(QStringLiteral("inspectorRelease"));
    release_->setProperty("size", QStringLiteral("small"));
    release_->setAccessibleName(tr("Release port"));
    polish(release_);
    connect(release_, &QPushButton::clicked, this,
            [this]
            {
                if (selected_ != 0)
                    Q_EMIT release_port_requested(selected_);
            });
    layout->addWidget(release_, 0, Qt::AlignLeft);

    auto* controls_title = caption(tr("Control lines"), content_);
    layout->addWidget(controls_title);

    // Simple mode shows the control lines as read-only chips, three to a row.
    simple_lines_ = new QWidget(content_);
    auto* simple_grid = new QGridLayout(simple_lines_);
    simple_grid->setContentsMargins(0, 0, 0, 0);
    simple_grid->setSpacing(6);
    for (int index = 0; index < static_cast<int>(line_chips_.size()); ++index)
    {
        auto* chip = new StatusChip(simple_lines_);
        chip->set_small(true);
        line_chips_[static_cast<std::size_t>(index)] = chip;
        simple_grid->addWidget(chip, index / 3, index % 3);
    }
    layout->addWidget(simple_lines_);

    // Advanced mode replaces the chips with switches for the output lines, a
    // Break button and a row of chips for the input lines.
    advanced_lines_ = new QWidget(content_);
    auto* advanced_layout = new QVBoxLayout(advanced_lines_);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->setSpacing(6);
    auto* switch_row = new QHBoxLayout();
    switch_row->setSpacing(12);
    dtr_ = new Switch(tr("DTR"), advanced_lines_);
    rts_ = new Switch(tr("RTS"), advanced_lines_);
    break_ = new QPushButton(tr("Break"), advanced_lines_);
    break_->setProperty("size", QStringLiteral("small"));
    polish(break_);
    switch_row->addWidget(dtr_);
    switch_row->addWidget(rts_);
    switch_row->addStretch(1);
    switch_row->addWidget(break_);
    advanced_layout->addLayout(switch_row);

    auto* input_grid = new QGridLayout();
    input_grid->setContentsMargins(0, 0, 0, 0);
    input_grid->setSpacing(6);
    for (int index = 0; index < static_cast<int>(input_chips_.size()); ++index)
    {
        auto* chip = new StatusChip(advanced_lines_);
        chip->set_small(true);
        input_chips_[static_cast<std::size_t>(index)] = chip;
        input_grid->addWidget(chip, 0, index);
    }
    advanced_layout->addLayout(input_grid);
    layout->addWidget(advanced_lines_);

    // The link only emits a request for the owner of the panel to act on.
    advanced_link_ = new QLabel(
        tr("<a href=\"faults\">Open line matrix and faults</a>"), content_);
    advanced_link_->setTextFormat(Qt::RichText);
    advanced_link_->setFont(Theme::instance().font(text_style::body));
    connect(advanced_link_, &QLabel::linkActivated, this,
            [this](const QString&) { Q_EMIT advanced_tools_requested(); });
    layout->addWidget(advanced_link_);
    layout->addStretch(1);

    // Both switches behave the same. If the backend refuses, put the switch
    // back without re-triggering this handler and report the error. While
    // refresh() updates the switches (refreshing_) their signals are ignored.
    const auto connect_switch = [this](Switch* toggle, vss::control_line line)
    {
        connect(toggle, &Switch::toggled, this,
                [this, toggle, line](bool on)
                {
                    if (refreshing_)
                        return;
                    const auto result = backend_.set_line(selected_, line, on);
                    if (!result)
                    {
                        const QSignalBlocker blocker(toggle);
                        toggle->setChecked(!on);
                        Q_EMIT error_message(error_text(result.error()));
                    }
                });
    };
    connect_switch(dtr_, vss::control_line::dtr);
    connect_switch(rts_, vss::control_line::rts);

    // Break is held for as long as the button is pressed, so it is sent on
    // press and cleared on release rather than on click.
    connect(break_, &QPushButton::pressed, this,
            [this] { set_line(vss::control_line::break_signal, true); });
    connect(break_, &QPushButton::released, this,
            [this] { set_line(vss::control_line::break_signal, false); });
}

void InspectorPanel::set_advanced(bool advanced)
{
    if (advanced_ == advanced)
        return;
    advanced_ = advanced;
    simple_lines_->setVisible(!advanced);
    advanced_lines_->setVisible(advanced);
    serial_row_->setVisible(advanced && port_ &&
                            port_->kind == vss::endpoint_kind::physical);
    advanced_link_->setVisible(advanced);
    update_content();
}

void InspectorPanel::set_behaviours(FixtureBehaviours* behaviours)
{
    if (behaviours == nullptr || fixture_panel_ != nullptr)
        return;
    fixture_panel_ = new FixturePanel(*behaviours, content_);
    content_layout_->insertWidget(content_layout_->indexOf(advanced_link_),
                                  fixture_panel_);
    update_content();
}

void InspectorPanel::set_selected_port(vss::endpoint_id id)
{
    selected_ = id;
    refresh();
}

void InspectorPanel::refresh()
{
    if (selected_ == 0)
    {
        port_.reset();
        update_content();
        return;
    }
    port_ = backend_.port(selected_);
    update_content();
}

void InspectorPanel::update_content()
{
    const bool has_port = port_.has_value();
    empty_->setVisible(!has_port);
    scroll_->setVisible(has_port);
    if (!has_port)
        return;

    refreshing_ = true;
    const port_view& port = *port_;
    name_->setText(port.name);
    state_->set_kind(chip_kind(PortModel::state_kind(port)));
    state_->set_text(PortModel::state_text(port));
    state_->setToolTip(port.state == vss::endpoint_state::failed
                           ? tr("The engine reported a failure.")
                           : QString());
    path_->setText(port.path);
    copy_->set_copy_text(port.path, tr("path"));
    traffic_->setText(tr("RX %1, TX %2")
                          .arg(format_bytes(port.stats.bytes_rx),
                               format_bytes(port.stats.bytes_tx)));
    serial_row_->setVisible(advanced_ &&
                            port.kind == vss::endpoint_kind::physical);
    serial_label_->setText(serial_description());

    const bool waiting = port.open && port.peer == peer_state::not_open;
    QString note =
        waiting ? tr("No program has opened %1 yet. The program has to open "
                     "exactly %1; a different port will not receive anything.")
                      .arg(port.path)
                : QString();
    const QString status = PortModel::status_text(port);
    if (!status.isEmpty())
        note = note.isEmpty() ? status : note + QStringLiteral(" ") + status;
    help_text_->setVisible(!note.isEmpty());
    help_text_->setText(note);
    help_copy_->setVisible(waiting);
    help_copy_->setAccessibleName(tr("Copy name, %1").arg(port.path));
    help_copy_->setToolTip(tr("Copy name"));
    help_link_->setText(
        tr("<a href=\"troubleshoot\">Why is nothing arriving?</a>"));

    // A driver port has a COM number that can be changed, and in Advanced a
    // program that has it open can be made to let go.
    const capabilities caps = backend_.caps();
    const bool numbered = port.driver_port && port.com_number != 0;
    com_caption_->setVisible(numbered);
    com_row_->setVisible(numbered);
    com_value_->setText(QStringLiteral("COM%1").arg(port.com_number));
    com_change_->setEnabled(caps.com_numbers);
    com_change_->setToolTip(
        caps.com_numbers ? QString() : tr("Not available in this build"));
    release_->setVisible(advanced_ && port.driver_port &&
                         port.peer == peer_state::open);
    release_->setEnabled(caps.release_port);
    release_->setToolTip(caps.release_port ? QString()
                                           : tr("Not available in this build"));

    simple_lines_->setVisible(!advanced_);
    advanced_lines_->setVisible(advanced_);
    advanced_link_->setVisible(advanced_);

    if (fixture_panel_ != nullptr)
    {
        // The behaviours are an Advanced control of a mock fixture.
        const bool fixture = advanced_ && port.kind == vss::endpoint_kind::mock;
        fixture_panel_->set_port(fixture ? port.id : 0);
        fixture_panel_->setVisible(fixture);
    }

    update_lines();
    const bool can_drive = port.supports_control_lines;
    dtr_->setEnabled(can_drive);
    rts_->setEnabled(can_drive);
    break_->setEnabled(can_drive);
    const QString tooltip =
        can_drive ? QString() : tr("This port does not support control lines.");
    dtr_->setToolTip(tooltip);
    rts_->setToolTip(tooltip);
    break_->setToolTip(tooltip);

    if (advanced_)
    {
        const QSignalBlocker dtr_blocker(dtr_);
        const QSignalBlocker rts_blocker(rts_);
        dtr_->setChecked(port.lines.dtr());
        rts_->setChecked(port.lines.rts());
    }
    refreshing_ = false;
}

void InspectorPanel::update_lines()
{
    if (!port_)
        return;
    const port_view& port = *port_;
    const std::array<vss::control_line, 6> lines = {
        vss::control_line::dtr, vss::control_line::rts, vss::control_line::cts,
        vss::control_line::dsr, vss::control_line::dcd, vss::control_line::ri};
    for (int index = 0; index < static_cast<int>(line_chips_.size()); ++index)
    {
        const auto line = lines[static_cast<std::size_t>(index)];
        const bool on = port.lines.test(line);
        StatusChip* chip = line_chips_[static_cast<std::size_t>(index)];
        chip->set_text(on ? tr("%1 on").arg(line_name(line))
                          : tr("%1 off").arg(line_name(line)));
        chip->set_kind(on ? StatusChip::kind::healthy
                          : StatusChip::kind::closed);
    }

    const std::array<vss::control_line, 4> inputs = {
        vss::control_line::cts, vss::control_line::dsr, vss::control_line::dcd,
        vss::control_line::ri};
    for (int index = 0; index < static_cast<int>(input_chips_.size()); ++index)
    {
        const auto line = inputs[static_cast<std::size_t>(index)];
        const bool on = port.lines.test(line);
        StatusChip* chip = input_chips_[static_cast<std::size_t>(index)];
        chip->set_text(on ? tr("%1 on").arg(line_name(line))
                          : tr("%1 off").arg(line_name(line)));
        chip->set_kind(on ? StatusChip::kind::healthy
                          : StatusChip::kind::closed);
    }
}

void InspectorPanel::set_line(vss::control_line line, bool state)
{
    if (selected_ == 0)
        return;
    const auto result = backend_.set_line(selected_, line, state);
    if (!result)
        Q_EMIT error_message(error_text(result.error()));
}

QString InspectorPanel::serial_description() const
{
    if (!port_ || port_->kind != vss::endpoint_kind::physical)
        return {};
    return tr("%1, %2").arg(
        QString::fromStdString(vss::to_label(port_->serial)),
        flow_text(port_->serial.flow_control));
}

} // namespace vss::gui
