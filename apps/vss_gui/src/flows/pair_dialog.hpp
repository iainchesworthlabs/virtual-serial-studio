//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_PAIR_DIALOG_HPP
#define VSS_GUI_FLOWS_PAIR_DIALOG_HPP

#include "flows/common.hpp"
#include "flows/flow_dialog.hpp"
#include "flows/pair_panel.hpp"

#include <optional>

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

Q_SIGNALS:
    void advanced_requested();
    void setup_check_requested();
    void driver_setup_requested();
    void error_shown(const QString& title);

private:
    void create_pair();

    PairPanel* panel_ = nullptr;
    std::optional<pair_outcome> outcome_;
};

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_PAIR_DIALOG_HPP
