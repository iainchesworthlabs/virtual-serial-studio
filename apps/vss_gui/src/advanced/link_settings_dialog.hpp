//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/view_types.hpp"

#include <vss/core/link.hpp>

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace vss::gui
{

class HalfDuplexFields;

/// "Settings for <link>": delay, jitter, what happens when two write at once,
/// and the modem-line crossing.  The caller changes the link in place with
/// `options()` (`change_link_options` in `backend/canvas_edit`).  A bus offers
/// delay and jitter and, where the backend has
/// `capabilities::half_duplex_bus`, the half-duplex line.
class LinkSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    /// `half_duplex_available` is `capabilities::half_duplex_bus` of the
    /// backend; without it a bus shows the half-duplex choice disabled.
    LinkSettingsDialog(const link_view& link, const QString& name,
                       bool half_duplex_available, QWidget* parent = nullptr);

    vss::link_options options() const;
    void set_delay(int milliseconds);
    void set_jitter(int milliseconds);
    QPushButton* apply_button() const noexcept { return apply_; }
    bool policy_shown() const;
    bool modem_lines_shown() const;
    bool half_duplex_shown() const;
    HalfDuplexFields* half_duplex_fields() const noexcept
    {
        return half_duplex_;
    }

private:
    void update_state();

    link_view link_;
    QSpinBox* delay_ = nullptr;
    QSpinBox* jitter_ = nullptr;
    QComboBox* policy_ = nullptr;
    QCheckBox* modem_lines_ = nullptr;
    HalfDuplexFields* half_duplex_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* apply_ = nullptr;
};

} // namespace vss::gui
