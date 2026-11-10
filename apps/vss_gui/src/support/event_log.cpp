//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/event_log.hpp"

#include <QTime>

namespace vss::gui::support
{
namespace
{

QString direction_text(vss::data_direction direction)
{
    return direction == vss::data_direction::rx ? QStringLiteral("RX")
                                                : QStringLiteral("TX");
}

} // namespace

EventLog::EventLog(QObject* parent) : QObject(parent) {}

void EventLog::append(const event_batch& events)
{
    const QString stamp =
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    for (const gui_event& event : events)
        append(event, stamp);
}

void EventLog::append(const gui_event& event)
{
    append(event,
           QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")));
}

void EventLog::append(const gui_event& event, const QString& stamp)
{
    // `stats` fires for every counter change; the log keeps the meaningful
    // state changes instead of a counter stream.
    if (event.kind == vss::event_kind::stats)
        return;
    const QString line = format(event, stamp);
    if (line.isEmpty())
        return;
    lines_.append(line);
    while (lines_.size() > kMaxLines)
        lines_.removeFirst();
}

QString EventLog::text() const
{
    return lines_.join(QLatin1Char('\n'));
}

void EventLog::clear()
{
    lines_.clear();
}

QString EventLog::format(const gui_event& event)
{
    return format(
        event, QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")));
}

QString EventLog::format(const gui_event& event, const QString& stamp)
{
    const QString name = event.name.isEmpty()
                             ? QStringLiteral("port %1").arg(event.endpoint)
                             : event.name;
    switch (event.kind)
    {
    case vss::event_kind::started:
        return QStringLiteral("[%1] %2 opened").arg(stamp, name);
    case vss::event_kind::stopped:
        return QStringLiteral("[%1] %2 closed").arg(stamp, name);
    case vss::event_kind::data:
        return QStringLiteral("[%1] %2 %3 %4 bytes")
            .arg(stamp, name, direction_text(event.direction))
            .arg(static_cast<qulonglong>(event.data.size()));
    case vss::event_kind::lines:
        return QStringLiteral("[%1] %2 control lines changed").arg(stamp, name);
    case vss::event_kind::error:
    {
        const QString detail =
            event.error.isEmpty() ? event.message : event.error;
        return QStringLiteral("[%1] %2 error: %3").arg(stamp, name, detail);
    }
    case vss::event_kind::log_message:
        if (event.message.isEmpty())
            return {};
        return QStringLiteral("[%1] %2 %3").arg(stamp, name, event.message);
    case vss::event_kind::bus_fault:
        return QStringLiteral("[%1] %2 bus: %3")
            .arg(stamp, name, event.message);
    case vss::event_kind::stats:
        return {};
    }
    return {};
}

} // namespace vss::gui::support
