//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "profiling/stall_watchdog.hpp"

#include <vss/core/profile.hpp>

#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QMetaEnum>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <fstream>

namespace vss::gui
{
namespace
{

std::atomic<const char*> g_class{nullptr};
std::atomic<int> g_type{-1};

std::int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string type_name(int type)
{
    const QMetaEnum meta = QMetaEnum::fromType<QEvent::Type>();
    const char* key = meta.valueToKey(static_cast<quint64>(type));
    return key != nullptr ? std::string(key)
                          : "Event#" + std::to_string(type);
}

} // namespace

void publish_current_event(const char* receiver_class, int event_type) noexcept
{
    g_class.store(receiver_class, std::memory_order_relaxed);
    g_type.store(event_type, std::memory_order_relaxed);
}

current_event_snapshot current_event() noexcept
{
    return {g_class.load(std::memory_order_relaxed),
            g_type.load(std::memory_order_relaxed)};
}

QString StallWatchdog::default_log_path()
{
    const QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(folder).filePath(QStringLiteral("stalls.log"));
}

StallWatchdog::StallWatchdog(int threshold_ms, sink report, QObject* parent)
    : QObject(parent), threshold_ms_(std::max(threshold_ms, 50)),
      report_(std::move(report))
{
    if (!report_)
    {
        const QString path = default_log_path();
        QDir().mkpath(QFileInfo(path).absolutePath());
        // The log is for the last few stalls, not a history: start afresh when
        // it has grown past a quarter of a megabyte.
        if (QFileInfo(path).size() > qint64{256} * 1024)
            QFile::remove(path);
        log_path_ = path.toStdString();
    }

    beat_ns_.store(now_ns());
    heartbeat_ = new QTimer(this);
    heartbeat_->setTimerType(Qt::PreciseTimer);
    heartbeat_->setInterval(kHeartbeatMs);
    connect(heartbeat_, &QTimer::timeout, this,
            [this] { beat_ns_.store(now_ns(), std::memory_order_relaxed); });
    heartbeat_->start();

    thread_ = std::thread([this] { watch(); });
}

StallWatchdog::~StallWatchdog()
{
    {
        const std::lock_guard lock(wake_mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void StallWatchdog::say(const std::string& line)
{
    VSS_PROFILE_MESSAGE_TEXT(line);
    if (report_)
    {
        report_(line);
        return;
    }
    std::ofstream out(log_path_, std::ios::app);
    if (out)
        out << line << '\n';
}

void StallWatchdog::watch()
{
    const auto poll = std::chrono::milliseconds(
        std::clamp(threshold_ms_ / 4, 10, 250));
    bool stalled = false;
    std::int64_t stall_began_ns = 0;
    std::unique_lock lock(wake_mutex_);
    while (!stop_)
    {
        wake_.wait_for(lock, poll);
        if (stop_)
            break;
        const std::int64_t now = now_ns();
        const std::int64_t beat = beat_ns_.load(std::memory_order_relaxed);
        const std::int64_t late_ms = (now - beat) / 1'000'000;
        if (!stalled && late_ms >= threshold_ms_)
        {
            stalled = true;
            stall_began_ns = beat;
            reported_.fetch_add(1);
            const current_event_snapshot event = current_event();
            const std::string stamp =
                QDateTime::currentDateTime()
                    .toString(Qt::ISODateWithMs)
                    .toStdString();
            lock.unlock();
            say(stamp + " gui.stall: the GUI thread has not run its event loop "
                        "for " +
                std::to_string(late_ms) + " ms; handling " +
                (event.receiver_class != nullptr
                     ? std::string(event.receiver_class) + " / " +
                           type_name(event.event_type)
                     : std::string("no event")));
            lock.lock();
        }
        else if (stalled && late_ms < threshold_ms_ / 2)
        {
            stalled = false;
            const std::int64_t total_ms = (now - stall_began_ns) / 1'000'000;
            const std::string stamp =
                QDateTime::currentDateTime()
                    .toString(Qt::ISODateWithMs)
                    .toStdString();
            lock.unlock();
            say(stamp + " gui.stall: the GUI thread is running again after " +
                std::to_string(total_ms) + " ms");
            lock.lock();
        }
    }
}

} // namespace vss::gui
