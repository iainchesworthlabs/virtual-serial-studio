//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/line_break.hpp>
#include <vss/core/stats.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

namespace vss
{

/// The endpoints one chunk has been read from or written to, shared by every
/// branch that carries it on.  An endpoint is claimed when a route delivers
/// the chunk to it, so a chunk reaches an endpoint once however many ports
/// pass it on, and a two-way link or a bus cannot send it back.
class chunk_trace
{
public:
    /// True when `id` has had the chunk.
    bool seen(endpoint_id id) const
    {
        std::lock_guard lock(mutex_);
        return contains(id);
    }

    /// Takes `id` for the chunk; false when it had it already.
    bool claim(endpoint_id id)
    {
        std::lock_guard lock(mutex_);
        if (contains(id))
            return false;
        endpoints_.push_back(id);
        return true;
    }

    /// Notes endpoints that already have the chunk.
    void add(endpoint_id id)
    {
        std::lock_guard lock(mutex_);
        if (!contains(id))
            endpoints_.push_back(id);
    }

private:
    bool contains(endpoint_id id) const
    {
        return std::find(endpoints_.begin(), endpoints_.end(), id) !=
               endpoints_.end();
    }

    mutable std::mutex mutex_;
    std::vector<endpoint_id> endpoints_;
};

/// What travels with a chunk that is carried on: the shared trace, and the
/// links it has crossed on the way to this endpoint.  A chunk does not leave
/// over a link it came by, so the listeners of a split do not hear each other.
/// Null for a chunk that has not been forwarded, and for a chunk that nothing
/// forwards.
struct visited_chunk
{
    std::shared_ptr<chunk_trace> trace;
    std::vector<link_id> via;
};

using visited_set = std::shared_ptr<const visited_chunk>;

/// A chunk or a break that a delay line or a bus wire holds: it owns the
/// bytes, which the item the router passes on only views.
using owned_item = std::variant<byte_buffer, line_break>;

/// Called by a delay line or a bus wire after it has written an item to
/// `at`, so the hub can carry it on along the links of `at`.
using forward_function = std::function<void(
    endpoint_id at, const stream_item& item, const visited_set& visited,
    time_point when)>;

/// The item a view of `owned` makes.
inline stream_item view_of(const owned_item& owned)
{
    if (const auto* bytes = std::get_if<byte_buffer>(&owned))
        return bytes_view(*bytes);
    return std::get<line_break>(owned);
}

} // namespace vss
