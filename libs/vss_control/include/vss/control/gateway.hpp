//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/control/failure_limiter.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/policy.hpp>
#include <vss/control/token_store.hpp>
#include <vss/core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace vss::control
{

/// The version of the program, as `hello` reports it.
std::string gateway_version();

struct gateway_options
{
    /// The instance name of the gateway's own endpoints; empty for the
    /// user's ordinary gateway.  A test gives its gateway another name.
    std::string instance;
    /// The instance name of the agent this gateway serves.
    std::string agent_instance;
    /// What a connection to the public local endpoint may do.  `admin` in
    /// here is ignored: only the administration endpoint grants it.
    scope_set local_scopes = role_monitor();

    /// Sessions on the public endpoint at once, and how many of them one
    /// principal may have.  One more is told `busy` and closed.
    std::size_t max_sessions = 8;
    std::size_t max_sessions_per_principal = 4;
    /// Sessions on the administration endpoint at once.  They are counted
    /// apart, so a full public endpoint can still be stopped.
    std::size_t max_admin_sessions = 2;
    /// Connections that have not said `hello` yet, over both endpoints.
    std::size_t max_unauthenticated = 8;
    /// A connection that has not said `hello` by then is closed.
    std::chrono::milliseconds handshake_timeout{5000};

    /// Requests of one session that may wait for their turn.  The gateway
    /// stops reading from a session that has this many.
    std::size_t max_pending_requests = 32;
    /// Requests one session may send each second, and how many it may send
    /// at once after a pause.  One over is answered `limit_exceeded`.
    double requests_per_second = 200.0;
    double request_burst = 400.0;
    /// Replies that may wait for one session to read them, in bytes.  A
    /// session that lets more pile up is closed.
    std::size_t max_unsent_bytes = std::size_t{4} * 1024 * 1024;
    /// How long one request to the agent may take.
    std::chrono::milliseconds agent_timeout{10000};

    /// Where the audit log is kept; empty for `default_audit_directory()`.
    std::filesystem::path audit_directory;
    /// The file of the token records; empty for `tokens.json` in the audit
    /// directory.  A gateway whose token file cannot be read does not start.
    std::filesystem::path token_file;
    /// The most a token may do on the public local endpoint, whatever its
    /// own scopes say.
    scope_set local_ceiling = every_scope();
    /// How wrong tokens slow down whoever presents them.
    failure_limits auth_limits;
    /// The time the token store goes by; empty for the system clock.
    token_store::time_source clock;
    /// The shortest time between two audit records of the same kind of
    /// refusal from the same source.  The ones between are counted and the
    /// count is written with the next, so a flood cannot fill the log.
    std::chrono::milliseconds audit_burst_period{1000};
};

/// What the gateway has seen, for the tests and the log.
struct gateway_counters
{
    std::uint64_t accepted = 0;
    /// Connections closed because the operating system said another user
    /// made them, or would not say who did.
    std::uint64_t refused_peer = 0;
    std::uint64_t refused_busy = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t handshake_timeouts = 0;
    /// Tokens that were presented and refused, for any reason.
    std::uint64_t auth_failures = 0;
    std::uint64_t forbidden = 0;
    std::uint64_t rate_limited = 0;
    std::uint64_t slow_clients_closed = 0;
    /// Connections open now, and those of them that have said `hello`.
    std::uint64_t connections = 0;
    std::uint64_t sessions = 0;

    friend bool operator==(const gateway_counters&,
                           const gateway_counters&) = default;
};

/// The control endpoint (docs/remote-control.md): it serves the control API
/// to this user's programs over two local endpoints and is itself a client of
/// this user's agent.  Nothing here listens on a network interface.
///
/// Every connection is checked twice before a byte of it is used: by the
/// operating system (the pipe's access list, the modes of the socket and its
/// directory) and by the gateway, which asks the kernel who is at the other
/// end.  Every request is then decided by `authorise`, and what was decided
/// is written to the audit log.
///
/// Threads: one does all I/O with the clients, and a small pool runs the
/// requests that wait for the agent, one session's requests in order.  Each
/// session has its own connection to the agent.
class gateway
{
public:
    explicit gateway(gateway_options options = {});
    ~gateway();

    gateway(const gateway&) = delete;
    gateway& operator=(const gateway&) = delete;

    /// Opens the audit log and starts listening on both endpoints.
    /// `agent::protocol_errc::already_running` when a gateway of this user
    /// and instance name already listens, `errc::permission_denied` when an
    /// address or the audit directory belongs to someone else.
    result<void> start();

    /// Closes every session and stops the threads.  Requests that are
    /// running finish first.  Safe to call twice.
    void stop();

    /// The pipe names or socket paths, once started.
    std::string address() const;
    std::string admin_address() const;

    /// Asks whoever waits in `wait_for_stop_request` to stop the gateway.
    /// Safe from any thread.
    void request_stop();

    /// Blocks until `endpoint.stop` arrived or `request_stop` was called.
    void wait_for_stop_request();

    /// As above, for at most `limit`.  True when a stop was requested.
    bool wait_for_stop_request(std::chrono::milliseconds limit);

    gateway_counters counters() const;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vss::control
