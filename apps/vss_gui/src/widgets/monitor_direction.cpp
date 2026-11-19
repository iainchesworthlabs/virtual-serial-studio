//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/monitor_direction.hpp"

#include <QComboBox>
#include <QCoreApplication>

#include <algorithm>

namespace vss::gui
{
QString monitor_direction_name(vss::monitor_direction direction)
{
    switch (direction)
    {
    case vss::monitor_direction::both:
        return QCoreApplication::translate("MonitorDirection",
                                           "Both directions");
    case vss::monitor_direction::transmit:
        return QCoreApplication::translate("MonitorDirection", "Transmit only");
    case vss::monitor_direction::receive:
        return QCoreApplication::translate("MonitorDirection", "Receive only");
    }
    return {};
}

QString monitor_direction_description(vss::monitor_direction direction)
{
    switch (direction)
    {
    case vss::monitor_direction::both:
        return QCoreApplication::translate(
            "MonitorDirection",
            "Copies what the program on the port writes and what is written "
            "into the port.");
    case vss::monitor_direction::transmit:
        return QCoreApplication::translate(
            "MonitorDirection",
            "Copies only what the program on the port writes.");
    case vss::monitor_direction::receive:
        return QCoreApplication::translate(
            "MonitorDirection", "Copies only what is written into the port.");
    }
    return {};
}

void fill_monitor_direction(QComboBox* combo, vss::monitor_direction current)
{
    combo->clear();
    for (const vss::monitor_direction each :
         {vss::monitor_direction::both, vss::monitor_direction::transmit,
          vss::monitor_direction::receive})
    {
        combo->addItem(monitor_direction_name(each), static_cast<int>(each));
        combo->setItemData(combo->count() - 1,
                           monitor_direction_description(each),
                           Qt::ToolTipRole);
    }
    combo->setCurrentIndex(
        std::max(0, combo->findData(static_cast<int>(current))));
}

} // namespace vss::gui
