//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/half_duplex_fields.hpp"

#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

/// A muted caption under a checkbox, in the style of a FormRow hint.
QLabel* make_hint(const QString& text, QWidget* parent)
{
    auto* hint = new QLabel(text, parent);
    hint->setFont(Theme::instance().font(text_style::caption));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    QObject::connect(
        &Theme::instance(), &Theme::theme_changed, hint,
        [hint] { hint->setFont(Theme::instance().font(text_style::caption)); });
    return hint;
}

} // namespace

HalfDuplexFields::HalfDuplexFields(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("halfDuplexFields"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    const QString line_hint =
        tr("One port sends at a time, as on a two-wire RS-485 line. Studio "
           "reports when two send at once.");
    enable_ = new QCheckBox(tr("Half-duplex line"), this);
    enable_->setObjectName(QStringLiteral("busHalfDuplex"));
    enable_->setAccessibleName(tr("Half-duplex line"));
    enable_->setAccessibleDescription(line_hint);
    layout->addWidget(enable_);
    layout->addWidget(make_hint(line_hint, this));

    rows_ = new QWidget(this);
    rows_->setObjectName(QStringLiteral("halfDuplexRows"));
    auto* rows = new QVBoxLayout(rows_);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(10);

    bit_rate_ = new QSpinBox(rows_);
    bit_rate_->setObjectName(QStringLiteral("busBitRate"));
    bit_rate_->setRange(static_cast<int>(vss::min_bus_bit_rate),
                        static_cast<int>(vss::max_bus_bit_rate));
    bit_rate_->setValue(static_cast<int>(vss::half_duplex_options{}.bit_rate));
    bit_rate_->setSingleStep(100);
    bit_rate_->setSuffix(tr(" bit/s"));
    // The editor inside a spin box takes focus on its own: name it too.
    bit_rate_->findChild<QLineEdit*>()->setAccessibleName(tr("Line rate"));
    rows->addWidget(new FormRow(
        tr("Line rate"), bit_rate_,
        tr("A byte takes ten bits. The others hear a message when its last "
           "byte has left the line."),
        rows_));

    turnaround_ = new QSpinBox(rows_);
    turnaround_->setObjectName(QStringLiteral("busTurnaround"));
    turnaround_->setRange(0, static_cast<int>(vss::max_link_delay_ms));
    turnaround_->setSuffix(tr(" ms"));
    turnaround_->findChild<QLineEdit*>()->setAccessibleName(tr("Turn-around"));
    rows->addWidget(new FormRow(
        tr("Turn-around"), turnaround_,
        tr("The line stays taken this long after a port stops sending. An "
           "answer that starts inside it is reported."),
        rows_));

    collisions_ = new QComboBox(rows_);
    collisions_->setObjectName(QStringLiteral("busCollisions"));
    collisions_->addItem(tr("Report it"),
                         static_cast<int>(vss::collision_action::report));
    collisions_->addItem(tr("Report it and garble the bytes"),
                         static_cast<int>(vss::collision_action::garble));
    rows->addWidget(
        new FormRow(tr("If two send at once"), collisions_, {}, rows_));

    const QString rts_hint =
        tr("For ports whose RTS Studio can follow. In this build that is a "
           "loopback port. Every other port always sends.");
    rts_gating_ = new QCheckBox(tr("Send only while RTS is high"), rows_);
    rts_gating_->setObjectName(QStringLiteral("busRtsGating"));
    rts_gating_->setAccessibleName(tr("Send only while RTS is high"));
    rts_gating_->setAccessibleDescription(rts_hint);
    rows->addWidget(rts_gating_);
    rows->addWidget(make_hint(rts_hint, rows_));
    layout->addWidget(rows_);

    connect(enable_, &QCheckBox::toggled, this,
            [this]
            {
                update_rows();
                Q_EMIT changed();
            });
    connect(bit_rate_, &QSpinBox::valueChanged, this,
            [this] { Q_EMIT changed(); });
    connect(turnaround_, &QSpinBox::valueChanged, this,
            [this] { Q_EMIT changed(); });
    connect(collisions_, &QComboBox::currentIndexChanged, this,
            [this] { Q_EMIT changed(); });
    connect(rts_gating_, &QCheckBox::toggled, this,
            [this] { Q_EMIT changed(); });

    setTabOrder(enable_, bit_rate_);
    setTabOrder(bit_rate_, turnaround_);
    setTabOrder(turnaround_, collisions_);
    setTabOrder(collisions_, rts_gating_);
    update_rows();
}

vss::half_duplex_options HalfDuplexFields::options() const
{
    vss::half_duplex_options out;
    if (!enable_->isChecked())
        return out;
    out.enabled = true;
    out.bit_rate = static_cast<std::uint32_t>(bit_rate_->value());
    out.turnaround_ms = static_cast<std::uint32_t>(turnaround_->value());
    out.on_collision =
        static_cast<vss::collision_action>(collisions_->currentData().toInt());
    out.rts_gating = rts_gating_->isChecked();
    return out;
}

void HalfDuplexFields::set_options(const vss::half_duplex_options& value)
{
    {
        const QSignalBlocker block_enable(enable_);
        const QSignalBlocker block_rate(bit_rate_);
        const QSignalBlocker block_turnaround(turnaround_);
        const QSignalBlocker block_collisions(collisions_);
        const QSignalBlocker block_rts(rts_gating_);
        enable_->setChecked(value.enabled && available_);
        bit_rate_->setValue(static_cast<int>(value.bit_rate));
        turnaround_->setValue(static_cast<int>(value.turnaround_ms));
        collisions_->setCurrentIndex(
            value.on_collision == vss::collision_action::garble ? 1 : 0);
        rts_gating_->setChecked(value.rts_gating);
    }
    update_rows();
    Q_EMIT changed();
}

void HalfDuplexFields::set_available(bool available)
{
    available_ = available;
    enable_->setEnabled(available);
    enable_->setToolTip(available ? QString()
                                  : tr("Not available in this build"));
    if (!available)
        enable_->setChecked(false);
    update_rows();
}

bool HalfDuplexFields::rows_enabled() const
{
    return rows_->isEnabled();
}

void HalfDuplexFields::update_rows()
{
    rows_->setEnabled(available_ && enable_->isChecked());
}

} // namespace vss::gui
