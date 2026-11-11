//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/line_matrix_panel.hpp"

#include "advanced/chrome.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

constexpr int kRows = 2;
constexpr int kColumns = 4;

const char* input_name(int column)
{
    switch (column)
    {
    case 0:
        return "DSR";
    case 1:
        return "CTS";
    case 2:
        return "DCD";
    case 3:
        return "RI";
    default:
        return "";
    }
}

struct fault_tile
{
    const char* title = nullptr;
    const char* description = nullptr;
    vss::fault_kind kind{};
};

const std::array<fault_tile, 6>& fault_tiles()
{
    static const std::array<fault_tile, 6> tiles = {{
        {QT_TRANSLATE_NOOP("LineMatrix", "Parity error"),
         QT_TRANSLATE_NOOP("LineMatrix", "Byte arrives with bad parity"),
         vss::fault_kind::parity},
        {QT_TRANSLATE_NOOP("LineMatrix", "Framing error"),
         QT_TRANSLATE_NOOP("LineMatrix",
                           "Byte arrives with no valid stop bit"),
         vss::fault_kind::framing},
        {QT_TRANSLATE_NOOP("LineMatrix", "Overrun"),
         QT_TRANSLATE_NOOP("LineMatrix", "Incoming data lost, too fast"),
         vss::fault_kind::overrun},
        {QT_TRANSLATE_NOOP("LineMatrix", "Break"),
         QT_TRANSLATE_NOOP("LineMatrix", "A break is received on the line"),
         vss::fault_kind::break_condition},
        {QT_TRANSLATE_NOOP("LineMatrix", "Hang-up"),
         QT_TRANSLATE_NOOP("LineMatrix", "Carrier drops, as a modem does"),
         vss::fault_kind::hangup},
        {QT_TRANSLATE_NOOP("LineMatrix", "Disconnect"),
         QT_TRANSLATE_NOOP("LineMatrix", "Device vanishes, as if unplugged"),
         vss::fault_kind::disconnect},
    }};
    return tiles;
}

} // namespace

LineMatrixPanel::LineMatrixPanel(StudioBackend& backend,
                                 vss::endpoint_id endpoint, QWidget* parent)
    : QWidget(parent), backend_(backend), endpoint_(endpoint)
{
    build();
    refresh();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh(); });
}

void LineMatrixPanel::build()
{
    VSS_PROFILE_ZONE("gui.advanced.line_matrix.build");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Theme::instance().space(4));
    layout->addWidget(build_wiring_column(), 1);
    layout->addWidget(build_fault_column(), 1);
}

