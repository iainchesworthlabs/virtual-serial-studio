//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_EVENT_BRIDGE_HPP
#define VSS_GUI_BACKEND_EVENT_BRIDGE_HPP

#include "backend/view_types.hpp"

#include <vss/core/types.hpp>

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <memory>

class QTimer;

namespace vss
{
class hub;
}

namespace vss::gui
{

/// Marshals hub events to the GUI thread and coalesces them by interval.
///
/// The hub delivers on its event strand.  The sink therefore does only one
/// thing: it captures a `shared_ptr` to the queue state and appends under a
/// mutex.  It never touches a `QObject`, so nothing in this class can be
/// used-after-free by an in-flight delivery after `unsubscribe`.  A `QTimer`
/// on the GUI thread drains the queue and emits one batch.
///
/// The bridge must be destroyed before the hub it refers to.
class EventBridge final : public QObject
{
    Q_OBJECT

public:
    struct options
    {
        /// Coalescing interval; 250 ms by default.
        int interval_ms = 250;
        /// Events kept between drains.  Oldest are dropped first.
        std::size_t max_events = 8192;
        /// Events delivered in one batch from the timer.  What is left is
        /// delivered in further batches, each from its own turn of the event
        /// loop, so a flood of events cannot hold the GUI thread for the time
        /// it takes to handle all of them: input and painting run between
        /// the slices.  `flush` ignores it.
        std::size_t max_batch = 256;
    };

    explicit EventBridge(vss::hub& hub, QObject* parent = nullptr);
    EventBridge(vss::hub& hub, options options, QObject* parent = nullptr);
    ~EventBridge() override;

    void start();
    void stop();
    bool is_running() const noexcept { return running_; }

    void set_interval(int milliseconds);
    int interval() const noexcept;

    /// Drains now instead of waiting for the timer.  Must run on the owning
    /// thread; used by "Refresh now" and by tests.
    void flush();

    std::uint64_t delivered() const noexcept;
    std::uint64_t dropped() const noexcept;
    std::size_t pending() const;

Q_SIGNALS:
    void batch_ready(const vss::gui::event_batch& events);

private:
    struct state;
    struct name_cache;

    void drain();
    /// Delivers up to `limit` of the oldest events as one batch.  True when
    /// events are left.
    bool deliver(std::size_t limit);
    void continue_draining();

    vss::hub& hub_;
    std::shared_ptr<state> state_;
    std::unique_ptr<name_cache> names_;
    QTimer* timer_ = nullptr;
    vss::subscriber_id subscription_ = 0;
    bool running_ = false;
    bool continuing_ = false;
    int last_slice_ms_ = 0;
    std::size_t max_batch_ = 256;
};

} // namespace vss::gui

#endif // VSS_GUI_BACKEND_EVENT_BRIDGE_HPP
