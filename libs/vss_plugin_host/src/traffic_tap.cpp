//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/traffic_tap.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

namespace vss::plugin::host
{

namespace
{

bool is_tapped(event_kind kind) noexcept
{
    return kind == event_kind::data || kind == event_kind::lines ||
           kind == event_kind::error || kind == event_kind::bus_fault;
}

bool belongs(const tap_place& place, const endpoint_event& event) noexcept
{
    if (event.kind == event_kind::bus_fault && place.link != 0 &&
        place.link == event.link)
    {
        return true;
    }
    return std::ranges::find(place.endpoints, event.endpoint) !=
           place.endpoints.end();
}

std::size_t cost_of(const tap_item& item) noexcept
{
    return item.bytes ? item.bytes->size() : 0;
}

} // namespace

// Everything the sink touches.  The sink holds a shared_ptr to it, so a
// delivery that runs after the tap is gone finds `closed` set.
struct traffic_tap::state
{
    mutable std::mutex mutex;
    bool closed = false;
    bool just_became_non_empty = false;
    std::vector<tap_place> places;
    tap_limits limits;
    std::deque<tap_item> queue;
    std::size_t queued_bytes = 0;
    std::uint64_t dropped_items = 0;
    std::uint64_t dropped_bytes = 0;
    std::vector<tap_drop> dropped;
    std::shared_ptr<std::function<void()>> notify;
    boost::asio::any_io_executor executor;

    // Called by the hub's event strand.  Returns the notification to post, if
    // any; the caller posts it after the lock is released.
    void deliver(const std::shared_ptr<state>& self,
                 const endpoint_event& event)
    {
        if (!is_tapped(event.kind))
            return;
        std::shared_ptr<std::function<void()>> to_post;
        boost::asio::any_io_executor where;
        {
            const std::lock_guard lock(mutex);
            if (closed)
                return;
            just_became_non_empty = false;
            for (const auto& place : places)
            {
                if (!belongs(place, event))
                    continue;
                add(place, event);
            }
            if (just_became_non_empty && notify)
            {
                to_post = notify;
                where = executor;
            }
        }
        if (to_post)
            post_notify(self, std::move(to_post), where);
    }

    static void post_notify(const std::shared_ptr<state>& self,
                            std::shared_ptr<std::function<void()>> call,
                            const boost::asio::any_io_executor& where)
    {
        boost::asio::post(where,
                          [self, call = std::move(call)]
                          {
                              {
                                  const std::lock_guard lock(self->mutex);
                                  if (self->closed)
                                      return;
                              }
                              (*call)();
                          });
    }

    // Caller holds the lock.
    void add(const tap_place& place, const endpoint_event& event)
    {
        tap_item item;
        item.kind = event.kind == event_kind::data    ? tap_item_kind::data
                    : event.kind == event_kind::lines ? tap_item_kind::lines
                    : event.kind == event_kind::error
                        ? tap_item_kind::error
                        : tap_item_kind::bus_fault;
        item.direction = event.direction;
        item.when = event.when;
        if (item.kind == tap_item_kind::data)
            item.bytes = event.data;
        item.lines = event.lines;
        item.error = event.error;
        item.fault = event.fault;
        item.link = event.link;
        item.message = event.message;
        const std::size_t cost = cost_of(item);
        if (item.kind == tap_item_kind::data && !item.bytes)
            return;
        if (queue.size() >= limits.max_items ||
            cost > limits.max_bytes - std::min(limits.max_bytes, queued_bytes))
        {
            ++dropped_items;
            dropped_bytes += cost;
            auto found =
                std::ranges::find(dropped, place.name, &tap_drop::place);
            if (found == dropped.end())
            {
                dropped.push_back(tap_drop{place.name, 0, 0});
                found = dropped.end() - 1;
            }
            ++found->items;
            found->bytes += cost;
            if (limits.mark_losses && item.kind == tap_item_kind::data)
                mark_loss(place, item, cost);
            return;
        }
        item.place = place.name;
        if (queue.empty())
            just_became_non_empty = true;
        queued_bytes += cost;
        queue.push_back(std::move(item));
    }

