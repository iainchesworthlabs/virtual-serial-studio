//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class QTimer;

namespace vss::gui
{

/// What the GUI thread is dispatching right now: the receiver's class name
/// (static storage, so another thread may read it) and the event type. The
/// application publishes it for every event, so a watchdog that finds the
/// thread stuck can say what it was doing.
struct current_event_snapshot
{
    const char* receiver_class = nullptr;
    int event_type = -1;
};

void publish_current_event(const char* receiver_class, int event_type) noexcept;
current_event_snapshot current_event() noexcept;

/// Notices when the GUI thread stops running its event loop, and says so in a
/// log file on this computer.
///
/// A timer on the GUI thread stamps a heartbeat every `heartbeat_ms`. A
/// background thread looks at the stamp; when it is older than `threshold_ms`
/// the window is not responding, and one line names the event the thread is
/// inside (`QTreeView / Resize`) and how long it has been stuck. A second line
/// follows when the loop comes back. Nothing leaves the computer: the default
/// destination is `stalls.log` in the application's local data folder, and the
/// same lines go to the Tracy profiler as messages while it records.
///
/// The watchdog never calls into the GUI thread and never waits for it.
class StallWatchdog final : public QObject
{
    Q_OBJECT

public:
    using sink = std::function<void(const std::string& line)>;

    /// `threshold_ms` is how long a stall lasts before it is reported. An
    /// empty `report` writes to `default_log_path()`.
    explicit StallWatchdog(int threshold_ms = 2000, sink report = {},
                           QObject* parent = nullptr);
    ~StallWatchdog() override;

    /// How many stalls have been reported so far.
    int reported() const noexcept { return reported_.load(); }

    /// The log file, `<local data>/stalls.log`.
    static QString default_log_path();

    static constexpr int kHeartbeatMs = 100;

private:
    void watch();
    void say(const std::string& line);

    int threshold_ms_;
    sink report_;
    std::string log_path_;
    QTimer* heartbeat_ = nullptr;
    std::atomic<std::int64_t> beat_ns_{0};
    std::atomic<int> reported_{0};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace vss::gui
