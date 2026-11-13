//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/event_bridge.hpp"

#include <vss/core/hub.hpp>
#include <vss/core/profile.hpp>

#include <QElapsedTimer>
#include <QMetaType>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vss::gui
{

struct EventBridge::state
{
    mutable std::mutex mutex;
    std::deque<vss::endpoint_event> events;
    std::size_t max_events = 8192;
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<bool> accepting{true};
};

/// The names of the endpoints as `QString`, so an event of a port whose name
/// has not changed shares the string of the event before it: no conversion
/// and no memory of its own.
struct EventBridge::name_cache
{
    struct entry
    {
        std::string name;
        QString text;
    };
    std::unordered_map<vss::endpoint_id, entry> names;

    const QString& text(vss::endpoint_id id, const std::string& name)
    {
        // Ports come and go; the cache does not need to remember them all.
        if (names.size() > 1024)
            names.clear();
        entry& known = names[id];
        if (known.name != name || known.text.isNull())
        {
            known.name = name;
            known.text = QString::fromStdString(name);
        }
        return known.text;
    }
};

namespace
{

/// Everything of an event except its bytes.
gui_event convert(const vss::endpoint_event& event, const QString& name)
{
    gui_event out;
    out.kind = event.kind;
    out.endpoint = event.endpoint;
    out.name = name;
    out.direction = event.direction;
    out.lines = event.lines;
    out.message = QString::fromStdString(event.message);
    if (event.error)
        out.error = QString::fromStdString(event.error.message());
    out.when = event.when;
    return out;
}

} // namespace

EventBridge::EventBridge(vss::hub& hub, QObject* parent)
    : EventBridge(hub, options{}, parent)
{
}

EventBridge::EventBridge(vss::hub& hub, options settings, QObject* parent)
    : QObject(parent), hub_(hub), state_(std::make_shared<state>()),
      names_(std::make_unique<name_cache>())
{
    state_->max_events = std::max<std::size_t>(settings.max_events, 1);
    max_batch_ = std::max<std::size_t>(settings.max_batch, 1);
    timer_ = new QTimer(this);
    timer_->setInterval(std::max(settings.interval_ms, 1));
    connect(timer_, &QTimer::timeout, this, &EventBridge::drain);
    qRegisterMetaType<vss::gui::gui_event>("vss::gui::gui_event");
    qRegisterMetaType<vss::gui::event_batch>("vss::gui::event_batch");
}

EventBridge::~EventBridge()
{
    stop();
}

void EventBridge::start()
{
    if (running_)
        return;

    // A restart starts clean: do not deliver anything queued before stop().
    state_->accepting.store(false);
    {
        std::lock_guard lock(state_->mutex);
        state_->events.clear();
    }
    state_->accepting.store(true);

    subscription_ = hub_.subscribe(
        {},
        [state = state_](const vss::endpoint_event& event)
        {
            if (!state->accepting.load())
                return;
            std::lock_guard lock(state->mutex);
            if (state->events.size() >= state->max_events)
            {
                state->events.pop_front();
                state->dropped.fetch_add(1, std::memory_order_relaxed);
            }
            state->events.push_back(event);
            state->delivered.fetch_add(1, std::memory_order_relaxed);
        });

    timer_->start();
    running_ = true;
}

void EventBridge::stop()
{
    state_->accepting.store(false);
    if (subscription_ != 0)
    {
        hub_.unsubscribe(subscription_);
        subscription_ = 0;
    }
    timer_->stop();
    running_ = false;
}

void EventBridge::set_interval(int milliseconds)
{
    timer_->setInterval(std::max(milliseconds, 1));
    if (running_)
        timer_->start();
}

int EventBridge::interval() const noexcept
{
    return timer_->interval();
}

void EventBridge::flush()
{
    Q_ASSERT_X(QThread::currentThread() == thread(), "EventBridge::flush",
               "drain on the GUI thread");
    if (QThread::currentThread() != thread())
        return;
    deliver(std::numeric_limits<std::size_t>::max());
}

std::uint64_t EventBridge::delivered() const noexcept
{
    return state_->delivered.load(std::memory_order_relaxed);
}

std::uint64_t EventBridge::dropped() const noexcept
{
    return state_->dropped.load(std::memory_order_relaxed);
}

std::size_t EventBridge::pending() const
{
    std::lock_guard lock(state_->mutex);
    return state_->events.size();
}

void EventBridge::drain()
{
    QElapsedTimer spent;
    spent.start();
    const bool more = deliver(max_batch_);
    last_slice_ms_ = static_cast<int>(spent.elapsed());
    if (more)
        continue_draining();
}

void EventBridge::continue_draining()
{
    if (continuing_)
        return;
    continuing_ = true;
    // A turn of the event loop of its own, after a pause twice as long as the
    // last slice took, so the window keeps at least two thirds of its time for
    // input, painting and its other timers however fast events arrive.
    const int pause = std::max(1, 2 * last_slice_ms_);
    QTimer::singleShot(pause, this,
                       [this]
                       {
                           continuing_ = false;
                           if (!running_)
                               return;
                           QElapsedTimer spent;
                           spent.start();
                           const bool more = deliver(max_batch_);
                           last_slice_ms_ = static_cast<int>(spent.elapsed());
                           if (more)
                               continue_draining();
                       });
}

bool EventBridge::deliver(std::size_t limit)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.bridge.drain");
    std::deque<vss::endpoint_event> pending;
    bool more = false;
    {
        VSS_PROFILE_ZONE("gui.bridge.take");
        std::lock_guard lock(state_->mutex);
        if (state_->events.size() <= limit)
        {
            pending.swap(state_->events);
        }
        else
        {
            const auto cut =
                state_->events.begin() + static_cast<std::ptrdiff_t>(limit);
            pending.assign(std::make_move_iterator(state_->events.begin()),
                           std::make_move_iterator(cut));
            state_->events.erase(state_->events.begin(), cut);
            more = true;
        }
    }

    if (pending.empty())
        return false;
    // The zone's text is the number of events, so a profile gives the cost of
    // an event as well as of a batch.
    VSS_PROFILE_ZONE_TEXT(zone, std::to_string(pending.size()));
    VSS_PROFILE_PLOT("gui.bridge.batch_events", pending.size());
    VSS_PROFILE_PLOT("gui.bridge.backlog", more ? state_->events.size() : 0);

    event_batch batch;
    {
        VSS_PROFILE_ZONE("gui.bridge.convert");
        // One block for the bytes of the whole batch (see `event_payload`).
        qsizetype total = 0;
        for (const auto& event : pending)
        {
            if (event.data)
                total += static_cast<qsizetype>(event.data->size());
        }
        QByteArray block(total, Qt::Uninitialized);
        // Taken once, before any payload shares the block: `data()` on a
        // shared array would copy it.
        char* const bytes = block.data();
        qsizetype offset = 0;
        batch.reserve(static_cast<qsizetype>(pending.size()));
        for (const auto& event : pending)
        {
            batch.push_back(
                convert(event, names_->text(event.endpoint, event.name)));
            if (!event.data || event.data->empty())
                continue;
            const auto size = static_cast<qsizetype>(event.data->size());
            std::memcpy(bytes + offset, event.data->data(), event.data->size());
            batch.back().data = event_payload(block, offset, size);
            offset += size;
        }
    }
    {
        // The engine's copies of the events go here, on the GUI thread.
        VSS_PROFILE_ZONE("gui.bridge.release");
        pending.clear();
    }

    {
        VSS_PROFILE_ZONE("gui.bridge.emit");
        Q_EMIT batch_ready(batch);
    }
    return more;
}

} // namespace vss::gui
