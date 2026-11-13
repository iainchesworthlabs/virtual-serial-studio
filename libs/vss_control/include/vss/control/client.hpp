//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/control/proto/message.hpp>
#include <vss/core/error.hpp>

#include <chrono>
#include <memory>
#include <string>

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
    /// Asks the gateway to close every session and exit.  Only a connection
    /// to the administration endpoint may.
    result<void> stop();

    /// The message of the last error the gateway sent, in its words.
    const std::string& last_error_message() const noexcept;

private:
    class impl;
    explicit client(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

} // namespace vss::control
