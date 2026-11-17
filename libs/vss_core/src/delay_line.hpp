//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "forwarding.hpp"

#include <vss/core/bytes.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/stats.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>

namespace vss {

/// Holds the bytes of one route (one source to one target on one link) for
/// the link's delay before writing them to the target.
///
/// The queue is first in, first out and the due times never go backwards, so
/// jitter cannot reorder the bytes.  One timer serves the head of the queue,
/// on a strand of its own, so two chunks never race each other to the target.
/// The hub owns the line; dropping the last reference (the route went away)
/// discards whatever is still waiting.
class delay_line : public std::enable_shared_from_this<delay_line>
{
public:
    using clock_function = std::function<time_point()>;

    /// `clock` is the hub's (`hub_options::clock`); empty is the steady
    /// clock.
    delay_line(const boost::asio::any_io_executor& executor,
               std::shared_ptr<endpoint> target, std::uint32_t delay_ms,
               std::uint32_t jitter_ms, clock_function clock = {},
               forward_function forward = {});

    delay_line(const delay_line&) = delete;
    delay_line& operator=(const delay_line&) = delete;

    /// Copies `item` and writes it to the target once its delay has passed.
    /// A break is held and delivered in its place among the bytes, as the
    /// bytes are.  Safe to call from any thread; calls from one thread keep
    /// their order.  When `visited` is set the line hands the item to its
    /// forward function after the write, with that set.
    void push(const stream_item& item, visited_set visited = {});

    /// Discards what is waiting and refuses what comes later.  When it
    /// returns, the line has written its last byte to the target: a write
    /// that was under way has finished, and none will start.  The hub calls
    /// it for a route it removes while the link stays, so a route made
    /// again for the same target cannot be overtaken by the old one.  Safe
    /// from any thread except inside the target's own write.
    void close();

private:
    struct entry
    {
        time_point due;
        owned_item data;
        visited_set visited;
    };

    void arm();
    void fire();
    time_point now() const;

    const boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    const std::shared_ptr<endpoint> target_;
    const std::uint32_t delay_ms_;
    const std::uint32_t jitter_ms_;
    const clock_function clock_;
    const forward_function forward_;

    /// Held while chunks that are due are written to the target, and by
    /// #close, so that a close waits for a write under way.  Always taken
    /// before `mutex_`.
    std::mutex deliver_mutex_;
    std::mutex mutex_;
    bool closed_ = false;
    std::deque<entry> queue_;
    time_point last_due_{};
    std::mt19937 random_;
};

} // namespace vss