    // Caller holds the lock.  Notes a dropped chunk where the queue stands:
    // in the loss item that ends the queue, or in a new one.
    void mark_loss(const tap_place& place, const tap_item& dropped,
                   std::size_t cost)
    {
        if (queue.empty() || queue.back().kind != tap_item_kind::loss)
        {
            tap_item marker;
            marker.kind = tap_item_kind::loss;
            marker.place = place.name;
            marker.when = dropped.when;
            if (queue.empty())
                just_became_non_empty = true;
            queue.push_back(std::move(marker));
        }
        tap_item& marker = queue.back();
        marker.when = dropped.when;
        const auto found =
            std::ranges::find_if(marker.losses,
                                 [&](const tap_loss& loss)
                                 {
                                     return loss.place == place.name &&
                                            loss.direction == dropped.direction;
                                 });
        if (found == marker.losses.end())
        {
            tap_loss loss;
            loss.place = place.name;
            loss.direction = dropped.direction;
            loss.first = dropped.when;
            loss.last = dropped.when;
            loss.items = 1;
            loss.bytes = cost;
            marker.losses.push_back(std::move(loss));
            return;
        }
        ++found->items;
        found->bytes += cost;
        found->last = dropped.when;
    }
};

traffic_tap::traffic_tap(hub& source, std::vector<tap_place> places,
                         tap_limits limits)
    : hub_(&source), state_(std::make_shared<state>())
{
    state_->places = std::move(places);
    state_->limits = limits;
    subscription_ = source.subscribe(
        event_filter{}, [shared = state_](const endpoint_event& event)
        { shared->deliver(shared, event); });
}

traffic_tap::~traffic_tap()
{
    close();
}

void traffic_tap::set_places(std::vector<tap_place> places)
{
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
        return;
    state_->places = std::move(places);
}

tap_batch traffic_tap::take(std::size_t max_items, std::size_t max_bytes)
{
    tap_batch batch;
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
        return batch;
    auto& queue = state_->queue;
    std::size_t bytes = 0;
    while (!queue.empty() && batch.items.size() < max_items)
    {
        const std::size_t cost = cost_of(queue.front());
        if (!batch.items.empty() &&
            cost > max_bytes - std::min(max_bytes, bytes))
            break;
        bytes += cost;
        state_->queued_bytes -= cost;
        batch.items.push_back(std::move(queue.front()));
        queue.pop_front();
    }
    batch.dropped_items = std::exchange(state_->dropped_items, 0);
    batch.dropped_bytes = std::exchange(state_->dropped_bytes, 0);
    batch.dropped = std::exchange(state_->dropped, {});
    return batch;
}

void traffic_tap::set_notify(std::function<void()> notify,
                             boost::asio::any_io_executor executor)
{
    auto call = std::make_shared<std::function<void()>>(std::move(notify));
    boost::asio::any_io_executor where = executor;
    bool now = false;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->closed)
            return;
        state_->notify = call;
        state_->executor = std::move(executor);
        now = !state_->queue.empty();
    }
    if (now)
        state::post_notify(state_, std::move(call), where);
}

std::size_t traffic_tap::queued_items() const
{
    const std::lock_guard lock(state_->mutex);
    return state_->queue.size();
}

std::size_t traffic_tap::queued_bytes() const
{
    const std::lock_guard lock(state_->mutex);
    return state_->queued_bytes;
}

void traffic_tap::close()
{
    std::deque<tap_item> released;
    std::shared_ptr<std::function<void()>> notify;
    boost::asio::any_io_executor executor;
    std::vector<tap_place> places;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->closed)
            return;
        state_->closed = true;
        released.swap(state_->queue);
        state_->queued_bytes = 0;
        notify = std::move(state_->notify);
        executor = std::move(state_->executor);
        places.swap(state_->places);
    }
    hub_->unsubscribe(subscription_);
}

} // namespace vss::plugin::host
