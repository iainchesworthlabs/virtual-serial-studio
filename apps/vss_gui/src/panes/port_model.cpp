//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/port_model.hpp"

#include "panes/format.hpp"
#include "language/bidi.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/serial_settings.hpp>

#include <QStringList>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString other_endpoint_names(const port_view& port,
                             const std::vector<port_view>& ports,
                             const std::vector<link_view>& links)
{
    const auto name_of = [&ports](vss::endpoint_id id) -> QString
    {
        const auto it =
            std::find_if(ports.begin(), ports.end(),
                         [id](const port_view& view) { return view.id == id; });
        return it == ports.end() ? QString() : it->name;
    };

    QStringList names;
    for (const auto& link : links)
    {
        const bool in_sources =
            std::find(link.sources.begin(), link.sources.end(), port.id) !=
            link.sources.end();
        const bool in_targets =
            std::find(link.targets.begin(), link.targets.end(), port.id) !=
            link.targets.end();
        if (!in_sources && !in_targets)
            continue;
        for (const auto id : link.sources)
        {
            if (id != port.id)
            {
                const QString name = name_of(id);
                if (!name.isEmpty() && !names.contains(name))
                    names << name;
            }
        }
        for (const auto id : link.targets)
        {
            if (id != port.id)
            {
                const QString name = name_of(id);
                if (!name.isEmpty() && !names.contains(name))
                    names << name;
            }
        }
    }
    for (QString& name : names)
        name = isolate_name(name);
    return names.join(QStringLiteral(", "));
}

} // namespace

PortModel::PortModel(QObject* parent) : QAbstractTableModel(parent) {}

void PortModel::set_snapshot(std::vector<port_view> ports,
                             std::vector<link_view> links)
{
    VSS_PROFILE_ZONE("gui.panes.port_model.update");
    VSS_PROFILE_PLOT("gui.panes.port_model.rows", ports.size());
    const bool same_ids =
        ports.size() == ports_.size() &&
        std::equal(ports.begin(), ports.end(), ports_.begin(),
                   [](const port_view& left, const port_view& right)
                   { return left.id == right.id; });
    if (!same_ids)
    {
        beginResetModel();
        ports_ = std::move(ports);
        links_ = std::move(links);
        rebuild_visible();
        endResetModel();
        return;
    }

    // Counters change on every traffic batch.  Keep the rows and selection,
    // and let the views repaint instead of resetting the model.
    ports_ = std::move(ports);
    links_ = std::move(links);
    const QVector<int> previous = visible_;
    rebuild_visible();
    if (visible_ != previous)
    {
        beginResetModel();
        endResetModel();
        return;
    }
    if (!visible_.isEmpty())
    {
        Q_EMIT dataChanged(index(0, 0),
                           index(rowCount() - 1, columnCount() - 1));
    }
}

void PortModel::set_filter(const QString& filter)
{
    if (filter_ == filter)
        return;
    beginResetModel();
    filter_ = filter;
    rebuild_visible();
    endResetModel();
}

void PortModel::rebuild_visible()
{
    VSS_PROFILE_ZONE("gui.panes.port_model.filter");
    visible_.clear();
    visible_.reserve(static_cast<qsizetype>(ports_.size()));
    for (int index = 0; index < static_cast<int>(ports_.size()); ++index)
    {
        if (matches(ports_[static_cast<std::size_t>(index)]))
            visible_.push_back(index);
    }
}

bool PortModel::matches(const port_view& port) const
{
    if (filter_.trimmed().isEmpty())
        return true;
    const QString needle = filter_.trimmed();
    return port.name.contains(needle, Qt::CaseInsensitive) ||
           port.path.contains(needle, Qt::CaseInsensitive) ||
           state_text(port).contains(needle, Qt::CaseInsensitive) ||
           role_text(port, ports_, links_)
               .contains(needle, Qt::CaseInsensitive);
}

int PortModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int PortModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 5;
}

QVariant PortModel::data(const QModelIndex& index, int item_role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const port_view& port =
        ports_[static_cast<std::size_t>(visible_[index.row()])];
    switch (item_role)
    {
    case Qt::DisplayRole:
        switch (index.column())
        {
        case 0:
            return port.name;
        case 1:
            return state_text(port);
        case 2:
            return format_bytes(port.stats.bytes_rx);
        case 3:
            return format_bytes(port.stats.bytes_tx);
        case 4:
            return format_bytes(port.stats.dropped);
        default:
            return {};
        }
    case Qt::TextAlignmentRole:
        if (index.column() >= 2)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ToolTipRole:
    {
        const QString status = status_text(port);
        return status.isEmpty() ? port.path
                                : port.path + QLatin1Char('\n') + status;
    }
    case Qt::AccessibleTextRole:
    {
        const QString text =
            tr("%1, %2, %3")
                .arg(port.name,
                     port.path.isEmpty() ? state_text(port) : port.path,
                     state_text(port));
        const QString status = status_text(port);
        return status.isEmpty() ? text : text + QStringLiteral(", ") + status;
    }
    case port_id_role:
        return QVariant::fromValue<qulonglong>(port.id);
    case name_role:
        return port.name;
    case role_text_role:
        return role_text(port, ports_, links_);
    case path_role:
        return port.path;
    case state_text_role:
        return state_text(port);
    case state_kind_role:
        return static_cast<int>(state_kind(port));
    case bytes_rx_role:
        return format_bytes(port.stats.bytes_rx);
    case bytes_tx_role:
        return format_bytes(port.stats.bytes_tx);
    case dropped_role:
        return format_bytes(port.stats.dropped);
    case serial_role:
        return serial_text(port);
    case status_role:
        return status_text(port);
    case format_warning_role:
        return has_format_warning(port);
    default:
        return {};
    }
}

