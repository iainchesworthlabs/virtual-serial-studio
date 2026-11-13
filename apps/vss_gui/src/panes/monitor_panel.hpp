//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_MONITOR_PANEL_HPP
#define VSS_GUI_PANES_MONITOR_PANEL_HPP

#include "backend/studio_backend.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QHBoxLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace vss::gui
{

class MonitorModel;
class PaneHeader;
class SegmentedControl;

/// Monitor dock: bounded traffic table, Advanced filters and Log/JSON tabs.
class MonitorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorPanel(StudioBackend& backend, QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    void set_compact(bool compact);
    /// The shell's WP7 Log/JSON docks can replace the in-panel tabs.
    void set_show_advanced_tabs(bool show);
    void set_selected_port(vss::endpoint_id id);
    void refresh_ports();
    void handle_events(const event_batch& events);

    /// The shortest time between two additions of rows to the table, unless
    /// `set_commit_interval` changed it.
    static constexpr int kCommitIntervalMs = 33;
    /// Changes that time. Rows that are waiting wait for the new time.
    void set_commit_interval(int milliseconds);

    /// Lets the Advanced controls share the pane's title row. They sit there
    /// while the row is wide enough for all of them, and drop to a row of
    /// their own under it when it is not.
    void attach_header(PaneHeader* header);
    /// True while the Advanced controls are in the title row.
    bool controls_in_header() const noexcept { return in_header_; }

    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    void port_selected(vss::endpoint_id id);
    void status_message(const QString& message);
    void error_message(const QString& message);

private:
    void build();
    void update_actions();
    void update_empty();
    void update_filters();
    void update_drop_rows();
    void append_log(const gui_event& event);
    void toggle_pause();
    void export_visible();
    void export_capture();
    void follow_if_enabled();
    /// Puts the rows the model has queued in the table now.
    void commit_rows();
    /// Puts them there now if the last time is long enough ago, and when it
    /// will be if not.
    void schedule_commit();
    void place_controls();

    StudioBackend& backend_;
    PaneHeader* header_ = nullptr;
    QVBoxLayout* page_layout_ = nullptr;
    QHBoxLayout* advanced_layout_ = nullptr;
    bool in_header_ = false;
    MonitorModel* model_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* monitor_page_ = nullptr;
    QStackedWidget* content_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* empty_ = nullptr;
    QWidget* simple_bar_ = nullptr;
    QWidget* advanced_bar_ = nullptr;
    QComboBox* port_filter_ = nullptr;
    QLineEdit* search_ = nullptr;
    QCheckBox* rx_ = nullptr;
    QCheckBox* tx_ = nullptr;
    QCheckBox* drop_ = nullptr;
    QCheckBox* hex_ = nullptr;
    QCheckBox* follow_ = nullptr;
    SegmentedControl* mode_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* simple_pause_ = nullptr;
    QPushButton* clear_ = nullptr;
    QPushButton* export_ = nullptr;
    QPushButton* export_capture_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel* json_ = nullptr;
    QLabel* all_ports_ = nullptr;
    QHash<vss::link_id, std::uint64_t> previous_drops_;
    bool advanced_ = false;
    bool follow_enabled_ = true;
    bool refreshing_ports_ = false;
    bool show_advanced_tabs_ = true;
    QStringList port_signature_;
    QTimer* commit_timer_ = nullptr;
    int commit_interval_ms_ = kCommitIntervalMs;
    QElapsedTimer last_commit_;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_MONITOR_PANEL_HPP
