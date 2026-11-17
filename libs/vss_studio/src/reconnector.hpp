//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/core/types.hpp>
#include <vss/studio/reconnect.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string>
#include <thread>

namespace vss {

/// Runs the tries of a #reconnect_policy on a thread of its own, because
/// opening a network transport can block for a while and the hub's threads
/// must not wait on it.  It knows nothing of ports: the studio gives it what
/// to call.
class reconnector
{
public:
    /// What one try returns tells the reconnector what to do next: success
    /// ends the sequence; `errc::not_found` and `errc::unsupported` drop the
    /// endpoint (it is gone, or not one that can come back); any other error
    /// is a failed try and the next one is scheduled.
    using attempt_fn = std::function<result<void>(endpoint_id)>;
    /// Tells the user what is happening, such as "trying again in 2000 ms".
    using note_fn = std::function<void(endpoint_id, std::string)>;

    reconnector(attempt_fn attempt, note_fn note);
    ~reconnector();

    reconnector(const reconnector&) = delete;
    reconnector& operator=(const reconnector&) = delete;

    void set_policy(const reconnect_policy& policy);
    reconnect_policy policy() const;

    /// Marks an endpoint as one the policy applies to (a client that connects
    /// out), or not.  Only allowed endpoints are retried.
    void allow(endpoint_id id);
    void disallow(endpoint_id id);

    /// An endpoint lost its connection: start a sequence for it.  Does
    /// nothing while the policy is off, the endpoint is not allowed, or a
    /// sequence is already running.
    void lost(endpoint_id id);

    /// Cancels the sequence for an endpoint.
    void forget(endpoint_id id);

    /// Endpoints waiting for their next try.
    std::size_t pending() const;

    /// Where the tries of one endpoint stand, for a window to show.
    struct progress
    {
        /// A sequence is running: the endpoint waits for its next try.
        bool retrying = false;
        /// Tries that have failed in the sequence so far.
        unsigned attempts = 0;
        /// Why the last try failed, or empty before one has.
        std::string last_error;
        /// The same error as category and number, such as "system:10061".
        std::string last_error_code;
        /// How long until the next try, or zero when none is waiting.
        std::chrono::milliseconds next_in{0};
    };
    progress status(endpoint_id id) const;

private:
    struct entry
    {
        std::chrono::steady_clock::time_point due;
        unsigned attempts = 0;
    };

    void run();
    void announce(endpoint_id id, const std::string& text) const;

    attempt_fn attempt_;
    note_fn note_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    reconnect_policy policy_;
    std::map<endpoint_id, entry> waiting_;
    /// The message of the last failed try of each endpoint, kept after the
    /// sequence gives up and dropped when a try succeeds.
    std::map<endpoint_id, std::string> last_errors_;
    std::map<endpoint_id, std::string> last_codes_;
    std::set<endpoint_id> allowed_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace vss
