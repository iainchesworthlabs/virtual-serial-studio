//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_MONITOR_PANEL_HPP
#define VSS_GUI_PANES_MONITOR_PANEL_HPP

#include "backend/studio_backend.hpp"
#include "panes/monitor_cell_delegate.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QStringList>
#include <QWidget>

#include <array>
#include <cstdint>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QHBoxLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStandardItemModel;
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

    /// The traffic table, for tests.
    QTableView* table() const noexcept { return table_; }
    /// Smallest a table column can be dragged to (a 24 px target and more).
    /// The same as the Ports table's.
    static constexpr int kMinimumColumnWidth = 48;
    /// Writes the widths the user set now rather than after the usual short
    /// delay.
    void save_column_widths() const;

Q_SIGNALS:
    void port_selected(vss::endpoint_id id);
    void status_message(const QString& message);
    void error_message(const QString& message);

protected:
    void changeEvent(QEvent* event) override;

private:
    void build();
    /// Sets everything that depends on the density, the text size and the
    /// language: the fonts and padding of the cells, the height of the rows
    /// and of the header, and the widths of the columns.
    void apply_metrics();
    /// Sets the widths of the columns from their headings and contents, or
    /// from what the user chose.
    void apply_column_widths();
    void restore_column_widths();
    /// The width a column's own delegate asks for `sample`.
    int cell_width(int column, const QString& sample);
    /// Widens or narrows a column by `delta` pixels, from the keyboard.
    void resize_column_by_key(int column, int delta);
    /// Gives a column back the width its heading and contents ask for.
    void fit_column_by_key(int column);
    void report_column_width(int column);
    /// The width of a "0" in the cell font, which is the unit a width the user
    /// set is kept in.
    double zero_width() const;
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
    monitor_cell_style cell_style_;
    MonitorCellDelegate* cell_delegate_ = nullptr;
    /// One row of sample texts, for measuring what a column has to show.
    QStandardItemModel* measure_model_ = nullptr;
    QTimer* column_save_timer_ = nullptr;
    /// The width the user gave each of the first four columns, as a number of
    /// "0" characters of the data font, so it still fits after the text size
    /// changes; 0 means not set, and the column follows its heading and
    /// contents.
    std::array<double, 4> user_widths_ = {};
    bool applying_widths_ = false;
    bool applying_metrics_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_MONITOR_PANEL_HPP
