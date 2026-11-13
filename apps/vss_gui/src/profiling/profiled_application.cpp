//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "profiling/profiled_application.hpp"

#include "profiling/stall_watchdog.hpp"

#include <vss/core/profile.hpp>

#include <QAbstractEventDispatcher>
#include <QEvent>
#include <QMetaEnum>
#include <QMetaObject>
#include <QObject>
#include <QWidget>

#include <chrono>
#include <string>
#include <unordered_map>

namespace vss::gui
{
namespace
{

/// The name of an event type, looked up once per thread.
const std::string& event_name(int type)
{
    thread_local std::unordered_map<int, std::string> names;
    auto found = names.find(type);
    if (found == names.end())
    {
        const QMetaEnum meta = QMetaEnum::fromType<QEvent::Type>();
        const char* key = meta.valueToKey(static_cast<quint64>(type));
        found =
            names
                .emplace(type, key != nullptr ? std::string(key)
                                              : "Event#" + std::to_string(type))
                .first;
    }
    return found->second;
}

bool is_input(int type) noexcept
{
    switch (type)
    {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        return true;
    default:
        return false;
    }
}

bool is_paint(int type) noexcept
{
    return type == QEvent::Paint || type == QEvent::UpdateRequest ||
           type == QEvent::UpdateLater || type == QEvent::Resize ||
           type == QEvent::Show;
}

} // namespace

int ProfiledApplication::budget_ms(int event_type) noexcept
{
    return (is_input(event_type) || is_paint(event_type)) ? 16 : 50;
}

bool ProfiledApplication::notify(QObject* receiver, QEvent* event)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.event");
    if (receiver == nullptr || event == nullptr)
        return QApplication::notify(receiver, event);

    // The stall watchdog reads this from its own thread to say what the GUI
    // thread was doing when it stopped answering: two relaxed stores per
    // event, with the outer event put back when a nested one ends.
    const current_event_snapshot outer = current_event();
    publish_current_event(receiver->metaObject()->className(),
                          static_cast<int>(event->type()));
    struct restore
    {
        current_event_snapshot previous;
        ~restore()
        {
            publish_current_event(previous.receiver_class,
                                  previous.event_type);
        }
    } const restore_outer{outer};
    if (!zone.active())
        return QApplication::notify(receiver, event);

    // Read everything needed from the receiver and the event up front: either
    // may be deleted while the event is delivered.
    const int type = static_cast<int>(event->type());
    const char* class_name = receiver->metaObject()->className();
    const bool top_level_paint = type == QEvent::Paint &&
                                 receiver->isWidgetType() &&
                                 static_cast<QWidget*>(receiver)->isWindow();
    std::string label(class_name);
    label += " / ";
    label += event_name(type);
    if (!receiver->objectName().isEmpty())
    {
        label += " / ";
        label += receiver->objectName().toStdString();
    }
    zone.text(label);

    const auto started = std::chrono::steady_clock::now();
    const bool handled = QApplication::notify(receiver, event);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();

    if (elapsed_ms >= 1.0)
        VSS_PROFILE_PLOT_F("gui.event_ms", elapsed_ms);
    if (elapsed_ms > static_cast<double>(budget_ms(type)))
    {
        VSS_PROFILE_MESSAGE_TEXT(
            "gui.slow_event " + label + " " +
            std::to_string(static_cast<long long>(elapsed_ms)) + " ms");
    }
    if (top_level_paint)
        VSS_PROFILE_FRAME_NAMED("gui.paint");
    return handled;
}

void ProfiledApplication::install_loop_marks()
{
    auto* dispatcher = QAbstractEventDispatcher::instance();
    if (dispatcher == nullptr)
        return;
    // A pass of the loop runs from `awake` to `aboutToBlock`; the gap in
    // between is idle time. Tracy shows the busy periods as a discontinuous
    // frame set, and the continuous main frame ticks once per pass.
    QObject::connect(
        dispatcher, &QAbstractEventDispatcher::awake, dispatcher,
        []() { VSS_PROFILE_FRAME_START("gui.loop"); }, Qt::DirectConnection);
    QObject::connect(
        dispatcher, &QAbstractEventDispatcher::aboutToBlock, dispatcher,
        []()
        {
            VSS_PROFILE_FRAME_END("gui.loop");
            VSS_PROFILE_FRAME();
        },
        Qt::DirectConnection);
}

} // namespace vss::gui
