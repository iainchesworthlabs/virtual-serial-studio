//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/control/proto/message.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/core/error.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vss::control
{

struct client_options
{
    /// The instance name of the gateway; empty for the user's ordinary one.
    std::string instance;
    /// The program that connects, for the gateway's audit log.
    std::string name = "client";
    /// Connect to the administration endpoint instead of the public one.
    bool admin = false;
    /// A token to connect as; empty to connect as the local user.  The
    /// administration endpoint takes none.
    secret_string token;
    /// Connect over TCP to `127.0.0.1` at this port instead of the local
    /// pipe or socket.  It needs a token made for `loopback`; `connect`
    /// fails with `errc::invalid_argument` without one, before it connects.
    /// There is no way to name another host.
    std::optional<std::uint16_t> loopback_port;
    /// How long one request may take before the connection is given up.
    std::chrono::milliseconds timeout{10000};
};

/// A connection to this user's gateway, for a program that asks and waits:
/// the command line tool and the tests.  A call blocks the calling thread
/// until the gateway answers or the time is up.  One thread at a time.
///
/// An error the gateway sent arrives as its `api_errc`, and
/// `last_error_message` has the gateway's wording.  When the gateway goes
/// away every call fails with `agent::protocol_errc::connection_closed`.
class client
{
public:
    /// Connects and says hello.  `agent::protocol_errc::not_running` when no
    /// gateway listens, `not_same_user` when what listens there runs as
    /// someone else, and `api_errc::unsupported` when the gateway speaks
    /// another major version of the API.
    static result<std::unique_ptr<client>>
    connect(const client_options& options = {});

    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /// What the gateway said about itself and about this connection.
    const hello_reply& gateway() const noexcept;

    result<status_reply> status();
    result<ports_reply> list_ports();

    /// Makes a port in the agent and gives its id (`p_3`).  It needs
    /// `control.ports`.  An error of the engine arrives as the engine's own
    /// code (`vss::errc`).
    result<std::string> create_port(const std::string& name,
                                    const std::string& kind = {});
    result<void> remove_port(const std::string& port);
    /// Links two ports, both ways or from `first` to `second` only, and
    /// gives the link's id (`l_2`).
    result<std::string> connect_ports(const std::string& first,
                                      const std::string& second, bool one_way);
    /// Writes bytes to a port.  It needs `control.data`, and takes
    /// `max_send_size` bytes at most.
    result<void> send(const std::string& port, bytes_view data);
    /// Asks the gateway to close every session and exit.  Only a connection
    /// to the administration endpoint may.
    result<void> stop();

    /// Makes a token with these scope names that lives `life_s` seconds (0
    /// for the usual 30 days).  The reply holds the token itself, this once.
    /// Only a connection to the administration endpoint may.
    result<token_created_reply>
    create_token(const std::string& label,
                 const std::vector<std::string>& scopes,
                 std::uint64_t life_s = 0,
                 const std::vector<std::string>& transports = {});
    /// The records of every token, revoked ones included.  Administration
    /// endpoint only.
    result<tokens_reply> list_tokens();
    /// Revokes a token and closes its sessions.  `api_errc::not_found` for
    /// an id no token has.  Administration endpoint only.
    result<void> revoke_token(const std::string& id);

    /// Waits in the gateway for a pattern on a port, after sending bytes
    /// when the request names a port to send to.  It needs `monitor.data`,
    /// and `control.data` to send.  `api_errc::timeout` when the pattern did
    /// not come in the request's time; the connection stays usable.  The
    /// call itself waits for the request's time on top of the usual limit.
    result<matched_reply> expect_bytes(const expect_bytes_request& wanted);

    /// Begins a named run for this principal; the reply has its id.
    /// `api_errc::conflict` while one is open.
    result<run_reply> begin_run(const std::string& name);
    /// Ends the open run and gives its counts.  `api_errc::not_found` when
    /// none is open.
    result<run_reply> end_run();
    /// Adds a check of the harness's own to the open run.  `outcome` is
    /// `passed`, `failed`, `error` or `skipped`.
    result<void> record_in_run(const runs_record_request& noted);
    /// A run as a JUnit XML document; an empty id means the latest.
    result<std::string> report_junit(const std::string& run = {});

    /// Starts the stream of what happens to the ports, without bytes, or
    /// changes its ports and its pace.  The reply says the pace the gateway
    /// took and the number of the next batch.
    result<subscribed_reply>
    subscribe_events(const stream_options& options = {});
    /// Starts the stream of the bytes of traffic.  It needs `monitor.data`.
    result<subscribed_reply>
    subscribe_traffic(const stream_options& options = {});
    result<void> unsubscribe_events();
    result<void> unsubscribe_traffic();

    /// The next batch of events, waiting up to `limit` for it.
    /// `errc::timeout` when none came; the connection stays usable.  With a
    /// limit of nothing it takes what has already arrived and does not wait.
    /// Batches that arrive while a request waits for its answer are kept,
    /// the newest 256 of them; `event_batch::seq` shows a gap.  When the
    /// gateway ends the session it says why first, and that is the error.
    result<event_batch> next_events(std::chrono::milliseconds limit);

    /// Who is connected.  Administration endpoint only.
    result<sessions_reply> list_sessions();
    /// Closes the session with this number.  `api_errc::not_found` when
    /// there is none.  Administration endpoint only.
    result<void> kick_session(std::uint64_t number);

    /// The message of the last error the gateway sent, in its words.
    const std::string& last_error_message() const noexcept;

private:
    class impl;
    explicit client(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

} // namespace vss::control
