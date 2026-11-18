//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/debug/protocol.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vss::debug
{

/// A running Studio that has debug access on, as its instance file says.
struct instance_info
{
    std::string instance;
    std::uint64_t process_id = 0;
    std::string address;
};

/// The directory the servers publish themselves in, and clients read:
/// `<state folder>/debug/instances` next to the gateway's `control` folder.
result<std::filesystem::path> default_instance_directory();
/// The directory of the debug audit log, next to it.
result<std::filesystem::path> default_audit_directory();

/// The servers listed in `directory`.  A file whose server is gone may be
/// listed: `client::connect` is what says whether one answers.
std::vector<instance_info>
find_instances(const std::filesystem::path& directory);

struct client_options
{
    /// The instance to connect to (a process number).
    std::string instance;
    /// What this program calls itself.
    std::string name = "vss_cli";
    /// What it asks for.
    scope_set scopes = bit(scope::read);
    std::chrono::milliseconds timeout{10000};
};

/// A reply from the server.
struct reply
{
    bool ok = false;
    /// The `result` object as JSON text when `ok`.
    std::string json;
    /// The `error.code` and `error.message` when not.
    std::string code;
    std::string message;
    byte_buffer blob;
};

/// A blocking client of the debug server, for `vss_cli debug` and the tests.
/// One thread at a time.
class client
{
public:
    /// Connects and says hello.  The call waits while the person at the
    /// computer decides, up to the timeout.  `errc::permission_denied` when
    /// the person refused, `protocol_errc::not_running` when nothing
    /// listens.
    static result<std::unique_ptr<client>>
    connect(const client_options& options);

    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /// The scopes the person granted.
    scope_set granted() const noexcept;

    /// One request and its reply.  A refused or failed operation is a reply
    /// with `ok` false; an error is a connection that failed.
    result<reply> call(const std::string& operation,
                       const std::string& args_json = "{}");

    /// The next event (a `trace` event's JSON header), waiting up to `limit`.
    /// `errc::timeout` when none came.
    result<std::string> next_event(std::chrono::milliseconds limit);

private:
    class impl;
    explicit client(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

} // namespace vss::debug
