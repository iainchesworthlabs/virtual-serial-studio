//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/license/license.hpp>
#include <vss/license/store.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace vss::control
{

/// Keeps what is known of the licence on this computer for a gateway to
/// report in `status` (docs/remote-control.md, section 6).
///
/// Reading the licence means reading files, which a `status` reply must not
/// wait for.  So one thread of this object reads, once as the object is made
/// and then once each `every`, and `latest` hands out what the last reading
/// found without reading anything.  The reader is only ever called from one
/// thread at a time, so it may use an object that is not safe to share, such
/// as a `license::store`.
///
/// A reader that throws leaves the last answer in place and is counted;
/// when the first reading throws the answer is `license::no_licence()`.
class licence_watch
{
public:
    using reader = std::function<license::status()>;

    /// Reads once, on the caller's thread, and starts the thread that reads
    /// again each `every`.
    licence_watch(reader read, std::chrono::milliseconds every);

    /// Stops the thread.  It waits for a reading that is under way and for
    /// nothing else.
    ~licence_watch();

    licence_watch(const licence_watch&) = delete;
    licence_watch& operator=(const licence_watch&) = delete;

    /// What the last reading found.  Safe from any thread; reads no file.
    license::status latest() const;

    /// Readings that threw, for the tests.
    std::uint64_t failures() const;

private:
    void run();
    void read_once();

    reader read_;
    std::chrono::milliseconds every_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    license::status latest_;
    std::uint64_t failures_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

/// A watch over the licence folders that `options` names, as `vss_gate`
/// keeps one over `license::default_store_options()`.  The licence in use
/// is chosen as Studio chooses it.  The clock file of `options` is not
/// used: a gateway reads the licence folders and writes nothing there, so
/// the latest time it has seen is kept in memory only.
std::unique_ptr<licence_watch>
watch_licence_folders(license::store_options options,
                      std::chrono::milliseconds every);

} // namespace vss::control