QWidget* LineMatrixPanel::build_wiring_column()
{
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    // Left column: heading and intro, preset controls, the wiring matrix and
    // its summary, then the live state of the lines.
    layout->addWidget(
        make_heading(tr("Line wiring"), text_style::subtitle, column));
    auto* intro = new QLabel(
        tr("Choose which signals this port drives on the other port."), column);
    intro->setFont(Theme::instance().font(text_style::body));
    intro->setWordWrap(true);
    set_ink2(intro);
    layout->addWidget(intro);

    // A preset sets the whole matrix at once; the switch turns modem-line
    // emulation on or off.
    auto* controls = new QHBoxLayout();
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(16);
    preset_ = new SegmentedControl(column);
    preset_->setAccessibleName(tr("Line wiring preset"));
    for (const auto& entry : wiring_presets())
        preset_->add_item(preset_name(entry.id));
    connect(preset_, &SegmentedControl::current_changed, this,
            &LineMatrixPanel::on_preset_changed);
    controls->addWidget(preset_, 0);
    emulate_ = new Switch(tr("Emulate modem lines"), column);
    emulate_->setAccessibleName(tr("Emulate modem lines"));
    controls->addWidget(emulate_, 0, Qt::AlignVCenter);
    controls->addStretch(1);
    layout->addLayout(controls);

    // The matrix is a grid of check boxes: one row per output line this port
    // drives (DTR, RTS) and one column per input line on the other port.
    auto* matrix = new QFrame(column);
    matrix->setProperty("card", true);
    polish(matrix);
    auto* matrix_layout = new QVBoxLayout(matrix);
    matrix_layout->setContentsMargins(0, 0, 0, 0);
    matrix_layout->setSpacing(0);
    auto* grid_holder = new QWidget(matrix);
    auto* grid = new QGridLayout(grid_holder);
    grid->setContentsMargins(12, 10, 12, 8);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);

    // Header row: a corner label, then one heading per input line.
    auto* corner = new QLabel(tr("This port drives"), grid_holder);
    corner->setFont(Theme::instance().font(text_style::caption));
    corner->setProperty("muted", true);
    grid->addWidget(corner, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    for (int column_index = 0; column_index < kColumns; ++column_index)
    {
        auto* heading = new QLabel(
            QString::fromLatin1(input_name(column_index)), grid_holder);
        heading->setFont(Theme::instance().font(text_style::caption));
        heading->setProperty("muted", true);
        heading->setAlignment(Qt::AlignCenter);
        grid->addWidget(heading, 0, column_index + 1);
    }

    // One row per output line.  Each box gets a name such as "DTR to CTS" so
    // a screen reader and the tests can tell the boxes apart.
    for (int row = 0; row < kRows; ++row)
    {
        const QString output =
            row == 0 ? QStringLiteral("DTR") : QStringLiteral("RTS");
        auto* label = new QLabel(output, grid_holder);
        QFont label_font = Theme::instance().font(text_style::body);
        label_font.setWeight(QFont::DemiBold);
        label->setFont(label_font);
        grid->addWidget(label, row + 1, 0, Qt::AlignLeft | Qt::AlignVCenter);
        for (int column_index = 0; column_index < kColumns; ++column_index)
        {
            auto* box = new QCheckBox(grid_holder);
            const QString name =
                tr("%1 to %2")
                    .arg(output, QString::fromLatin1(input_name(column_index)));
            box->setAccessibleName(name);
            box->setToolTip(name);
            box->setLayoutDirection(Qt::LeftToRight);
            boxes_[row][column_index] = box;
            grid->addWidget(box, row + 1, column_index + 1, Qt::AlignCenter);
            connect(box, &QCheckBox::toggled, this,
                    [this] { on_box_toggled(); });
        }
    }
    matrix_layout->addWidget(grid_holder);

    // A caption strip under the grid, on a sunken background.  The colour is
    // set on the palette here, so it is not touched by the style sheet.
    caption_ = new QLabel(matrix);
    caption_->setFont(Theme::instance().font(text_style::caption));
    caption_->setProperty("muted", true);
    caption_->setWordWrap(true);
    caption_->setContentsMargins(12, 8, 12, 8);
    caption_->setAutoFillBackground(true);
    QPalette caption_palette = caption_->palette();
    caption_palette.setColor(QPalette::Window,
                             Theme::instance().colour(colour_role::sunken));
    caption_->setPalette(caption_palette);
    matrix_layout->addWidget(caption_);
    layout->addWidget(matrix);

    // A plain-language sentence for the current wiring, filled in later.
    sentence_ = new NoteBanner(NoteBanner::kind::info, {}, column);
    layout->addWidget(sentence_);

    // Live state: switches for the lines this port drives, a Break button and
    // chips for the four input lines.
    auto* state_card = new QFrame(column);
    state_card->setProperty("card", true);
    polish(state_card);
    auto* state_layout = new QVBoxLayout(state_card);
    state_layout->setContentsMargins(14, 12, 14, 12);
    state_layout->setSpacing(8);
    auto* state_title = new QLabel(tr("Line state now"), state_card);
    state_title->setFont(Theme::instance().font(text_style::caption));
    state_title->setProperty("muted", true);
    state_layout->addWidget(state_title);

    auto* state_row = new QHBoxLayout();
    state_row->setContentsMargins(0, 0, 0, 0);
    state_row->setSpacing(14);
    // updating_ is set while the panel moves the switches itself, so only a
    // user's toggle is sent on to the line.
    dtr_ = new Switch(tr("DTR"), state_card);
    dtr_->setAccessibleName(tr("DTR"));
    connect(dtr_, &QAbstractButton::toggled, this,
            [this](bool on)
            {
                if (!updating_)
                    set_line(vss::control_line::dtr, on);
            });
    state_row->addWidget(dtr_);
    rts_ = new Switch(tr("RTS"), state_card);
    rts_->setAccessibleName(tr("RTS"));
    connect(rts_, &QAbstractButton::toggled, this,
            [this](bool on)
            {
                if (!updating_)
                    set_line(vss::control_line::rts, on);
            });
    state_row->addWidget(rts_);
    // A break is a pulse: assert it, then clear it again after 100 ms.
    break_ = make_button(tr("Break"), nullptr, state_card);
    break_->setProperty("size", QStringLiteral("small"));
    polish(break_);
    break_->setAccessibleName(tr("Send a break"));
    connect(break_, &QPushButton::clicked, this,
            [this]
            {
                set_line(vss::control_line::break_signal, true);
                QTimer::singleShot(
                    100, this, [this]
                    { set_line(vss::control_line::break_signal, false); });
            });
    state_row->addWidget(break_);
    state_row->addStretch(1);
    state_layout->addLayout(state_row);

    // One chip per input line.  They start as "--" and are kept in inputs_ so
    // that refresh() can show the real state.
    auto* chip_grid = new QGridLayout();
    chip_grid->setContentsMargins(0, 0, 0, 0);
    chip_grid->setHorizontalSpacing(6);
    for (int index = 0; index < 4; ++index)
    {
        auto* chip = new StatusChip(QStringLiteral("--"),
                                    StatusChip::kind::closed, state_card);
        inputs_[index] = chip;
        chip_grid->addWidget(chip, 0, index);
    }
    state_layout->addLayout(chip_grid);
    layout->addWidget(state_card);
    layout->addStretch(1);
    return column;
}

