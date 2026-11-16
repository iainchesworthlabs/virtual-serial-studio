//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The traffic tap: the bounded queue through which one plugin sees the
// traffic of the ports and links it was granted.  docs/plugins.md, sections
// 7.9, 9.1 (`traffic.tap`) and 11 (rules 2 and 3), are the design.
//
// A tap subscribes one sink to the hub.  The sink runs on the hub's event
// strand.  It matches the event against the watched places, takes the event's
// bytes (the event shares them; nothing is copied) and appends an item to the
// queue under a short lock.  It does no I/O, calls nothing of the hub and
// never waits.  When the queue is full the new item is dropped and counted.
//
// The host reads with `take`, which returns items in arrival order with what
// was dropped since the last `take`.  `set_notify` says, without polling,
// that the queue went from empty to not empty; the callback is posted to an
// executor the caller gives, never run on a hub strand.
//
// What is a place.  A place has a name (what the plugin sees), the ports
// whose events belong to it, and optionally a link.  The hub tags data
// events by port, not by link, so a link is watched through its member ports,
// which the caller lists; `link` is matched against the link of a bus fault.
// An event that belongs to several places is queued once for each.
//
// The tap must be destroyed, or closed, before the hub.  The state that the
// sink shares is owned by a `shared_ptr` the sink holds, so a delivery that
// is running or arrives after `close` finds `closed` set and does nothing.
//
// Nothing here starts a thread, a process or a plugin, and nothing here
// builds an API message.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/event.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vss::plugin::host
{

/// What an item of the tap is.
enum class tap_item_kind
{
    data,
    lines,
    error,
    bus_fault
};

/// A place the tap watches.
struct tap_place
{
    /// The name the plugin sees.
    std::string name;
    /// Ports whose events belong to this place.  For a link, its members.
    std::vector<endpoint_id> endpoints;
    /// A link whose bus faults belong to this place; 0 for none.
    link_id link = 0;
};

/// Limits of one tap.  The defaults follow section 7.9: 8 MiB of bytes, and
/// 4096 items, which is the burst of 4000 frames and a little more.  A limit
/// of 0 means that nothing can be held, so every item is dropped.
struct tap_limits
{
    std::size_t max_items = 4096;
    std::size_t max_bytes = std::size_t{8} * 1024 * 1024;
};

/// One thing that happened at a place, as the hub reported it.
struct tap_item
{
    std::string place;
    tap_item_kind kind = tap_item_kind::data;
    data_direction direction = data_direction::rx;
    /// The hub's time for the event.
    time_point when{};
    /// For `data`: the bytes.  Shared with the hub and never changed.
    std::shared_ptr<const byte_buffer> bytes;
    /// For `lines`: the lines after the change.
    control_lines lines;
    /// For `error`.
    boost::system::error_code error;
    /// For `bus_fault`.
    bus_fault fault = bus_fault::none;
    link_id link = 0;
    std::string message;
};

/// What was dropped at one place.
struct tap_drop
{
    std::string place;
    std::uint64_t items = 0;
    std::uint64_t bytes = 0;
};

/// What `take` returns.
struct tap_batch
{
    std::vector<tap_item> items;
    /// Dropped since the previous `take`, because the queue was full.
    std::uint64_t dropped_items = 0;
    std::uint64_t dropped_bytes = 0;
    /// The same drops, for each place that had any.
    std::vector<tap_drop> dropped;
};

class traffic_tap
{
public:
    /// Subscribes to `source`, which must outlive the tap or the tap must be
    /// closed first.
    traffic_tap(hub& source, std::vector<tap_place> places,
                tap_limits limits = {});
    ~traffic_tap();

    traffic_tap(const traffic_tap&) = delete;
    traffic_tap& operator=(const traffic_tap&) = delete;

    /// Replaces the watched places.  Atomic with respect to deliveries: each
    /// event is matched against the old set or the new one, so none is lost
    /// or queued twice.  Items already queued stay.
    void set_places(std::vector<tap_place> places);

    /// Returns at most `max_items` items and at most `max_bytes` bytes of
    /// data, oldest first, and the drops since the last call.  An item whose
    /// bytes alone are over `max_bytes` is still returned when it is first.
    /// Returns nothing after `close`.
    tap_batch take(std::size_t max_items, std::size_t max_bytes);

    /// Calls `notify` on `executor` once each time the queue goes from empty
    /// to not empty, and once at once if it already holds items.  A second
    /// call replaces the first.  `notify` may run after `close` has returned
    /// only if it was posted before; it is never posted afterwards.
    void set_notify(std::function<void()> notify,
                    boost::asio::any_io_executor executor);

    /// Items and bytes held now.
    std::size_t queued_items() const;
    std::size_t queued_bytes() const;

    /// Unsubscribes, makes late deliveries do nothing and releases what is
    /// held.  Safe to call more than once and from any thread.
    void close();

    struct state;

private:
    hub* hub_;
    subscriber_id subscription_ = 0;
    std::shared_ptr<state> state_;
};

} // namespace vss::plugin::host
