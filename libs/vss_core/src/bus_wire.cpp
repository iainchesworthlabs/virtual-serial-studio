//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "bus_wire.hpp"

#include <vss/core/profile.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace vss
{
namespace
{

/// How often a wire on a clock that is not the steady clock looks at it.  A
/// timer cannot be asked to wait for such a clock, which a test moves by
/// hand, so the wire looks again this often while something is on it.
constexpr std::chrono::milliseconds foreign_clock_poll{1};

} // namespace

bus_wire::bus_wire(const boost::asio::any_io_executor& executor,
                   clock_function clock,
                   std::shared_ptr<std::atomic<std::uint64_t>> garbled)
    : strand_(boost::asio::make_strand(executor)), timer_(strand_),
      clock_(std::move(clock)), garbled_(std::move(garbled))
{
}

time_point bus_wire::now() const
{
    return clock_ ? clock_() : clock_type::now();
}

void bus_wire::push(endpoint_id sender, time_point due, bytes_view data,
                    std::shared_ptr<bus_transmission> transmission,
                    std::vector<delivery> receivers)
{
    if (data.empty() || receivers.empty())
        return;

    VSS_PROFILE_ZONE("link.wire_push");

    bool first = false;
    {
        std::lock_guard lock(mutex_);
        const auto placed = queue_.emplace(
            due, item{sender, byte_buffer(data.begin(), data.end()),
                      std::move(transmission), std::move(receivers)});
        first = placed == queue_.begin();
        VSS_PROFILE_PLOT("link.wire_queue", queue_.size());
    }

    // Only a chunk that is now the next to leave changes what the timer
    // waits for.  As in delay_line, the handlers hold a weak reference, so a
    // bus that goes away takes its wire with it.
    if (!first)
        return;
    boost::asio::post(strand_,
                      [weak = weak_from_this()]
                      {
                          if (const auto self = weak.lock())
                              self->arm();
                      });
}

void bus_wire::forget(endpoint_id member)
{
    std::lock_guard lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();)
    {
        auto& receivers = it->second.receivers;
        std::erase_if(receivers, [member](const delivery& entry)
                      { return entry.target->id() == member; });
        if (it->second.sender == member || receivers.empty())
            it = queue_.erase(it);
        else
            ++it;
    }
}

void bus_wire::arm()
{
    time_point due;
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty())
            return;
        due = queue_.begin()->first;
    }

    if (clock_)
    {
        const bool ready = due <= clock_();
        timer_.expires_after(ready ? std::chrono::milliseconds(0)
                                   : foreign_clock_poll);
    }
    else
    {
        timer_.expires_at(due);
    }
    timer_.async_wait(
        [weak = weak_from_this()](boost::system::error_code ec)
        {
            if (ec)
                return;
            if (const auto self = weak.lock())
                self->fire();
        });
}

void bus_wire::fire()
{
    VSS_PROFILE_ZONE("link.wire_fire");
    std::vector<item> ready;
    {
        std::lock_guard lock(mutex_);
        const time_point current = now();
        while (!queue_.empty() && queue_.begin()->first <= current)
        {
            ready.push_back(std::move(queue_.begin()->second));
            queue_.erase(queue_.begin());
        }
    }

    for (auto& chunk : ready)
    {
        // Read here and not when the chunk was queued: a chunk that ran into
        // this one while it was on the wire set the flag since.
        if (chunk.transmission && chunk.transmission->garble.load())
        {
            for (auto& value : chunk.data)
                value = static_cast<byte_buffer::value_type>(~value);
            garbled_->fetch_add(chunk.data.size() * chunk.receivers.size());
        }
        for (const delivery& entry : chunk.receivers)
        {
            if (entry.delay)
                entry.delay->push(chunk.data);
            else
                entry.target->write(bytes_view(chunk.data));
        }
    }

    arm();
}

} // namespace vss
