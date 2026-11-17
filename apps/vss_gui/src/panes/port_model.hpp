//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_PANES_PORT_MODEL_HPP
#define VSS_GUI_PANES_PORT_MODEL_HPP

#include "backend/view_types.hpp"

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace vss::gui
{

/// The six state words plus the two distinctive port kinds.
enum class port_state_kind
{
    open,
    waiting,
    connected,
    studio_only,
    closed,
    failed,
    fixture,
    /// Held by the user for a while; see `port_view::paused`.
    paused
};

/// One row per port.  Simple and Advanced share the model; only the view
/// changes.  No widgets are created per row.
class PortModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum role
    {
        port_id_role = Qt::UserRole + 1,
        name_role,
        role_text_role,
        path_role,
        state_text_role,
        state_kind_role,
        bytes_rx_role,
        bytes_tx_role,
        dropped_role,
        serial_role,
        status_role,
        format_warning_role
    };

    /// The columns. The first five are the default view; the rest are offered
    /// by the header menu. The numbers are fixed, because saved settings name
    /// columns by them.
    enum table_column
    {
        port_column,
        state_column,
        rx_column,
        tx_column,
        dropped_column,
        kind_column,
        address_column,
        settings_column,
        baud_column,
        data_bits_column,
        parity_column,
        stop_bits_column,
        flow_control_column,
        errors_column,
        column_count
    };

    explicit PortModel(QObject* parent = nullptr);

    void set_snapshot(std::vector<port_view> ports,
                      std::vector<link_view> links);
    void set_filter(const QString& filter);
    /// Orders the rows by a column, or in the engine's order with -1. Ties
    /// keep the engine's order, and a value the engine does not know (a dash)
    /// always comes last, whichever way the column is sorted.
    void set_sort(int column, Qt::SortOrder order);
    int sort_column() const noexcept { return sort_column_; }
    Qt::SortOrder sort_order() const noexcept { return sort_order_; }
    QString filter() const { return filter_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    vss::endpoint_id port_id(int row) const;
    int row_for_id(vss::endpoint_id id) const;
    std::optional<port_view> port_at(int row) const;
    const std::vector<port_view>& ports() const noexcept { return ports_; }

    static QString state_text(const port_view& port);
    static port_state_kind state_kind(const port_view& port);
    /// Where a port's state sorts, first to last: failed, waiting, open,
    /// connected, Studio only, fixture, closed, then a port paused by the user
    /// and a port stopped on purpose last. The Monitor's Pause is not a
    /// port's.
    static int state_rank(const port_view& port);
    static QString role_text(const port_view& port,
                             const std::vector<port_view>& ports,
                             const std::vector<link_view>& links);
    static QString serial_text(const port_view& port);
    /// What the cell of `column` shows for `port`; a dash where the engine
    /// does not know the value for this kind of port.
    static QString cell_text(const port_view& port, int column);
    /// The kind of endpoint in words, such as "Network client".
    static QString kind_text(const port_view& port);
    /// True for the columns that hold numbers, which are right aligned.
    static bool is_numeric_column(int column);
    /// What the engine knows about the other side of the port, in one line: the
    /// baud rate and framing a program set, who is connected to a listener, and
    /// a warning when the port runs at a different speed from the others on its
    /// link.  Empty when there is nothing to say; the GUI never claims a speed
    /// it was not told.
    static QString status_text(const port_view& port);
    /// True when #status_text carries the mismatch warning.
    static bool has_format_warning(const port_view& port);

private:
    /// The rows to show for these ports, in the order to show them.
    QVector<int> compute_visible(const std::vector<port_view>& ports,
                                 const std::vector<link_view>& links) const;
    bool matches(const port_view& port, const std::vector<port_view>& ports,
                 const std::vector<link_view>& links) const;
    QVariant sort_key(const port_view& port, int column) const;
    bool sorts_before(const port_view& left, const port_view& right) const;
    QVector<qulonglong> visible_ids() const;
    /// Puts `next` in place of the shown rows when they are the same rows in
    /// another order, so selection and the current cell follow their rows.
    void reorder_visible(QVector<int> next);

    /// What each visible row showed when it was last published, one string
    /// per column and a last one for what only the row delegate reads, so an
    /// update tells the views which cells changed.
    QStringList row_signature(const port_view& port) const;
    void publish_changes(const std::vector<QStringList>& before);

    std::vector<port_view> ports_;
    std::vector<link_view> links_;
    QVector<int> visible_;
    QString filter_;
    int sort_column_ = -1;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;
};

} // namespace vss::gui

#endif // VSS_GUI_PANES_PORT_MODEL_HPP
