//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/agent/proto/message.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/core/types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace vss::agent
{

struct client_options
{
    /// The instance name; empty for the user's ordinary agent.
    std::string instance;
    /// The program that connects, for the agent's log.
    std::string name = "client";
    /// How long one request may take before the connection is given up.
    std::chrono::milliseconds timeout{10000};
};

/// A connection to this user's agent, for a program that asks and waits: the
/// command line tool and the tests.  A call blocks the calling thread until
/// the agent answers or the time is up.  One thread at a time.
///
/// An error from the engine arrives as the engine's own error code
/// (`vss::errc`), so a caller handles it as it would handle the studio's;
/// `last_error_message` has the agent's wording.  When the agent goes away
/// every call fails with `protocol_errc::connection_closed`.
class client
{
public:
    /// Connects and says hello.  `protocol_errc::not_running` when no agent
    /// listens, `version_mismatch` when it speaks another protocol version.
    static result<std::unique_ptr<client>>
    connect(const client_options& options = {});

    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /// What the agent said about itself.
    const hello_reply& agent() const noexcept;

    result<list_reply> list();
    result<endpoint_id> create_port(const std::string& name,
                                    const std::string& kind = "loopback");
    result<void> remove_port(endpoint_id endpoint);
    result<link_id> connect_ports(endpoint_id first, endpoint_id second,
                                  bool one_way);
    result<void> write(endpoint_id endpoint, bytes_view data);
    /// Starts the event stream.  With `data` the batches carry the bytes of
    /// the traffic, which makes this connection a read-only tap.
    result<void>
    subscribe(bool data, std::uint32_t interval_ms = default_event_interval_ms);
    /// Asks the agent to close every port and exit.
    result<void> shutdown();

    /// The next batch of events, waiting up to `limit` for it.
    /// `errc::timeout` when none came; the connection stays usable.
    result<event_batch> next_events(std::chrono::milliseconds limit);

    /// The message of the last error the agent sent, in its words.
    const std::string& last_error_message() const noexcept;

private:
    class impl;
    explicit client(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

} // namespace vss::agent
