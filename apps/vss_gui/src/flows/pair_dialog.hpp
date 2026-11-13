//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_PAIR_DIALOG_HPP
#define VSS_GUI_FLOWS_PAIR_DIALOG_HPP

#include "backend/background_work.hpp"
#include "flows/common.hpp"
#include "flows/flow_dialog.hpp"
#include "flows/pair_panel.hpp"

#include <optional>

class QPushButton;

class QLabel;
class QProgressBar;

namespace vss::gui
{

/// "Connect two programs": the standalone linked-pair form.
class PairDialog final : public FlowDialog
{
    Q_OBJECT

public:
    PairDialog(StudioBackend& backend, bool advanced,
               platform_kind platform = host_platform(),
               QWidget* parent = nullptr);

    PairPanel* panel() const noexcept { return panel_; }
    std::optional<pair_outcome> outcome() const { return outcome_; }
    void set_advanced(bool advanced) { panel_->set_advanced(advanced); }
    /// True while the ports are being made on a worker thread.  The dialog
    /// stays open until the answer is in.
    bool busy() const noexcept { return work_.busy(); }

Q_SIGNALS:
    void advanced_requested();
    void setup_check_requested();
    void driver_setup_requested();
    void error_shown(const QString& title);

protected:
    void done(int result) override;

private:
    void create_pair();
    void set_busy(bool busy);
    void show_error(const flow_error& error);

    StudioBackend& backend_;
    BackgroundWork work_;
    PairPanel* panel_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QWidget* busy_row_ = nullptr;
    QLabel* busy_label_ = nullptr;
    std::optional<pair_outcome> outcome_;
};

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_PAIR_DIALOG_HPP
