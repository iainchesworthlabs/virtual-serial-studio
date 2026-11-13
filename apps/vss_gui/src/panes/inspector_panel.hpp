//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_INSPECTOR_PANEL_HPP
#define VSS_GUI_PANES_INSPECTOR_PANEL_HPP

#include "backend/studio_backend.hpp"

#include <QWidget>

#include <array>
#include <optional>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace vss::gui
{

class CopyButton;
class FixtureBehaviours;
class FixturePanel;
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
    void error_message(const QString& message);

private:
    void build();
    void update_content();
    void update_lines();
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
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_INSPECTOR_PANEL_HPP
