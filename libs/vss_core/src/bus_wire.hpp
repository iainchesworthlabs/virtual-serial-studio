//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "delay_line.hpp"

#include <vss/core/bus_line.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/stats.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vss
{

/// Holds the chunks of a half-duplex bus while they are on the wire, and
/// hands each to its receivers when its last byte has left it.
///
/// One wire serves the whole bus, with one timer on a strand of its own, so
/// every member hears the chunks in the order they left the wire.  The hub
/// owns the wire; dropping the last reference (the bus went away) discards
/// what is still on it.
class bus_wire : public std::enable_shared_from_this<bus_wire>
{
public:
    /// Where one copy of a chunk goes: straight to the member, or through
    /// the delay line of its route when the bus has a delay.
    struct delivery
    {
        std::shared_ptr<endpoint> target;
        std::shared_ptr<delay_line> delay;
    };

    using clock_function = std::function<time_point()>;

    /// `clock` is the hub's clock, empty for the steady clock.  `garbled`
    /// counts the bytes delivered inverted, once for each receiver.
    bus_wire(const boost::asio::any_io_executor& executor, clock_function clock,
             std::shared_ptr<std::atomic<std::uint64_t>> garbled);

    bus_wire(const bus_wire&) = delete;
    bus_wire& operator=(const bus_wire&) = delete;

    /// Copies `data` and delivers it at `due`.  Safe to call from any thread.
    void push(endpoint_id sender, time_point due, bytes_view data,
              std::shared_ptr<bus_transmission> transmission,
              std::vector<delivery> receivers);

    /// Drops what `member` still has on the wire and stops delivering to it.
    void forget(endpoint_id member);

    /// Discards what is on the wire and takes nothing more.  When it returns
    /// no delivery is under way and none will follow, so the hub can put
    /// another wire in this one's place without a chunk of this one arriving
    /// after a chunk of the next.
    void close();

private:
    struct item
    {
        endpoint_id sender = 0;
        byte_buffer data;
        std::shared_ptr<bus_transmission> transmission;
        std::vector<delivery> receivers;
    };

    time_point now() const;
    void arm();
    void fire();

    const boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    const clock_function clock_;
    const std::shared_ptr<std::atomic<std::uint64_t>> garbled_;

    /// Held while chunks are handed to their receivers, and by #close, so
    /// that a close waits for a delivery under way.  Always taken before
    /// `mutex_`.
    std::mutex deliver_mutex_;
    std::mutex mutex_;
    bool closed_ = false;
    /// By the time each leaves the wire; chunks due together keep the order
    /// they were pushed in.
    std::multimap<time_point, item> queue_;
};

} // namespace vss
