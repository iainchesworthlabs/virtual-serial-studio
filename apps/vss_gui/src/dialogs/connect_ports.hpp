//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_DIALOGS_CONNECT_PORTS_HPP
#define VSS_GUI_DIALOGS_CONNECT_PORTS_HPP

#include "backend/studio_backend.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLayout;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace vss::gui
{

class HalfDuplexFields;
class InlineError;
class NoteBanner;
class PickCard;
class ResultSentence;

/// Topology, Connect ports (Ctrl+L).  The six link shapes with per-mode
/// validation, live result sentences and the engine's cycle refusal.  The
/// shared bus is offered where `Capabilities::shared_bus` is on and shown
/// disabled otherwise.
class ConnectPortsDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class mode
    {
        two_way = 0,
        redirect,
        monitor,
        split,
        join,
        bus
    };

    explicit ConnectPortsDialog(StudioBackend& backend,
                                vss::endpoint_id preselected = 0,
                                bool advanced = false,
                                QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    bool advanced() const noexcept { return advanced_; }

    mode current_mode() const noexcept { return mode_; }
    /// Selects a mode unless it is disabled for the chosen pair.
    bool set_mode(mode value);

    void reload_ports();
    bool primary_enabled() const;
    QString count_text() const;
    QString result_text() const;
    QString accessible_result_text() const;
    QString block_reason() const;
    bool two_way_allowed() const;

    /// Runs the backend operation.  On success the dialog accepts and records
    /// the link; on failure it stays open with the error pattern shown.
    bool submit();

    std::optional<vss::link_id> created_link() const { return result_link_; }
    QString result_summary() const { return result_summary_; }
    QString last_error() const;
    QString error_title() const;
    QString error_body() const;
    bool error_visible() const;

    vss::endpoint_id from_id() const;
    vss::endpoint_id to_id() const;
    void select_from(vss::endpoint_id id);
    void select_to(vss::endpoint_id id);
    vss::endpoint_id split_source_id() const;
    void select_split_source(vss::endpoint_id id);
    QVector<vss::endpoint_id> target_ids() const;
    void set_target_ids(const QVector<vss::endpoint_id>& ids);
    vss::endpoint_id join_target_id() const;
    void select_join_target(vss::endpoint_id id);
    QVector<vss::endpoint_id> source_ids() const;
    void set_source_ids(const QVector<vss::endpoint_id>& ids);
    QVector<vss::endpoint_id> member_ids() const;
    void set_member_ids(const QVector<vss::endpoint_id>& ids);
    QCheckBox* owner_check() const { return owner_check_; }
    /// The half-duplex line of the bus page; shown at the Advanced level.
    HalfDuplexFields* half_duplex_fields() const { return half_duplex_; }
    QPushButton* primary_button() const { return primary_; }

Q_SIGNALS:
    void advanced_requested();

private:
    void build();
    QWidget* build_modes();
    QWidget* build_pair_page();
    QWidget* build_split_page();
    QWidget* build_join_page();
    QWidget* build_bus_page();
    void build_footer(QVBoxLayout* outer);
    void add_mode_card(QLayout* layout, mode value, const QString& title,
                       const QString& description, const QString& glyph);
    void rebuild_split_list();
    void rebuild_join_list();
    void rebuild_bus_list();
    void rebuild_checklist(QWidget* card, QVBoxLayout* card_layout, bool split,
                           QVector<vss::endpoint_id>& list,
                           vss::endpoint_id excluded,
                           const QString& excluded_reason);
    void choose_first_two(QVector<vss::endpoint_id>& list,
                          vss::endpoint_id avoid) const;
    void on_mode_selected(mode value);
    void on_ports_changed();
    void update_ui();
    void update_result();
    void update_count();
    void clear_error();
    void show_backend_error(const vss::error_code& error, const QString& from,
                            const QString& to);
    std::optional<port_view> find_port(vss::endpoint_id id) const;
    QVector<vss::endpoint_id> checked_ids(mode kind) const;
    QString names_for(const QVector<vss::endpoint_id>& ids) const;
    QString single_name(vss::endpoint_id id) const;

    StudioBackend& backend_;
    bool advanced_ = false;
    bool updating_ = false;
    mode mode_ = mode::two_way;
    std::vector<port_view> ports_;
    vss::endpoint_id preselected_ = 0;
    QVector<vss::endpoint_id> split_targets_;
    QVector<vss::endpoint_id> join_sources_;
    QVector<vss::endpoint_id> bus_members_;

    PickCard* cards_[6] = {};
    QButtonGroup* card_group_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QComboBox* from_combo_ = nullptr;
    QComboBox* to_combo_ = nullptr;
    QComboBox* split_source_combo_ = nullptr;
    QComboBox* join_target_combo_ = nullptr;
    QWidget* split_card_ = nullptr;
    QVBoxLayout* split_layout_ = nullptr;
    QWidget* join_card_ = nullptr;
    QVBoxLayout* join_layout_ = nullptr;
    QWidget* bus_card_ = nullptr;
    QVBoxLayout* bus_layout_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* join_count_ = nullptr;
    QLabel* bus_count_ = nullptr;
    NoteBanner* block_note_ = nullptr;
    InlineError* error_ = nullptr;
    ResultSentence* result_ = nullptr;
    QCheckBox* owner_check_ = nullptr;
    HalfDuplexFields* half_duplex_ = nullptr;
    QPushButton* advanced_button_ = nullptr;
    QPushButton* primary_ = nullptr;

    std::optional<vss::link_id> result_link_;
    QString result_summary_;
    QString last_error_;
};

} // namespace vss::gui

#endif // VSS_GUI_DIALOGS_CONNECT_PORTS_HPP