QWidget* LineMatrixPanel::build_fault_column()
{
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    layout->addWidget(
        make_heading(tr("Inject a fault"), text_style::subtitle, column));
    auto* intro = new QLabel(
        tr("Simulate a bad cable or device. Each fault is reported as an "
           "error event on the port."),
        column);
    intro->setFont(Theme::instance().font(text_style::body));
    intro->setWordWrap(true);
    set_ink2(intro);
    layout->addWidget(intro);

    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    const auto& tiles = fault_tiles();
    for (std::size_t index = 0; index < tiles.size(); ++index)
    {
        const auto& tile = tiles[index];
        auto* card = new QFrame(column);
        card->setProperty("card", true);
        polish(card);
        card->setMinimumHeight(72);
        auto* card_layout = new QHBoxLayout(card);
        card_layout->setContentsMargins(12, 10, 12, 10);
        card_layout->setSpacing(8);
        auto* text_column = new QVBoxLayout();
        text_column->setContentsMargins(0, 0, 0, 0);
        text_column->setSpacing(2);
        const QString tile_title =
            QCoreApplication::translate("LineMatrix", tile.title);
        auto* title = new QLabel(tile_title, card);
        QFont title_font = Theme::instance().font(text_style::body);
        title_font.setWeight(QFont::DemiBold);
        title->setFont(title_font);
        title->setWordWrap(true);
        text_column->addWidget(title);
        auto* description = new QLabel(
            QCoreApplication::translate("LineMatrix", tile.description), card);
        description->setFont(Theme::instance().font(text_style::caption));
        description->setProperty("muted", true);
        description->setWordWrap(true);
        text_column->addWidget(description);
        card_layout->addLayout(text_column, 1);
        auto* inject = make_button(tr("Inject"), nullptr, card);
        inject->setProperty("size", QStringLiteral("small"));
        polish(inject);
        inject->setAccessibleName(
            QCoreApplication::translate("LineMatrix", "Inject %1")
                .arg(tile_title));
        // refresh() enables the button only for a running fixture, or a
        // driver port with a program on it, on a backend that can inject;
        // otherwise it stays disabled with the reason rather than pretending
        // to inject.
        degrade(inject);
        connect(inject, &QPushButton::clicked, this,
                [this, index] { inject_fault(index); });
        inject_.push_back(inject);
        card_layout->addWidget(inject, 0, Qt::AlignVCenter);
        grid->addWidget(card, static_cast<int>(index / 2),
                        static_cast<int>(index % 2));
    }
    layout->addLayout(grid);

    auto* history_title = new QLabel(tr("Injected so far"), column);
    history_title->setFont(Theme::instance().font(text_style::caption));
    history_title->setProperty("muted", true);
    layout->addWidget(history_title);

    auto* history_card = new QFrame(column);
    history_card->setProperty("card", true);
    polish(history_card);
    auto* history_layout = new QVBoxLayout(history_card);
    history_layout->setContentsMargins(0, 0, 0, 0);
    history_ = new QTableWidget(0, 3, history_card);
    history_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Fault"), tr("Result")});
    history_->setAccessibleName(tr("Injected faults"));
    history_->verticalHeader()->hide();
    history_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history_->setSelectionBehavior(QAbstractItemView::SelectRows);
    history_->horizontalHeader()->setStretchLastSection(true);
    history_->setMinimumHeight(110);
    history_layout->addWidget(history_);
    layout->addWidget(history_card);

    auto* code_title = new QLabel(tr("Create this fixture in code"), column);
    code_title->setFont(Theme::instance().font(text_style::caption));
    code_title->setProperty("muted", true);
    layout->addWidget(code_title);
    code_ = new CodeBlock(column);
    code_->set_text(QString(), tr("Create this fixture in code"));
    layout->addWidget(code_);
    layout->addStretch(1);
    return column;
}

