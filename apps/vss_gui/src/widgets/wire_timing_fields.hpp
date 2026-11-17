//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/link.hpp>

#include <QWidget>

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace vss::gui
{

/// The controls of a link wire timing in the link Settings dialog: the
/// "Wire timing" choice and, under it, the bit rate and the framing to use
/// when the sending port has none of its own.  The rows are disabled, not
/// hidden, until the choice is on, and the framing is disabled while the bit
/// rate is "From the port".
class WireTimingFields final : public QWidget
{
    Q_OBJECT

public:
    explicit WireTimingFields(QWidget* parent = nullptr);

    /// The options the controls describe.  With the choice off this is the
    /// default `wire_timing_options` (`enabled` false), whatever the other
    /// controls hold.
    vss::wire_timing_options options() const;

    void set_options(const vss::wire_timing_options& value);

    bool rows_enabled() const;
    bool framing_enabled() const;

    QCheckBox* enable_box() const noexcept { return enable_; }
    QSpinBox* bit_rate_spin() const noexcept { return bit_rate_; }
    QComboBox* data_bits_combo() const noexcept { return data_bits_; }
    QComboBox* parity_combo() const noexcept { return parity_; }
    QComboBox* stop_bits_combo() const noexcept { return stop_bits_; }

Q_SIGNALS:
    /// Any control changed, by the user or by `set_options`.
    void changed();

private:
    void update_rows();

    QCheckBox* enable_ = nullptr;
    QWidget* rows_ = nullptr;
    QWidget* framing_row_ = nullptr;
    QSpinBox* bit_rate_ = nullptr;
    QComboBox* data_bits_ = nullptr;
    QComboBox* parity_ = nullptr;
    QComboBox* stop_bits_ = nullptr;
};

} // namespace vss::gui
