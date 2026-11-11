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
class QWidget;

namespace vss::gui
{

/// The controls of a half-duplex bus line, shared by the Connect ports dialog
/// and the link Settings dialog: the "Half-duplex line" choice and, under it,
/// the line rate, the turn-around, what happens when two send at once and RTS
/// gating.  The rows are disabled, not hidden, until the choice is on.
///
/// A backend that cannot run a half-duplex bus
/// (`capabilities::half_duplex_bus`) calls `set_available(false)`: the choice
/// is then off and disabled, with the tooltip "Not available in this build".
class HalfDuplexFields final : public QWidget
{
    Q_OBJECT

public:
    explicit HalfDuplexFields(QWidget* parent = nullptr);

    /// The options the controls describe.  With the choice off this is the
    /// default `half_duplex_options` (`enabled` false), whatever the other
    /// controls hold.
    vss::half_duplex_options options() const;

    /// Fills the controls.  `enabled` turns the choice on only while the
    /// fields are available.
    void set_options(const vss::half_duplex_options& value);

    void set_available(bool available);
    bool available() const noexcept { return available_; }

    /// True while the line rate, turn-around, collision and RTS rows can be
    /// edited.
    bool rows_enabled() const;

    QCheckBox* enable_box() const noexcept { return enable_; }
    QSpinBox* bit_rate_spin() const noexcept { return bit_rate_; }
    QSpinBox* turnaround_spin() const noexcept { return turnaround_; }
    QComboBox* collisions_combo() const noexcept { return collisions_; }
    QCheckBox* rts_gating_box() const noexcept { return rts_gating_; }

Q_SIGNALS:
    /// Any control changed, by the user or by `set_options`.
    void changed();

private:
    void update_rows();

    bool available_ = true;
    QCheckBox* enable_ = nullptr;
    QWidget* rows_ = nullptr;
    QSpinBox* bit_rate_ = nullptr;
    QSpinBox* turnaround_ = nullptr;
    QComboBox* collisions_ = nullptr;
    QCheckBox* rts_gating_ = nullptr;
};

} // namespace vss::gui