void LineMatrixPanel::set_endpoint(vss::endpoint_id endpoint)
{
    endpoint_ = endpoint;
    refresh();
}

void LineMatrixPanel::refresh()
{
    VSS_PROFILE_ZONE("gui.advanced.line_matrix.refresh");
    const capabilities caps = backend_.caps();
    const auto port = backend_.port(endpoint_);

    // Find the two-way link this port is on, if any, and the name of the
    // port at its other end.  The first match wins.
    link_.reset();
    other_name_.clear();
    if (port)
    {
        for (const auto& candidate : backend_.links())
        {
            if (candidate.shape != link_shape::two_way)
                continue;
            const bool in_sources = candidate.sources.contains(endpoint_);
            const bool in_targets = candidate.targets.contains(endpoint_);
            if (!in_sources && !in_targets)
                continue;
            link_ = candidate;
            const auto other_ids =
                in_sources ? candidate.targets : candidate.sources;
            if (!other_ids.isEmpty())
                other_name_ = port_name(other_ids.first());
            break;
        }
    }

    // The port is gone (for example removed while the panel was open), so
    // disable the wiring controls and stop here.
    if (!port)
    {
        for (int row = 0; row < kRows; ++row)
        {
            for (int column = 0; column < kColumns; ++column)
                degrade(boxes_[row][column]);
        }
        degrade(preset_);
        degrade(emulate_);
        return;
    }

    // Show the wiring stored for the link.  Without a link, a stored value or
    // line-matrix support, show the standard null-modem wiring.
    line_wiring wiring;
    if (link_ && caps.line_matrix)
    {
        const auto stored = backend_.link_wiring(link_->id);
        if (stored)
            wiring = *stored;
        else
            wiring = preset_for(wiring_preset::standard_null_modem).wiring;
    }
    else
    {
        wiring = preset_for(wiring_preset::standard_null_modem).wiring;
    }
    write_boxes(wiring);

    // Select the preset that matches the boxes.  The signals are blocked so
    // this does not count as the user choosing a preset.
    const wiring_preset matched = preset_for_wiring(current_wiring());
    {
        const QSignalBlocker blocker(preset_);
        preset_->set_current(static_cast<int>(matched));
    }

    // The boxes can be edited only with line-matrix support and a link.
    // Otherwise they stay disabled with a tooltip that says what is missing.
    for (int row = 0; row < kRows; ++row)
    {
        for (int column = 0; column < kColumns; ++column)
        {
            const bool editable = caps.line_matrix && link_.has_value();
            boxes_[row][column]->setEnabled(editable);
            if (!editable)
            {
                boxes_[row][column]->setToolTip(
                    caps.line_matrix
                        ? tr("Link this fixture to another port first.")
                        : not_available_text());
                boxes_[row][column]->setAccessibleDescription(
                    boxes_[row][column]->toolTip());
            }
            else
            {
                boxes_[row][column]->setToolTip({});
                boxes_[row][column]->setAccessibleDescription({});
            }
        }
    }

    // The preset control follows the same rule as the boxes.
    if (caps.line_matrix && link_.has_value())
    {
        preset_->setEnabled(true);
        preset_->setToolTip({});
        preset_->setAccessibleDescription({});
    }
    else
    {
        degrade(preset_, caps.line_matrix
                             ? tr("Link this fixture to another port first.")
                             : not_available_text());
    }

    // Emulation is shown but always read-only: it is fixed when the link is
    // created, so the switch only reports it.
    emulate_->setChecked(link_ ? link_->options.emulate_modem_lines : true);
    degrade(emulate_, not_available_text());
    emulate_->setAccessibleDescription(
        tr("The line crossing is fixed when the link is created."));

    // The DTR and RTS switches and the Break button need a port that has
    // control lines.
    const bool supports_lines = port->supports_control_lines;
    for (Switch* toggle : {dtr_, rts_})
    {
        toggle->setEnabled(supports_lines);
        if (supports_lines)
        {
            toggle->setToolTip({});
            toggle->setAccessibleDescription({});
        }
        else
        {
            degrade(toggle);
        }
    }
    break_->setEnabled(supports_lines);
    if (supports_lines)
    {
        break_->setToolTip({});
        break_->setAccessibleDescription({});
    }
    else
    {
        degrade(break_);
    }

    // The code sample is named after the current port.
    code_->set_text(QStringLiteral("studio app;\nconst auto device = "
                                   "app.create_mock_port(\"%1\");")
                        .arg(port->name),
                    tr("Create this fixture in code"));

    // Faults can be injected into a running fixture (a mock port) on a backend
    // that supports faults.  A port of the Windows driver takes all six,
    // raised at the program that has it open, where the backend can do so.
    // Each other case gets its own reason.
    const bool is_fixture = port->kind == vss::endpoint_kind::mock;
    const bool is_driver_port =
        !is_fixture && port->driver_port && caps.driver_faults;
    const bool running = port->state == vss::endpoint_state::open;
    const bool can_inject = caps.faults && is_fixture && running;
    for (qsizetype index = 0; index < inject_.size(); ++index)
    {
        QPushButton* button = inject_.at(index);
        if (is_driver_port)
        {
            if (!running)
            {
                degrade(button, tr("The port is not running."));
            }
            else if (port->peer != peer_state::open)
            {
                degrade(button, tr("No program has the port open."));
            }
            else
            {
                button->setEnabled(true);
                button->setToolTip({});
                button->setAccessibleDescription({});
            }
        }
        else if (can_inject)
        {
            button->setEnabled(true);
            button->setToolTip({});
            button->setAccessibleDescription({});
        }
        else if (caps.faults && is_fixture)
        {
            degrade(button, tr("The fixture is not running."));
        }
        else if (caps.faults)
        {
            degrade(button, tr("Faults are only available on fixture ports."));
        }
        else
        {
            degrade(button);
        }
    }

    // Finish with the parts that are derived from the state set above.
    update_caption();
    update_line_state();
}

