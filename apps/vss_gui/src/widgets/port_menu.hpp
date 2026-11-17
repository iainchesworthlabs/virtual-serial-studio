//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QObject;

namespace vss::gui
{

/// What the Send, Monitor and fixture actions of a port's menu can do.
enum class port_menu_action
{
    send_bytes,
    toggle_repeat,
    open_monitor,
    decode_as,
    toggle_generator,
    toggle_echo
};

/// What decides which of those actions a port's menu shows and how they read.
/// The window fills it in; the Ports list and the Topology only draw it.
struct port_menu_state
{
    /// Advanced mode: the repeat is an Advanced control.
    bool advanced = false;
    /// The port is a mock fixture: it gets the generator and echo.
    bool fixture = false;
    /// A repeating send is running from this port.
    bool repeating = false;
    /// The byte generator of this fixture is running.
    bool generating = false;
    /// The fixture echoes what it receives.
    bool echoing = false;
    /// The build can echo (`Capabilities::fixture_echo`).
    bool echo_available = false;
    /// "Decode as..." is shown (Advanced): a plugin with a decoder is
    /// installed and on, or was and cannot be used now.
    bool decode_shown = false;
    /// "Decode as..." can be used now. When it cannot, `decode_reason` is its
    /// tooltip.
    bool decode_enabled = false;
    QString decode_reason;
};

/// The labels `add_port_menu_actions` adds, in order.  One place for the
/// strings; the translation context is "PortMenu".
QStringList port_menu_labels(const port_menu_state& state);

/// Adds Send bytes, Start or Stop repeat (Advanced), Open in monitor, and for a
/// fixture Start or Stop generator and Echo.  `trigger` receives the action
/// chosen.  Echo is disabled with "Not available in this build" when the build
/// cannot do it.
void add_port_menu_actions(QMenu* menu, QObject* context,
                           const port_menu_state& state,
                           std::function<void(port_menu_action)> trigger);

} // namespace vss::gui
