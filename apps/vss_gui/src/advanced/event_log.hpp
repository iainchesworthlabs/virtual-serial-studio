//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_ADVANCED_EVENT_LOG_HPP
#define VSS_GUI_ADVANCED_EVENT_LOG_HPP

#include "backend/view_types.hpp"

#include <QString>
#include <QStringList>
#include <QWidget>

class QAction;
class QGridLayout;
class QToolButton;
class QComboBox;
class QEvent;
class QLineEdit;
class QLabel;
class QPushButton;
class QTableWidget;

namespace vss::gui
{

class PaneHeader;

/// The Advanced Log tab: state changes and errors since the setup was opened.
///
/// It is fed by the backend's coalesced batch on the GUI thread and keeps a
/// fixed row cap.  Traffic rows live in the Monitor, not here; the engine
/// publishes no drop event, so this log makes no claim about owner drops.
///
/// Its table is laid out as the Monitor's is: rows, heading, cell fonts and
/// padding follow the density (through `dense_table.hpp`) and the text size.
class EventLogPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit EventLogPanel(QWidget* parent = nullptr);

    void append(const event_batch& events);
    /// A GUI-side event, for example "Applied the topology JSON.".
    void add_local(const QString& text);
    void clear();

    /// Puts the filters and Clear on the pane's title row while it is wide
    /// enough, as the Monitor does, and in a bar under it when it is not.
    /// Without a header they stay in a slim bar above the table.
    void attach_header(PaneHeader* header);

    int row_count() const;

    QTableWidget* table() const noexcept { return table_; }
    QLabel* empty_label() const noexcept { return empty_; }
    QPushButton* clear_button() const noexcept { return clear_; }

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void add_row(const QString& time, const QString& port, const QString& event,
                 const QString& detail, int category);
    void update_empty();
    void place_controls();
    /// The width the controls need on one row.
    int single_row_width() const;
    /// One row, or the search box on a second row of its own.
    void arrange(bool two_rows);
    /// Shows the rows the port, the kinds and the search leave in.
    void apply_filters();
    bool row_shown(int row) const;
    void note_port(const QString& port);
    /// Row height, heading height, fonts, padding and column widths from the
    /// theme's density and the text size; runs when either changes.
    void apply_metrics();

    QTableWidget* table_ = nullptr;
    QLabel* empty_ = nullptr;
    QWidget* bar_ = nullptr;
    QPushButton* clear_ = nullptr;
    QComboBox* port_filter_ = nullptr;
    QToolButton* kinds_ = nullptr;
    QAction* errors_ = nullptr;
    QAction* started_ = nullptr;
    QAction* stopped_ = nullptr;
    QAction* other_ = nullptr;
    QGridLayout* bar_layout_ = nullptr;
    bool two_rows_ = false;
    QLineEdit* search_ = nullptr;
    PaneHeader* header_ = nullptr;
    bool in_header_ = false;
    QStringList ports_;
    bool applying_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_ADVANCED_EVENT_LOG_HPP
