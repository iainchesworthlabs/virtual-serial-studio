//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/licence_watch.hpp>

#include <memory>
#include <utility>

namespace vss::control
{

licence_watch::licence_watch(reader read, std::chrono::milliseconds every)
    : read_(std::move(read)), every_(every), latest_(license::no_licence())
{
    read_once();
    thread_ = std::thread([this] { run(); });
}

licence_watch::~licence_watch()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

license::status licence_watch::latest() const
{
    const std::lock_guard lock(mutex_);
    return latest_;
}

std::uint64_t licence_watch::failures() const
{
    const std::lock_guard lock(mutex_);
    return failures_;
}

void licence_watch::run()
{
    std::unique_lock lock(mutex_);
    while (!stopping_)
    {
        if (wake_.wait_for(lock, every_, [this] { return stopping_; }))
            return;
        // The files are read with the lock released, so `latest` never
        // waits for a disk.
        lock.unlock();
        read_once();
        lock.lock();
    }
}

void licence_watch::read_once()
{
    if (!read_)
        return;
    try
    {
        license::status found = read_();
        const std::lock_guard lock(mutex_);
        latest_ = std::move(found);
    }
    catch (...)
    {
        // The last answer stands.
        const std::lock_guard lock(mutex_);
        ++failures_;
    }
}

std::unique_ptr<licence_watch>
watch_licence_folders(license::store_options options,
                      std::chrono::milliseconds every)
{
    options.clock_file.clear();
    auto store = std::make_shared<license::store>(std::move(options));
    return std::make_unique<licence_watch>(
        [store]
        {
            store->refresh();
            return store->view().status;
        },
        every);
}

} // namespace vss::control
