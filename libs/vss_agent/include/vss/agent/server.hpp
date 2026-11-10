//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/studio/studio.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vss::agent
{

/// The version of the program, as `hello` reports it.
std::string agent_version();

struct server_options
{
    /// The instance name; empty for the user's ordinary agent.
    std::string instance;
    /// Connections served at once.  One more is told `busy` and closed.
    std::size_t max_clients = 16;
    /// A connection that has not said `hello` by then is closed.
    std::chrono::milliseconds handshake_timeout{5000};
    /// Requests of one client that may wait for their turn.  The agent stops
    /// reading from a client that has this many, so a client that floods
    /// slows down only itself.
    std::size_t max_pending_requests = 32;
    /// Replies and batches that may wait for one client to read them, in
    /// bytes.  A client that lets more than this pile up is closed.
    std::size_t max_unsent_bytes = std::size_t{4} * 1024 * 1024;
    /// Events kept for one subscriber between two batches, and the bytes of
    /// traffic among them.  Older ones are dropped and counted.
    std::size_t max_queued_events = 2048;
    std::size_t max_queued_event_bytes = std::size_t{512} * 1024;
};

/// What the server has seen, for the tests and the log.
struct server_counters
{
    std::uint64_t accepted = 0;
    std::uint64_t refused_busy = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t handshake_timeouts = 0;
    std::uint64_t slow_clients_closed = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t clients = 0;

    friend bool operator==(const server_counters&,
                           const server_counters&) = default;
};

/// Serves one studio to this user's clients over the local transport.
///
/// Threads: the studio keeps its own (each endpoint on its strand, events on
/// the hub's dispatcher strand).  The server adds one thread that does all
/// client I/O and a small pool that runs requests, so a slow client cannot
/// hold up the engine and a slow request cannot hold up another client.  The
/// only thing that runs on the dispatcher strand is a sink that copies an
/// event into a bounded queue.
///
/// The studio must outlive the server.
class server
{
public:
    explicit server(studio& app, server_options options = {});
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    /// Starts listening.  See `listen_local` for the errors.
    result<void> start();

    /// Closes every connection and stops the threads.  Requests that are
    /// running finish first.  Safe to call twice.
    void stop();

    /// The pipe name or socket path, once started.
    std::string address() const;

    /// Asks whoever waits in `wait_for_stop_request` to stop the agent.  Safe
    /// from any thread.
    void request_stop();

    /// Blocks until a client sent `shutdown` or `request_stop` was called.
    void wait_for_stop_request();

    /// As `wait_for_stop_request`, for at most `limit`.  True when a stop was
    /// requested.
    bool wait_for_stop_request(std::chrono::milliseconds limit);

    server_counters counters() const;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vss::agent
