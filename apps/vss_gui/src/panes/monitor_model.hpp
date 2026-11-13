//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_MONITOR_MODEL_HPP
#define VSS_GUI_PANES_MONITOR_MODEL_HPP

#include "backend/view_types.hpp"

#include <vss/studio/capture.hpp>

#include <QAbstractTableModel>
#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace vss::gui
{

enum class traffic_direction
{
    rx,
    tx,
    drop
};

struct traffic_row
{
    QDateTime time;
    vss::endpoint_id endpoint = 0;
    QString port;
    traffic_direction direction = traffic_direction::rx;
    std::uint64_t bytes = 0;
    /// Every byte of the chunk, not only the preview.  The capture export
    /// writes these.
    event_payload data;
    /// The engine's event time (a steady clock), unlike #time, which is the
    /// wall clock when the batch reached the GUI.
    vss::time_point when{};
    bool dropped = false;
    /// A dropped row of a bus: a bus has no owner, so its bytes were not
    /// carried (RTS low, or too far ahead of the line rate).
    bool bus = false;

    /// The bytes the table shows: the first 256.  Valid while the row is.
    QByteArrayView preview() const noexcept;
};

/// The rows a capture export writes, with the wall clock at their offset zero.
struct capture_export
{
    std::vector<vss::capture_record> records;
    std::chrono::system_clock::time_point started;
};

/// Bounded traffic model.  Events arrive in coalesced batches from the
/// backend; the model never owns a widget per row.
class MonitorModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum role
    {
        endpoint_role = Qt::UserRole + 1,
        direction_role,
        dropped_role,
        bytes_role
    };

    /// The design's named cap: 10,000 rows.
    static constexpr int kMaxRows = 10'000;
    static constexpr int kPreviewBytes = 256;
    /// Rows also keep their whole payload for the capture export, so the
    /// payloads kept are bounded as well: the oldest rows go first.
    static constexpr qint64 kMaxRetainedBytes = qint64{32} * 1024 * 1024;

    explicit MonitorModel(QObject* parent = nullptr);

    /// Adds the traffic of a batch to the table: `queue_events` and
    /// `commit_queued` in one step.
    void append_events(const event_batch& events);
    /// Takes the traffic of a batch without telling the views.  The rows get
    /// the time of this call.  Nothing is taken while paused.
    void queue_events(const event_batch& events);
    /// Puts the queued rows in the table, as one insertion.  A view lays out
    /// and paints once for however many batches were queued.
    void commit_queued();
    bool has_queued() const noexcept { return !queued_.empty(); }
    /// `bus` says the link is a shared bus, whose drops are not owner drops.
    void append_drop(const QString& port, std::uint64_t bytes,
                     bool bus = false);
    void clear();
    void set_paused(bool paused);
    bool paused() const noexcept { return paused_; }
    void set_hex(bool hex);
    bool hex() const noexcept { return hex_; }
    void set_filters(vss::endpoint_id port, bool rx, bool tx, bool dropped,
                     const QString& search);
    void set_selected_port(vss::endpoint_id id);
    vss::endpoint_id selected_port() const noexcept { return selected_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    vss::endpoint_id port_at(int row) const;
    const traffic_row* row_at(int row) const;
    int total_rows() const noexcept { return static_cast<int>(rows_.size()); }
    QString export_text() const;
    /// The rows that pass the filters, with every byte and the engine's event
    /// time, as capture records.  Offsets run from the first of those rows.
    capture_export export_capture() const;

    static QString ascii_preview(QByteArrayView data);
    static QString hex_preview(QByteArrayView data);

private:
    void append_row(traffic_row row);
    void rebuild_visible();
    bool matches(const traffic_row& row) const;
    QString preview_for(const traffic_row& row) const;
    /// Drops the oldest rows beyond the caps and returns how many went.
    std::size_t trim();
    /// Tells the views about rows pushed onto `rows_` since it had `before`
    /// rows: the oldest rows that were trimmed leave, the new rows that pass
    /// the filters arrive.  No reset, so a view keeps its scroll position and
    /// does not lay out 10,000 rows again for every batch.
    void commit_appended(std::size_t before);

    std::deque<traffic_row> rows_;
    std::vector<traffic_row> queued_;
    qint64 retained_bytes_ = 0;
    QVector<int> visible_;
    vss::endpoint_id port_filter_ = 0;
    vss::endpoint_id selected_ = 0;
    QString search_;
    bool rx_ = true;
    bool tx_ = true;
    bool dropped_ = true;
    bool paused_ = false;
    bool hex_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_MONITOR_MODEL_HPP
