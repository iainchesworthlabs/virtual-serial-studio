//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_SETUP_CHECK_DIALOG_HPP
#define VSS_GUI_SUPPORT_SETUP_CHECK_DIALOG_HPP

#include "backend/studio_backend.hpp"
#include "support/setup_facts.hpp"
#include "widgets/chips.hpp"
#include "widgets/notes.hpp"

#include <QDialog>
#include <QVector>

class QLabel;
class QVBoxLayout;

namespace vss::gui::support
{

class CheckRowWidget;

/// "Setup check": what this PC needs so other programs can use your virtual
/// ports.  Every row either comes from a real probe or says "Not checked
/// yet"; the dialog never fakes a pass.
class SetupCheckDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SetupCheckDialog(StudioBackend& backend,
                              QWidget* parent = nullptr);

    /// Re-reads the engine facts and the platform probes.
    void recheck();

    const setup_report& report() const noexcept { return report_; }
    int row_count() const { return static_cast<int>(rows_.size()); }
    CheckRowWidget* row_widget(int index) const;

Q_SIGNALS:
    /// The shell owns the unsaved-changes check and the relaunch.
    void restart_as_administrator_requested();
    /// The user chose "Continue in test mode"; the shell can explain the
    /// consequence in its own voice.
    void continue_in_test_mode_requested();

private:
    void build();
    void rebuild();
    void handle_action(setup_action action);
    void show_driver_setup();
    void show_firewall_help();
    void show_permission_help(vss::permissions::topic what);

    StudioBackend& backend_;
    setup_report report_;
    StatusChip* header_chip_ = nullptr;
    NoteBanner* driver_note_ = nullptr;
    QWidget* rows_host_ = nullptr;
    QVBoxLayout* rows_layout_ = nullptr;
    QLabel* platform_note_ = nullptr;
    QVector<CheckRowWidget*> rows_;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_SETUP_CHECK_DIALOG_HPP
