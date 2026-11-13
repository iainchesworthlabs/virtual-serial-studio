//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_PAIR_PANEL_HPP
#define VSS_GUI_FLOWS_PAIR_PANEL_HPP

#include "backend/studio_backend.hpp"
#include "flows/common.hpp"

#include <QLineEdit>
#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;

namespace vss::gui
{

class Disclosure;
class FormRow;
class NoteBanner;
class ResultSentence;

struct pair_outcome
{
    vss::endpoint_id first = 0;
    vss::endpoint_id second = 0;
    vss::link_id link = 0;
    QString first_path;
    QString second_path;
};

/// What `PairPanel::prepare_pair` read from the form, so the ports can be made
/// on a worker thread without touching a widget.
struct pair_request
{
    QString base;
    bool emulate_modem_lines = true;
};

/// The linked-pair form, shared by the Add a port dialog and the standalone
/// Connect two programs dialog.
///
/// The current adapter cannot choose an advertised driver port name before
/// the port is created.  The form therefore shows the real free COM ports it
/// found and creates through `create_auto_port`, never by pretending a name
/// was honoured.  On POSIX the paths are chosen when the pair is created.
class PairPanel final : public QWidget
{
    Q_OBJECT

public:
    PairPanel(StudioBackend& backend, bool advanced, platform_kind platform,
              QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    bool is_advanced() const noexcept { return advanced_; }

    /// Refreshes the free-port suggestions and the driver warning.
    void refresh();
    bool can_create() const noexcept { return can_create_; }
    /// Shows inline errors for empty or invalid fields.  Returns false and
    /// leaves every value untouched.
    bool validate();

    /// Returns the error in `error` when creation fails.  Any port created
    /// before the failure is removed again, so a failed call changes nothing.
    std::optional<pair_outcome> create_pair(flow_error* error = nullptr);

    /// The two halves of `create_pair`.  `prepare_pair` reads and checks the
    /// form on the GUI thread.  `run_pair` makes the ports and the link, which
    /// takes seconds where Windows installs a device, so it may run on a
    /// worker thread: it touches no widget.  The caller refreshes the backend
    /// afterwards on the GUI thread.
    std::optional<pair_request> prepare_pair(flow_error* error = nullptr);
    std::optional<pair_outcome> run_pair(const pair_request& request,
                                         flow_error* error = nullptr);

    QLineEdit* pair_name_edit() const noexcept { return pair_name_; }
    QLineEdit* first_port_edit() const noexcept { return first_port_; }
    QLineEdit* second_port_edit() const noexcept { return second_port_; }
    ResultSentence* result_sentence() const noexcept { return result_; }
    NoteBanner* driver_note() const noexcept { return driver_note_; }
    NoteBanner* admin_note() const noexcept { return admin_note_; }
    QComboBox* wiring_combo() const noexcept { return wiring_; }
    QString suggested_first() const { return first_port_->text(); }
    QString suggested_second() const { return second_port_->text(); }

Q_SIGNALS:
    void state_changed();
    void advanced_requested();
    void setup_check_requested();

private:
    struct free_pair
    {
        QString first;
        QString second;
    };

    void build(bool advanced);
    void refresh_result();
    void refresh_warning();
    QVector<free_pair> free_pairs() const;
    void apply_dashed_frame(QFrame* frame);

    StudioBackend& backend_;
    platform_kind platform_;
    bool advanced_ = false;
    bool can_create_ = false;
    bool driver_available_ = false;
    bool studio_driver_ = false;
    int free_pair_count_ = 0;

    QLineEdit* pair_name_ = nullptr;
    QLineEdit* first_port_ = nullptr;
    QLineEdit* second_port_ = nullptr;
    FormRow* pair_name_row_ = nullptr;
    FormRow* first_port_row_ = nullptr;
    FormRow* second_port_row_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* first_diagram_ = nullptr;
    QLabel* second_diagram_ = nullptr;
    ResultSentence* result_ = nullptr;
    NoteBanner* driver_note_ = nullptr;
    NoteBanner* admin_note_ = nullptr;
    QFrame* more_simple_ = nullptr;
    Disclosure* more_advanced_ = nullptr;
    QComboBox* wiring_ = nullptr;
    QComboBox* kind_ = nullptr;
};

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_PAIR_PANEL_HPP
