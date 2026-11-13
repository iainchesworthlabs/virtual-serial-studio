//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/background_work.hpp"

#include <QMetaObject>

#include <utility>

namespace vss::gui
{

BackgroundWork::BackgroundWork(QObject* parent) : QObject(parent) {}

BackgroundWork::~BackgroundWork()
{
    if (worker_.joinable())
        worker_.join();
}

bool BackgroundWork::start(std::function<void()> work,
                           std::function<void()> done)
{
    if (busy_)
        return false;
    if (worker_.joinable())
        worker_.join();
    busy_ = true;
    done_ = std::move(done);
    worker_ = std::thread(
        [this, work = std::move(work)]
        {
            work();
            QMetaObject::invokeMethod(this, [this] { finish(); },
                                      Qt::QueuedConnection);
        });
    return true;
}

void BackgroundWork::finish()
{
    if (worker_.joinable())
        worker_.join();
    auto done = std::move(done_);
    done_ = nullptr;
    busy_ = false;
    if (done)
        done();
}

} // namespace vss::gui
