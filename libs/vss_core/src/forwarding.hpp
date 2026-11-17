//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/stats.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
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

/// Called by a delay line or a bus wire after it has written a chunk to
/// `at`, so the hub can carry the chunk on along the links of `at`.
using forward_function = std::function<void(
    endpoint_id at, bytes_view data, const visited_set& visited,
    time_point when)>;

} // namespace vss
