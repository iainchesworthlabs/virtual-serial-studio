//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words for the state of a network client's connection.  The ports list,
// the Inspector and the topology all say it the same way.  The translation
// context is "Connection".
//

#ifndef VSS_GUI_BACKEND_CONNECTION_TEXT_HPP
#define VSS_GUI_BACKEND_CONNECTION_TEXT_HPP

#include "backend/view_types.hpp"

#include <QString>

namespace vss::gui::connection_text
{

/// One word: "Connecting", "Connected", "Failed" or "Retrying".
QString state(connect_phase phase);

/// A sentence with the target and, for a failure, the reason: "Could not
/// connect to tcp://192.168.1.50:4001: Connection refused. Trying again."
QString sentence(const connection_view& connection);

/// Lines of a connection's history.
QString event_started();
QString event_stopped();
QString event_connected();
QString event_lost();
/// "Connection refused (system:10061)"; the code is left out when empty.
QString event_error(const QString& text, const QString& code);
/// The last error with its code, for the Inspector.
QString error_with_code(const connection_view& connection);

} // namespace vss::gui::connection_text

#endif // VSS_GUI_BACKEND_CONNECTION_TEXT_HPP
