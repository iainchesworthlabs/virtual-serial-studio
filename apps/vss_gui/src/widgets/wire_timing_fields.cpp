//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/wire_timing_fields.hpp"

#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
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

WireTimingFields::WireTimingFields(QWidget* parent) : QWidget(parent)
{
    // Top level: the enable box and its hint, then one block of rows that is
    // greyed out while wire timing is off.
    setObjectName(QStringLiteral("wireTimingFields"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    const QString hint =
        tr("Bytes and breaks arrive as slowly as a serial line would carry "
           "them. A long write takes as long as it would on a wire, and a "
           "break holds back what follows it. Gaps under a millisecond use a "
           "little more of the processor.");
    enable_ = new QCheckBox(tr("Wire timing"), this);
    enable_->setObjectName(QStringLiteral("linkWireTiming"));
    enable_->setAccessibleName(tr("Wire timing"));
    enable_->setAccessibleDescription(hint);
    layout->addWidget(enable_);
    layout->addWidget(make_hint(hint, this));

    rows_ = new QWidget(this);
    rows_->setObjectName(QStringLiteral("wireTimingRows"));
    auto* rows = new QVBoxLayout(rows_);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(10);

    // Bit rate: zero is the "use the port's own rate" value, shown as text
    // through the special value text.
    bit_rate_ = new QSpinBox(rows_);
    bit_rate_->setObjectName(QStringLiteral("wireTimingBitRate"));
    bit_rate_->setRange(0, static_cast<int>(vss::max_bus_bit_rate));
    bit_rate_->setSingleStep(100);
    bit_rate_->setSuffix(tr(" bit/s"));
    bit_rate_->setSpecialValueText(tr("From the port"));
    bit_rate_->setValue(0);
    // The editor inside a spin box takes focus on its own: name it too.
    bit_rate_->findChild<QLineEdit*>()->setAccessibleName(tr("Bit rate"));
    rows->addWidget(new FormRow(
        tr("Bit rate"), bit_rate_,
        tr("Used when the sending port has no rate of its own. The rate the "
           "program set on the port is used first. With neither, the link is "
           "not paced."),
        rows_));

    // Framing: three combo boxes side by side. The item data holds the
    // engine enum value, so options() can read it back without parsing text.
    framing_row_ = new QWidget(rows_);
    framing_row_->setObjectName(QStringLiteral("wireTimingFraming"));
    auto* framing = new QHBoxLayout(framing_row_);
    framing->setContentsMargins(0, 0, 0, 0);
    framing->setSpacing(10);

    data_bits_ = new QComboBox(framing_row_);
    data_bits_->setObjectName(QStringLiteral("wireTimingDataBits"));
    data_bits_->setAccessibleName(tr("Data bits"));
    for (int bits = 5; bits <= 8; ++bits)
        data_bits_->addItem(QString::number(bits), bits);
    // Index 3 is eight data bits, the usual default.
    data_bits_->setCurrentIndex(3);

    parity_ = new QComboBox(framing_row_);
    parity_->setObjectName(QStringLiteral("wireTimingParity"));
    parity_->setAccessibleName(tr("Parity"));
    parity_->addItem(tr("None"), static_cast<int>(vss::parity_mode::none));
    parity_->addItem(tr("Odd"), static_cast<int>(vss::parity_mode::odd));
    parity_->addItem(tr("Even"), static_cast<int>(vss::parity_mode::even));
    parity_->addItem(tr("Mark"), static_cast<int>(vss::parity_mode::mark));
    parity_->addItem(tr("Space"), static_cast<int>(vss::parity_mode::space));

    stop_bits_ = new QComboBox(framing_row_);
    stop_bits_->setObjectName(QStringLiteral("wireTimingStopBits"));
    stop_bits_->setAccessibleName(tr("Stop bits"));
    stop_bits_->addItem(QStringLiteral("1"),
                        static_cast<int>(vss::stop_bits_mode::one));
    stop_bits_->addItem(QStringLiteral("1.5"),
                        static_cast<int>(vss::stop_bits_mode::one_point_five));
    stop_bits_->addItem(QStringLiteral("2"),
                        static_cast<int>(vss::stop_bits_mode::two));

    framing->addWidget(data_bits_);
    framing->addWidget(parity_);
    framing->addWidget(stop_bits_);
    rows->addWidget(new FormRow(
        tr("Framing"), framing_row_,
        tr("Data bits, parity and stop bits, used with the bit rate."),
        rows_));
    layout->addWidget(rows_);

    // Every edit refreshes which rows are usable and tells the owner that
    // the link settings have changed.
    connect(enable_, &QCheckBox::toggled, this,
            [this]
            {
                update_rows();
                Q_EMIT changed();
            });
    connect(bit_rate_, &QSpinBox::valueChanged, this,
            [this]
            {
                update_rows();
                Q_EMIT changed();
            });
    for (QComboBox* box : {data_bits_, parity_, stop_bits_})
        connect(box, &QComboBox::currentIndexChanged, this,
                [this] { Q_EMIT changed(); });

    // Keyboard order follows the visual order, top to bottom and left to
    // right.
    setTabOrder(enable_, bit_rate_);
    setTabOrder(bit_rate_, data_bits_);
    setTabOrder(data_bits_, parity_);
    setTabOrder(parity_, stop_bits_);
    update_rows();
}

vss::wire_timing_options WireTimingFields::options() const
{
    vss::wire_timing_options out;
    if (!enable_->isChecked())
        return out;
    out.enabled = true;
    out.bit_rate = static_cast<std::uint32_t>(bit_rate_->value());
    if (out.bit_rate != 0)
    {
        vss::wire_framing framing;
        framing.data_bits =
            static_cast<unsigned>(data_bits_->currentData().toInt());
        framing.parity =
            static_cast<vss::parity_mode>(parity_->currentData().toInt());
        framing.stop_bits =
            static_cast<vss::stop_bits_mode>(stop_bits_->currentData().toInt());
        // 8N1 is what no framing means.
        if (framing != vss::wire_framing{})
            out.framing = framing;
    }
    return out;
}

void WireTimingFields::set_options(const vss::wire_timing_options& value)
{
    {
        const QSignalBlocker block_enable(enable_);
        const QSignalBlocker block_rate(bit_rate_);
        const QSignalBlocker block_data(data_bits_);
        const QSignalBlocker block_parity(parity_);
        const QSignalBlocker block_stop(stop_bits_);
        enable_->setChecked(value.enabled);
        bit_rate_->setValue(static_cast<int>(value.bit_rate));
        const vss::wire_framing framing =
            value.framing.value_or(vss::wire_framing{});
        data_bits_->setCurrentIndex(
            data_bits_->findData(static_cast<int>(framing.data_bits)));
        parity_->setCurrentIndex(
            parity_->findData(static_cast<int>(framing.parity)));
        stop_bits_->setCurrentIndex(
            stop_bits_->findData(static_cast<int>(framing.stop_bits)));
    }
    update_rows();
    Q_EMIT changed();
}

bool WireTimingFields::rows_enabled() const
{
    return rows_->isEnabled();
}

bool WireTimingFields::framing_enabled() const
{
    return framing_row_->isEnabled();
}

void WireTimingFields::update_rows()
{
    rows_->setEnabled(enable_->isChecked());
    framing_row_->setEnabled(enable_->isChecked() && bit_rate_->value() != 0);
}

} // namespace vss::gui
