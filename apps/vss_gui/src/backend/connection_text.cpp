//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/connection_text.hpp"

#include <QCoreApplication>

namespace vss::gui::connection_text
{

QString state(connect_phase phase)
{
    switch (phase)
    {
    case connect_phase::connecting:
        return QCoreApplication::translate("Connection", "Connecting");
    case connect_phase::connected:
        return QCoreApplication::translate("Connection", "Connected");
    case connect_phase::failed:
        return QCoreApplication::translate("Connection", "Failed");
    case connect_phase::retrying:
        return QCoreApplication::translate("Connection", "Retrying");
    case connect_phase::stopped:
        return QCoreApplication::translate("Connection", "Stopped");
    case connect_phase::paused:
        return QCoreApplication::translate("Connection", "Paused");
    }
    return {};
}

QString sentence(const connection_view& connection)
{
    const QString& target = connection.target;
    const QString& reason = connection.reason;
    switch (connection.phase)
    {
    case connect_phase::connecting:
        return QCoreApplication::translate("Connection", "Connecting to %1.")
            .arg(target);
    case connect_phase::connected:
        return QCoreApplication::translate("Connection", "Connected to %1.")
            .arg(target);
    case connect_phase::failed:
        if (reason.isEmpty())
            return QCoreApplication::translate("Connection",
                                               "Not connected to %1.")
                .arg(target);
        return QCoreApplication::translate("Connection",
                                           "Could not connect to %1: %2.")
            .arg(target, reason);
    case connect_phase::stopped:
        return QCoreApplication::translate(
                   "Connection", "Stopped. Not connecting to %1.")
            .arg(target);
    case connect_phase::paused:
        return QCoreApplication::translate(
                   "Connection", "Paused. Not connecting to %1.")
            .arg(target);
    case connect_phase::retrying:
        if (reason.isEmpty())
            return QCoreApplication::translate(
                       "Connection", "Trying to connect to %1.")
                .arg(target);
        return QCoreApplication::translate(
                   "Connection", "Could not connect to %1: %2. Trying again.")
            .arg(target, reason);
    }
    return {};
}

QString event_started()
{
    return QCoreApplication::translate("Connection", "Started");
}

QString event_stopped()
{
    return QCoreApplication::translate("Connection", "Stopped");
}

QString event_paused()
{
    return QCoreApplication::translate("Connection", "Paused");
}

QString event_resumed()
{
    return QCoreApplication::translate("Connection", "Resumed");
}

QString event_connected()
{
    return QCoreApplication::translate("Connection", "Connected");
}

QString event_lost()
{
    return QCoreApplication::translate("Connection", "Connection lost");
}

QString event_error(const QString& text, const QString& code)
{
    if (code.isEmpty())
        return text;
    return QCoreApplication::translate("Connection", "%1 (%2)")
        .arg(text, code);
}

QString error_with_code(const connection_view& connection)
{
    return event_error(connection.reason, connection.error_code);
}

} // namespace vss::gui::connection_text
