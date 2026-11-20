//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/link_settings_dialog.hpp"

#include "advanced/chrome.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/half_duplex_fields.hpp"
#include "widgets/monitor_direction.hpp"
#include "widgets/wire_timing_fields.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace vss::gui
{

LinkSettingsDialog::LinkSettingsDialog(const link_view& link,
                                       const QString& name,
                                       bool half_duplex_available,
                                       QWidget* parent)
    : QDialog(parent), link_(link)
{
    // Window setup: the title doubles as the accessible name, and the width
    // comes from a theme metric.
    const QString title = tr("Settings for %1").arg(name);
    setWindowTitle(title);
    setAccessibleName(title);
    setObjectName(QStringLiteral("linkSettingsDialog"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    // The page is a single column: header, the rows that apply to this kind
    // of link, an error line and a footer. The bottom margin is zero because
    // the footer draws its own padding.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 0);
    layout->setSpacing(14);
    layout->addWidget(make_header(
        title,
        tr("Applying changes the link where it is and keeps its ports. Bytes "
           "it still holds for its delay are discarded when the delay "
           "changes."),
        this, false, {}));

    // Delay and jitter are both millisecond spin boxes with the same range,
    // so one helper builds them.
    const auto make_ms = [this](const QString& label, int maximum)
    {
        auto* spin = new QSpinBox(this);
        spin->setObjectName(label);
        spin->setAccessibleName(label);
        spin->setRange(0, maximum);
        spin->setSuffix(tr(" ms"));
        return spin;
    };
    // Start from the values the link has now.
    delay_ = make_ms(tr("Delay"), static_cast<int>(vss::max_link_delay_ms));
    delay_->setValue(static_cast<int>(link.options.delay_ms));
    jitter_ = make_ms(tr("Jitter"), static_cast<int>(vss::max_link_delay_ms));
    jitter_->setValue(static_cast<int>(link.options.jitter_ms));
    auto* delay_row = new FormRow(
        tr("Delay"), delay_, tr("Every byte arrives this much later."), this);
    auto* jitter_row = new FormRow(
        tr("Jitter"), jitter_,
        tr("Each delay varies by up to this much. No more than the delay."),
        this);
    // A monitor is a passive copy: it has a direction and no delay.
    const bool monitor = link.shape == link_shape::monitor;
    delay_row->setVisible(!monitor);
    jitter_row->setVisible(!monitor);
    layout->addWidget(delay_row);
    layout->addWidget(jitter_row);

    // Wire timing applies to every shape except a bus, which has its own
    // half duplex settings below.
    wire_timing_ = new WireTimingFields(this);
    wire_timing_->set_options(link.options.wire_timing);
    wire_timing_->setVisible(link.shape != link_shape::bus);
    layout->addWidget(wire_timing_);

    // Direction is only for a monitor. The hint under it follows the choice
    // and is also set as the accessible description.
    direction_ = new QComboBox(this);
    direction_->setObjectName(QStringLiteral("linkDirection"));
    direction_->setAccessibleName(tr("Direction"));
    fill_monitor_direction(direction_, link.options.direction);
    auto* direction_row =
        new FormRow(tr("Direction"), direction_, {}, this);
    direction_row->setVisible(monitor);
    direction_->setAccessibleDescription(
        monitor_direction_description(link.options.direction));
    connect(direction_, &QComboBox::currentIndexChanged, this,
            [this, direction_row]
            {
                const auto now = static_cast<vss::monitor_direction>(
                    direction_->currentData().toInt());
                direction_row->set_hint(monitor_direction_description(now));
                direction_->setAccessibleDescription(
                    monitor_direction_description(now));
            });
    direction_row->set_hint(
        monitor_direction_description(link.options.direction));
    layout->addWidget(direction_row);

    // The merge policy only matters when more than one port can write to the
    // link, so the row stays hidden otherwise.
    const bool bus = link.shape == link_shape::bus;
    const bool many_senders = !bus && link.sources.size() > 1;
    policy_ = new QComboBox(this);
    policy_->setObjectName(QStringLiteral("linkPolicy"));
    policy_->setAccessibleName(tr("If two write at once"));
    policy_->addItem(tr("Mix them"),
                     static_cast<int>(vss::merge_policy::interleave));
    policy_->addItem(tr("One owner at a time"),
                     static_cast<int>(vss::merge_policy::owner));
    policy_->setCurrentIndex(
        link.options.policy == vss::merge_policy::owner ? 1 : 0);
    auto* policy_row =
        new FormRow(tr("If two write at once"), policy_, {}, this);
    policy_row->setVisible(many_senders);
    layout->addWidget(policy_row);

    // Modem line emulation only makes sense between two ports.
    modem_lines_ = new QCheckBox(tr("Emulate modem lines"), this);
    modem_lines_->setObjectName(QStringLiteral("linkModemLines"));
    modem_lines_->setChecked(link.options.emulate_modem_lines);
    modem_lines_->setVisible(link.shape == link_shape::two_way);
    layout->addWidget(modem_lines_);

    // Half duplex settings belong to a bus. The engine may not support them,
    // and the fields then show that they are unavailable.
    half_duplex_ = new HalfDuplexFields(this);
    half_duplex_->set_available(half_duplex_available);
    half_duplex_->set_options(link.options.half_duplex);
    half_duplex_->setVisible(bus);
    layout->addWidget(half_duplex_);

    // The error line is filled in by update_state when the values clash.
    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->setFont(Theme::instance().font(text_style::caption));
    set_ink2(error_);
    layout->addWidget(error_);
    layout->addStretch(1);

    // Footer: Apply is the default button and is the one update_state
    // enables or disables.
    QHBoxLayout* footer_layout = nullptr;
    auto* footer = make_footer(this, footer_layout);
    auto* cancel = make_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(cancel);
    apply_ = make_button(tr("Apply"), "primary", footer);
    apply_->setObjectName(QStringLiteral("linkSettingsApply"));
    apply_->setDefault(true);
    connect(apply_, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(apply_);
    layout->addWidget(footer);

    // Any edit re-checks the values, then the first useful control gets the
    // focus.
    connect(delay_, &QSpinBox::valueChanged, this, [this] { update_state(); });
    connect(jitter_, &QSpinBox::valueChanged, this, [this] { update_state(); });
    connect(half_duplex_, &HalfDuplexFields::changed, this,
            [this] { update_state(); });
    connect(wire_timing_, &WireTimingFields::changed, this,
            [this] { update_state(); });
    update_state();
    (direction_shown() ? static_cast<QWidget*>(direction_)
                       : static_cast<QWidget*>(delay_))
        ->setFocus(Qt::OtherFocusReason);
}

vss::link_options LinkSettingsDialog::options() const
{
    vss::link_options out = link_.options;
    out.delay_ms = static_cast<std::uint32_t>(delay_->value());
    out.jitter_ms = static_cast<std::uint32_t>(jitter_->value());
    if (direction_shown())
        out.direction = static_cast<vss::monitor_direction>(
            direction_->currentData().toInt());
    if (policy_shown())
        out.policy =
            static_cast<vss::merge_policy>(policy_->currentData().toInt());
    if (modem_lines_shown())
        out.emulate_modem_lines = modem_lines_->isChecked();
    if (half_duplex_shown())
        out.half_duplex = half_duplex_->options();
    if (wire_timing_shown())
        out.wire_timing = wire_timing_->options();
    return out;
}

void LinkSettingsDialog::set_delay(int milliseconds)
{
    delay_->setValue(milliseconds);
}

void LinkSettingsDialog::set_jitter(int milliseconds)
{
    jitter_->setValue(milliseconds);
}

bool LinkSettingsDialog::policy_shown() const
{
    return link_.shape != link_shape::bus && link_.sources.size() > 1;
}

bool LinkSettingsDialog::modem_lines_shown() const
{
    return link_.shape == link_shape::two_way;
}

bool LinkSettingsDialog::direction_shown() const
{
    return link_.shape == link_shape::monitor;
}

bool LinkSettingsDialog::wire_timing_shown() const
{
    return link_.shape != link_shape::bus;
}

bool LinkSettingsDialog::half_duplex_shown() const
{
    return link_.shape == link_shape::bus;
}

void LinkSettingsDialog::update_state()
{
    VSS_PROFILE_ZONE("gui.advanced.link_settings.update_state");
    const vss::link_options now = options();
    const bool valid = vss::valid_latency(now) &&
                       vss::valid_half_duplex(now.half_duplex) &&
                       vss::valid_wire_timing(now.wire_timing);
    error_->setText(valid ? QString()
                          : tr("The jitter can be no more than the delay."));
    apply_->setEnabled(valid);
}

} // namespace vss::gui
