//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QObject>

#include <functional>
#include <thread>

namespace vss::gui
{

/// Runs one piece of slow engine work (making a port installs a device in
/// Windows and takes seconds) off the GUI thread.
///
/// `start()` runs `work` on a worker thread and then `done` on the thread that
/// owns this object, through a queued call.  `work` must touch no widget and
/// no state the GUI thread uses meanwhile; hand its result back in a
/// `shared_ptr` both lambdas hold.  Only one job runs at a time, so a second
/// `start()` (a double click) is refused.  The destructor waits for a running
/// job, so owners keep their window open while `busy()`.
class BackgroundWork final : public QObject
{
    Q_OBJECT

public:
    explicit BackgroundWork(QObject* parent = nullptr);
    ~BackgroundWork() override;

    BackgroundWork(const BackgroundWork&) = delete;
    BackgroundWork& operator=(const BackgroundWork&) = delete;

    /// True from `start()` until `done` has returned.
    bool busy() const noexcept { return busy_; }

    /// False, and nothing runs, while a job is running.
    bool start(std::function<void()> work, std::function<void()> done);

private:
    void finish();

    std::thread worker_;
    std::function<void()> done_;
    bool busy_ = false;
};

} // namespace vss::gui
