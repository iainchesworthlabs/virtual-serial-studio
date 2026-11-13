//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_EVENT_LOG_HPP
#define VSS_GUI_SUPPORT_EVENT_LOG_HPP

#include "backend/view_types.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

namespace vss::gui::support
{

/// A bounded, GUI-side record of state changes, errors and log messages.
///
/// It is fed from the coalesced event batch on the GUI thread and never
/// records payload bytes, so the support bundle's `log.txt` is useful without
/// being a second copy of the traffic.  The support bundle never holds payload
/// bytes.
class EventLog final : public QObject
{
    Q_OBJECT

public:
    explicit EventLog(QObject* parent = nullptr);

    void append(const event_batch& events);
    void append(const gui_event& event);

    QStringList lines() const { return lines_; }
    /// One line per entry, newline separated; empty when nothing happened.
    QString text() const;
    int size() const { return static_cast<int>(lines_.size()); }
    void clear();

    /// The one-line rendering used by text() and by tests.
    static QString format(const gui_event& event);
    /// Same, with the time already rendered, so a batch of thousands of
    /// events reads the clock once.
    static QString format(const gui_event& event, const QString& stamp);

    /// Entries retained before the oldest is dropped.
    static constexpr int kMaxLines = 5000;

private:
    void append(const gui_event& event, const QString& stamp);

    QStringList lines_;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_EVENT_LOG_HPP