QVariant PortModel::headerData(int section, Qt::Orientation orientation,
                               int item_role) const
{
    if (orientation != Qt::Horizontal || item_role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, item_role);
    switch (section)
    {
    case 0:
        return tr("Port");
    case 1:
        return tr("State");
    case 2:
        return tr("RX");
    case 3:
        return tr("TX");
    case 4:
        return tr("Dropped");
    default:
        return {};
    }
}

vss::endpoint_id PortModel::port_id(int row) const
{
    if (row < 0 || row >= rowCount())
        return 0;
    return ports_[static_cast<std::size_t>(visible_[row])].id;
}

int PortModel::row_for_id(vss::endpoint_id id) const
{
    for (int row = 0; row < rowCount(); ++row)
    {
        if (port_id(row) == id)
            return row;
    }
    return -1;
}

std::optional<port_view> PortModel::port_at(int row) const
{
    if (row < 0 || row >= rowCount())
        return std::nullopt;
    return ports_[static_cast<std::size_t>(visible_[row])];
}

QString PortModel::state_text(const port_view& port)
{
    if (port.state == vss::endpoint_state::failed)
        return tr("Failed");
    if (port.kind == vss::endpoint_kind::mock)
        return tr("Fixture");
    if (port.studio_only)
        return tr("Studio only");
    if (port.open)
    {
        switch (port.peer)
        {
        case peer_state::not_open:
            return tr("Waiting for program");
        case peer_state::open:
            return tr("Program connected");
        case peer_state::unknown:
            break;
        }
        return tr("Open");
    }
    if (port.state == vss::endpoint_state::closed)
        return tr("Closed");
    return tr("Closed");
}

port_state_kind PortModel::state_kind(const port_view& port)
{
    if (port.state == vss::endpoint_state::failed)
        return port_state_kind::failed;
    if (port.kind == vss::endpoint_kind::mock)
        return port_state_kind::fixture;
    if (port.studio_only)
        return port_state_kind::studio_only;
    if (port.open)
    {
        if (port.peer == peer_state::not_open)
            return port_state_kind::waiting;
        if (port.peer == peer_state::open)
            return port_state_kind::connected;
        return port_state_kind::open;
    }
    return port_state_kind::closed;
}

QString PortModel::role_text(const port_view& port,
                             const std::vector<port_view>& ports,
                             const std::vector<link_view>& links)
{
    const QString others = other_endpoint_names(port, ports, links);
    switch (port.kind)
    {
    case vss::endpoint_kind::mock:
        return tr("Fixture (mock)");
    case vss::endpoint_kind::physical:
        if (!others.isEmpty())
        {
            const QStringList names = others.split(QStringLiteral(", "));
            return tr("Real device, shared with %n programs", "",
                      static_cast<int>(names.size()));
        }
        return tr("Real device");
    case vss::endpoint_kind::tcp:
    case vss::endpoint_kind::udp:
    case vss::endpoint_kind::rfc2217:
        return tr("Network port");
    case vss::endpoint_kind::virtual_port:
        break;
    }
    if (port.studio_only)
        return tr("In-process port. Only Studio can open it");
    if (!others.isEmpty())
        return tr("Virtual port, linked to %1").arg(others);
    return tr("Virtual port");
}

QString PortModel::serial_text(const port_view& port)
{
    if (port.kind != vss::endpoint_kind::physical)
        return {};
    return QString::fromStdString(vss::to_label(port.serial));
}

bool PortModel::has_format_warning(const port_view& port)
{
    return !port.format_conflicts.isEmpty();
}

QString PortModel::status_text(const port_view& port)
{
    QStringList parts;
    // A real device already shows the settings it runs at (serial_text), so
    // the format here is only what a program set on a port Studio made.
    if (port.peer_format && port.kind != vss::endpoint_kind::physical)
        parts << tr("Program set %1")
                     .arg(QString::fromStdString(
                         vss::to_label(*port.peer_format)));
    if (port.clients)
    {
        if (*port.clients == 0)
            parts << tr("No remote connected");
        else
            parts << tr("Clients connected: %1").arg(*port.clients);
    }
    if (has_format_warning(port))
        parts << tr("Differs from %1 in baud rate or framing")
                     .arg(port.format_conflicts.join(QStringLiteral(", ")));
    return parts.join(QStringLiteral(". "));
}

} // namespace vss::gui