QCheckBox* LineMatrixPanel::matrix_box(int row, int column) const
{
    if (row < 0 || row >= kRows || column < 0 || column >= kColumns)
        return nullptr;
    return boxes_[row][column];
}

QCheckBox* LineMatrixPanel::box_named(const QString& name) const
{
    for (int row = 0; row < kRows; ++row)
    {
        for (int column = 0; column < kColumns; ++column)
        {
            if (boxes_[row][column]->accessibleName() == name)
                return boxes_[row][column];
        }
    }
    return nullptr;
}

QPushButton* LineMatrixPanel::inject_button(int index) const
{
    if (index < 0 || index >= inject_.size())
        return nullptr;
    return inject_.at(index);
}

int LineMatrixPanel::inject_count() const noexcept
{
    return static_cast<int>(inject_.size());
}

QString LineMatrixPanel::wiring_text() const
{
    return sentence_->text();
}

QString LineMatrixPanel::caption_text() const
{
    return caption_->text();
}

QString LineMatrixPanel::port_name(vss::endpoint_id id) const
{
    if (const auto port = backend_.port(id))
        return port->name;
    return tr("Port %1").arg(static_cast<qulonglong>(id));
}

void LineMatrixPanel::update_caption()
{
    const QString other =
        other_name_.isEmpty() ? tr("the other port") : other_name_;
    caption_->setText(tr("Column headings are the inputs on %1. The same "
                         "wiring applies in the other direction.")
                          .arg(other));
    caption_->setAccessibleName(caption_->text());
    QPalette caption_palette = caption_->palette();
    caption_palette.setColor(QPalette::Window,
                             Theme::instance().colour(colour_role::sunken));
    caption_->setPalette(caption_palette);
    const auto this_port = backend_.port(endpoint_);
    const QString this_name = this_port ? this_port->name : QString();
    sentence_->set_text(wiring_sentence(current_wiring(), this_name, other));
}

