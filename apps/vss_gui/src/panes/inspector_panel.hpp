//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_INSPECTOR_PANEL_HPP
#define VSS_GUI_PANES_INSPECTOR_PANEL_HPP

#include "backend/plugin_part_view.hpp"
#include "backend/studio_backend.hpp"

#include <QWidget>

#include <array>
#include <optional>

class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace vss::gui
{

class CopyButton;
class FixtureBehaviours;
class FixturePanel;
class NoteBanner;
class StatusChip;
class Switch;

/// The Inspector dock: state, path, traffic, help and control lines.
class InspectorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorPanel(StudioBackend& backend, QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    void set_selected_port(vss::endpoint_id id);
    /// What the ports that a plugin provides say about their plugins, from
    /// the shell. The selected port's is shown as "Provided by", its note and
    /// its action; a port with none shows nothing of it.
    void set_plugin_parts(
        const QHash<vss::endpoint_id, vss::gui::part_description>& parts);
    /// The "Provided by" block, hidden for a port no plugin provides.
    QWidget* plugin_block() const noexcept { return plugin_block_; }
    NoteBanner* plugin_note() const noexcept { return plugin_note_; }
    /// The "monitor" block, hidden for a port that is not a monitor.
    QWidget* monitor_block() const noexcept { return monitor_block_; }
    QPushButton* monitor_open_button() const noexcept { return monitor_open_; }
    QComboBox* monitor_direction_combo() const noexcept
    {
        return monitor_direction_;
    }
    QPushButton* plugin_show_button() const noexcept { return plugin_show_; }
    void refresh();
    /// Adds the fixture behaviour section (echo, generator) for a mock
    /// fixture in Advanced mode.
    void set_behaviours(FixtureBehaviours* behaviours);

    /// "Change..." beside the COM number of a driver port.
    QPushButton* com_change_button() const noexcept { return com_change_; }
    /// "Release port..." (Advanced), for a driver port a program has open.
    QPushButton* release_button() const noexcept { return release_; }

Q_SIGNALS:
    void troubleshoot_requested();
    void advanced_tools_requested();
    void com_number_requested(vss::endpoint_id id);
    void release_port_requested(vss::endpoint_id id);
    /// The Stop or Start button: `run` is true to start.
    void run_requested(vss::endpoint_id id, bool run);
    /// "Open <port> in the Monitor", for a port that is a monitor.
    void monitor_requested(vss::endpoint_id id);
    void error_message(const QString& message);
    /// "Show in Plugins" or the action of the note: `action` is a
    /// `vss::gui::part_action`.
    void plugin_action_requested(const QString& plugin_id, int action);

private:
    void build();
    void update_content();
    void update_connection_rows(const port_view& port);
    void update_lines();
    void update_plugin_part();
    void update_monitor_block();
    void set_line(vss::control_line line, bool state);
    QString serial_description() const;

    StudioBackend& backend_;
    vss::endpoint_id selected_ = 0;
    std::optional<port_view> port_;
    bool advanced_ = false;
    bool refreshing_ = false;

    QScrollArea* scroll_ = nullptr;
    QWidget* content_ = nullptr;
    QLabel* empty_ = nullptr;
    QLabel* name_ = nullptr;
    StatusChip* state_ = nullptr;
    QLabel* path_ = nullptr;
    CopyButton* copy_ = nullptr;
    QLabel* traffic_ = nullptr;
    QLabel* run_caption_ = nullptr;
    QPushButton* run_button_ = nullptr;
    /// The rows of a network client's connection, in the order shown; all
    /// hidden for any other port.  Each is a caption and its value.
    struct connection_row
    {
        QLabel* caption = nullptr;
        QLabel* value = nullptr;
    };
    connection_row remote_row_;
    connection_row local_row_;
    connection_row connected_row_;
    connection_row error_row_;
    connection_row next_try_row_;
    connection_row history_row_;
    connection_row received_row_;
    connection_row sent_row_;
    connection_row reconnects_row_;
    QLabel* com_caption_ = nullptr;
    QWidget* com_row_ = nullptr;
    QLabel* com_value_ = nullptr;
    QPushButton* com_change_ = nullptr;
    QPushButton* release_ = nullptr;
    QLabel* serial_label_ = nullptr;
    QWidget* serial_row_ = nullptr;
    QLabel* help_text_ = nullptr;
    QLabel* help_link_ = nullptr;
    QWidget* simple_lines_ = nullptr;
    QWidget* advanced_lines_ = nullptr;
    std::array<StatusChip*, 6> line_chips_{};
    std::array<StatusChip*, 4> input_chips_{};
    Switch* dtr_ = nullptr;
    Switch* rts_ = nullptr;
    QPushButton* break_ = nullptr;
    QLabel* advanced_link_ = nullptr;
    FixturePanel* fixture_panel_ = nullptr;
    QVBoxLayout* content_layout_ = nullptr;
    QWidget* monitor_block_ = nullptr;
    QLabel* monitor_text_ = nullptr;
    QPushButton* monitor_open_ = nullptr;
    QComboBox* monitor_direction_ = nullptr;
    QLabel* monitor_direction_hint_ = nullptr;
    vss::link_id monitor_link_ = 0;
    vss::endpoint_id watched_ = 0;
    QHash<vss::endpoint_id, vss::gui::part_description> plugin_parts_;
    QWidget* plugin_block_ = nullptr;
    QLabel* plugin_provided_ = nullptr;
    StatusChip* plugin_chip_ = nullptr;
    QPushButton* plugin_show_ = nullptr;
    NoteBanner* plugin_note_ = nullptr;
    QLabel* plugin_kept_ = nullptr;
    QString plugin_id_;
    vss::gui::part_action plugin_note_action_ = vss::gui::part_action::none;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_INSPECTOR_PANEL_HPP
