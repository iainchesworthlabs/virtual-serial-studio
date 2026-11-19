//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/types.hpp>

#include <QString>

class QComboBox;

namespace vss::gui
{

/// The name of a monitor's direction as the choice shows it: "Both
/// directions", "Transmit only" or "Receive only".
QString monitor_direction_name(vss::monitor_direction direction);

/// One plain sentence for what the choice copies.
QString monitor_direction_description(vss::monitor_direction direction);

/// Puts the three choices in `combo`, in that order, with the direction as
/// each entry's data, and selects `current`.
void fill_monitor_direction(QComboBox* combo, vss::monitor_direction current);

} // namespace vss::gui