void LineMatrixPanel::write_boxes(const line_wiring& wiring)
{
    updating_ = true;
    const bool states[kRows][kColumns] = {{wiring.dtr_to_dsr, wiring.dtr_to_cts,
                                           wiring.dtr_to_dcd, wiring.dtr_to_ri},
                                          {wiring.rts_to_dsr, wiring.rts_to_cts,
                                           wiring.rts_to_dcd,
                                           wiring.rts_to_ri}};
    for (int row = 0; row < kRows; ++row)
    {
        for (int column = 0; column < kColumns; ++column)
        {
            const QSignalBlocker blocker(boxes_[row][column]);
            boxes_[row][column]->setChecked(states[row][column]);
        }
    }
    updating_ = false;
}

line_wiring LineMatrixPanel::current_wiring() const
{
    line_wiring wiring;
    wiring.dtr_to_dsr = boxes_[0][0]->isChecked();
    wiring.dtr_to_cts = boxes_[0][1]->isChecked();
    wiring.dtr_to_dcd = boxes_[0][2]->isChecked();
    wiring.dtr_to_ri = boxes_[0][3]->isChecked();
    wiring.rts_to_dsr = boxes_[1][0]->isChecked();
    wiring.rts_to_cts = boxes_[1][1]->isChecked();
    wiring.rts_to_dcd = boxes_[1][2]->isChecked();
    wiring.rts_to_ri = boxes_[1][3]->isChecked();
    return wiring;
}

void LineMatrixPanel::apply_preset(wiring_preset preset)
{
    if (preset == wiring_preset::custom)
        return;
    write_boxes(preset_for(preset).wiring);
    apply_wiring(current_wiring());
}

void LineMatrixPanel::on_box_toggled()
{
    if (updating_ || !backend_.caps().line_matrix)
        return;
    const wiring_preset matched = preset_for_wiring(current_wiring());
    {
        const QSignalBlocker blocker(preset_);
        preset_->set_current(static_cast<int>(matched));
    }
    apply_wiring(current_wiring());
}

void LineMatrixPanel::on_preset_changed(int id)
{
    if (updating_ || !backend_.caps().line_matrix)
        return;
    apply_preset(static_cast<wiring_preset>(id));
}

