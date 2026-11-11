//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "delay_line.hpp"

#include <vss/core/profile.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace vss {

delay_line::delay_line(const boost::asio::any_io_executor& executor,
                       std::shared_ptr<endpoint> target,
                       std::uint32_t delay_ms, std::uint32_t jitter_ms)
    : strand_(boost::asio::make_strand(executor))
    , timer_(strand_)
    , target_(std::move(target))
    , delay_ms_(delay_ms)
    , jitter_ms_(std::min(jitter_ms, delay_ms))
    , random_(std::random_device{}())
{
}

void delay_line::push(bytes_view data)
{
    if (data.empty())
        return;

    VSS_PROFILE_ZONE("link.delay_push");

    bool was_empty = false;
    {
        std::lock_guard lock(mutex_);
        // A chunk the hub was routing when the route was removed.
        if (closed_)
            return;
        std::int64_t latency = delay_ms_;
        if (jitter_ms_ != 0)
        {
            std::uniform_int_distribution<std::int64_t> spread(
                -static_cast<std::int64_t>(jitter_ms_),
                static_cast<std::int64_t>(jitter_ms_));
            latency += spread(random_);
        }

        // A chunk is never due before the one ahead of it, so a short draw
        // after a long one waits for it instead of overtaking it.
        const time_point due = std::max(
            clock_type::now() + std::chrono::milliseconds(latency), last_due_);
        last_due_ = due;
        was_empty = queue_.empty();
        queue_.push_back(item{due, byte_buffer(data.begin(), data.end())});
        VSS_PROFILE_PLOT("link.delay_queue", queue_.size());
    }

    if (!was_empty)
        return;

    // The line holds the timer's strand, which holds the line: the handlers
    // keep only a weak reference, so a route that goes away takes its queue
    // with it.
    boost::asio::post(strand_, [weak = weak_from_this()] {
        if (const auto self = weak.lock())
            self->arm();
    });
}

void delay_line::arm()
{
    time_point due;
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty())
            return;
        due = queue_.front().due;
    }

    timer_.expires_at(due);
    timer_.async_wait([weak = weak_from_this()](boost::system::error_code ec) {
        if (ec)
            return;
        if (const auto self = weak.lock())
            self->fire();
    });
}

void delay_line::fire()
{
    VSS_PROFILE_ZONE("link.delay_fire");
    // Only this strand and close() take it, so it is free unless the route
    // is being removed.
    std::lock_guard delivering(deliver_mutex_);
    std::vector<byte_buffer> ready;
    {
        std::lock_guard lock(mutex_);
        const time_point now = clock_type::now();
        while (!queue_.empty() && queue_.front().due <= now)
        {
            ready.push_back(std::move(queue_.front().data));
            queue_.pop_front();
        }
    }

    for (auto& chunk : ready)
        target_->write(std::move(chunk));

    arm();
}

void delay_line::close()
{
    std::lock_guard delivering(deliver_mutex_);
    std::lock_guard lock(mutex_);
    closed_ = true;
    queue_.clear();
}

} // namespace vss
