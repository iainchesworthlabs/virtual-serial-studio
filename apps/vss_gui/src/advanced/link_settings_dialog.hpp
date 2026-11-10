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

/// "Settings for <link>": delay, jitter, what happens when two write at once,
/// and the modem-line crossing.  The caller changes the link in place with
/// `options()` (`change_link_options` in `backend/canvas_edit`).  A bus offers
/// delay and jitter only.
class LinkSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    LinkSettingsDialog(const link_view& link, const QString& name,
                       QWidget* parent = nullptr);

    vss::link_options options() const;
    void set_delay(int milliseconds);
    void set_jitter(int milliseconds);
    QPushButton* apply_button() const noexcept { return apply_; }
    bool policy_shown() const;
    bool modem_lines_shown() const;

private:
    void update_state();

    link_view link_;
    QSpinBox* delay_ = nullptr;
    QSpinBox* jitter_ = nullptr;
    QComboBox* policy_ = nullptr;
    QCheckBox* modem_lines_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* apply_ = nullptr;
};

} // namespace vss::gui