void LineMatrixPanel::apply_wiring(const line_wiring& wiring)
{
    VSS_PROFILE_ZONE("gui.advanced.line_matrix.apply_wiring");
    if (!link_)
    {
        Q_EMIT message(tr("Link this fixture to another port first."));
        return;
    }
    const auto applied = backend_.set_link_wiring(link_->id, wiring);
    if (!applied)
    {
        Q_EMIT message(tr("Could not change the wiring: %1")
                           .arg(error_text(applied.error())));
        refresh();
        return;
    }
    refresh();
    Q_EMIT message(tr("Line wiring updated."));
}

void LineMatrixPanel::inject_fault(std::size_t index)
{
    VSS_PROFILE_ZONE("gui.advanced.line_matrix.inject_fault");
    const auto& tiles = fault_tiles();
    if (index >= tiles.size())
        return;
    const QString title =
        QCoreApplication::translate("LineMatrix", tiles[index].title);
    const QString port = port_name(endpoint_);

    // A fault on a fixture stops it and reports an error event; one on a
    // driver port is raised at the program that has it open.
    const auto view = backend_.port(endpoint_);
    const bool at_program = view && view->kind != vss::endpoint_kind::mock &&
                            view->driver_port;

    const auto injected = backend_.inject_fault(endpoint_, tiles[index].kind);
    QString outcome;
    if (!injected)
    {
        outcome = tr("Could not inject the fault: %1")
                      .arg(error_text(injected.error()));
    }
    else if (at_program && tiles[index].kind == vss::fault_kind::hangup)
    {
        outcome = tr("DSR and DCD dropped on %1").arg(port);
    }
    else if (at_program && tiles[index].kind == vss::fault_kind::disconnect)
    {
        outcome = tr("Port taken back from the program on %1").arg(port);
    }
    else if (at_program)
    {
        outcome = tr("Line error raised at the program on %1").arg(port);
    }
    else
    {
        outcome = tr("Error event reported on %1").arg(port);
    }

    const int row = history_->rowCount();
    history_->insertRow(row);
    history_->setItem(row, 0,
                      new QTableWidgetItem(QTime::currentTime().toString(
                          QStringLiteral("HH:mm:ss"))));
    history_->setItem(row, 1, new QTableWidgetItem(title));
    history_->setItem(row, 2, new QTableWidgetItem(outcome));
    history_->scrollToBottom();

    Q_EMIT message(injected ? tr("%1 injected on %2.").arg(title, port)
                            : outcome);
    refresh();
}

void LineMatrixPanel::update_line_state()
{
    VSS_PROFILE_ZONE("gui.advanced.line_matrix.update_line_state");
    const auto port = backend_.port(endpoint_);
    const bool available = port.has_value();
    const bool supports = available && port->supports_control_lines;
    dtr_->setEnabled(supports);
    rts_->setEnabled(supports);
    break_->setEnabled(supports);
    if (!available)
        return;

    updating_ = true;
    dtr_->setChecked(port->lines.dtr());
    rts_->setChecked(port->lines.rts());
    updating_ = false;

    const bool states[4] = {port->lines.dsr(), port->lines.cts(),
                            port->lines.dcd(), port->lines.ri()};
    for (int index = 0; index < 4; ++index)
    {
        const QString name = QString::fromLatin1(input_name(index));
        const QString text =
            states[index] ? tr("%1 on").arg(name) : tr("%1 off").arg(name);
        inputs_[index]->set_text(text);
        inputs_[index]->set_kind(states[index] ? StatusChip::kind::healthy
                                               : StatusChip::kind::closed);
        inputs_[index]->setAccessibleName(text);
    }
}

void LineMatrixPanel::set_line(vss::control_line line, bool state)
{
    const auto changed = backend_.set_line(endpoint_, line, state);
    if (!changed)
    {
        Q_EMIT message(tr("Could not change a control line: %1")
                           .arg(error_text(changed.error())));
        update_line_state();
        return;
    }
    update_line_state();
}

} // namespace vss::gui
