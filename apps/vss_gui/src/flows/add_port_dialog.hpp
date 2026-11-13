//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_ADD_PORT_DIALOG_HPP
#define VSS_GUI_FLOWS_ADD_PORT_DIALOG_HPP

#include "backend/studio_backend.hpp"
#include "flows/common.hpp"
#include "flows/flow_dialog.hpp"
#include "flows/pair_panel.hpp"

#include <QString>
#include <QVector>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace vss::gui
{

class FormRow;
class PickCard;
class ResultSentence;

/// "Add a port": the kind picker plus the inline virtual-port and linked-pair
/// settings.  Real device continues to the Attach dialog, exactly as the
/// "Next" label says.
class AddPortDialog final : public FlowDialog
{
    Q_OBJECT

public:
    enum class mode
    {
        virtual_port = 0,
        linked_pair = 1,
        real_device = 2,
        network_endpoint = 3,
        mock_fixture = 4
    };

    AddPortDialog(StudioBackend& backend, bool advanced,
                  mode initial = mode::virtual_port,
                  platform_kind platform = host_platform(),
                  QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    bool is_advanced() const noexcept { return advanced_; }
    mode selected_mode() const noexcept { return mode_; }
    void set_mode(mode value);

    QLineEdit* name_edit() const noexcept { return name_; }
    QSpinBox* count_spin() const noexcept { return count_; }
    QComboBox* kind_combo() const noexcept { return kind_; }
    /// The COM number to ask for, 0 for Automatic.  Shown only where the Studio
    /// driver is installed.
    QSpinBox* com_number_spin() const noexcept { return com_; }
    bool com_number_visible() const;
    FormRow* com_number_row() const noexcept { return com_row_; }
    ResultSentence* result_sentence() const noexcept { return result_; }
    PairPanel* pair_panel() const noexcept { return pair_panel_; }
    PickCard* mode_card(mode value) const;
    QPushButton* cancel_button() const noexcept { return cancel_; }
    QVector<vss::endpoint_id> created_ports() const { return created_ports_; }
    QString created_kind_token() const { return created_kind_; }
    bool wants_attach() const noexcept { return wants_attach_; }
    std::optional<pair_outcome> pair_result() const { return pair_outcome_; }

    /// Names the current form would use, after silent unique suffixing.
    QVector<QString> planned_names() const;
    QString validation_error() const;

Q_SIGNALS:
    void advanced_requested();
    void setup_check_requested();
    /// The error offered "Set up Virtual Serial Studio" and it was chosen.
    void driver_setup_requested();
    void error_shown(const QString& title);

private:
    void build(bool advanced, mode initial);
    void build_virtual_page();
    void build_pair_page();
    void build_real_page();
    void build_placeholder_page(mode value);
    void refresh_mode();
    void refresh_result();
    void validate_count();
    void refresh_com_row();
    void create_virtual_ports();
    void create_pair();
    void show_error(const flow_error& error);

    StudioBackend& backend_;
    platform_kind platform_;
    mode mode_ = mode::virtual_port;
    bool advanced_ = false;
    bool wants_attach_ = false;

    QVector<PickCard*> cards_;
    QStackedWidget* pages_ = nullptr;
    QWidget* virtual_page_ = nullptr;
    QWidget* pair_page_ = nullptr;
    QWidget* real_page_ = nullptr;
    QWidget* network_page_ = nullptr;
    QWidget* mock_page_ = nullptr;
    QLabel* settings_title_ = nullptr;
    QLabel* more_types_ = nullptr;
    QWidget* more_row_ = nullptr;
    QPushButton* advanced_button_ = nullptr;

    QLineEdit* name_ = nullptr;
    QSpinBox* count_ = nullptr;
    QComboBox* kind_ = nullptr;
    FormRow* name_row_ = nullptr;
    FormRow* count_row_ = nullptr;
    QSpinBox* com_ = nullptr;
    FormRow* com_row_ = nullptr;
    ResultSentence* result_ = nullptr;
    PairPanel* pair_panel_ = nullptr;
    QPushButton* cancel_ = nullptr;

    QVector<vss::endpoint_id> created_ports_;
    QString created_kind_;
    std::optional<pair_outcome> pair_outcome_;
};

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_ADD_PORT_DIALOG_HPP
