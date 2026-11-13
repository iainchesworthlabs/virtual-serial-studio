//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "flows/flow_dialog.hpp"

#include <QString>

#include <thread>

class QSpinBox;

namespace vss::gui
{

class FormRow;

/// "Change COM number": the number a program opens, such as COM7 in `COM7`.
///
/// The change restarts the port, which takes a second or two, so it runs on a
/// worker thread and the dialog stays responsive.  A refusal (a number another
/// device has, a program holding the port, no helper service) is said beside
/// the number, in words that name what to do, and nothing is changed.
class ComNumberDialog final : public FlowDialog
{
    Q_OBJECT

public:
    ComNumberDialog(StudioBackend& backend, vss::endpoint_id port,
                    QWidget* parent = nullptr);
    /// Waits for a change in progress.
    ~ComNumberDialog() override;

    QSpinBox* number_spin() const noexcept { return number_; }
    FormRow* number_row() const noexcept { return number_row_; }
    /// True from the click on Change number until the answer is shown.
    bool changing() const noexcept { return busy_; }
    /// The COM name the port has now: the new one after a successful change.
    QString com_name() const;
    void done(int result) override;

Q_SIGNALS:
    void error_shown(const QString& title);

private:
    /// Why the number cannot be used, or empty when it can.
    QString problem() const;
    void update_state();
    void change();
    void on_finished(unsigned number, const vss::result<QString>& outcome);

    StudioBackend& backend_;
    vss::endpoint_id port_ = 0;
    unsigned old_number_ = 0;
    bool busy_ = false;
    QSpinBox* number_ = nullptr;
    FormRow* number_row_ = nullptr;
    std::thread worker_;
};

} // namespace vss::gui

