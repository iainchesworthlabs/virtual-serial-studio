//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/debug/protocol.hpp>
#include <vss/debug/provider.hpp>
#include <vss/debug/redact.hpp>
#include <vss/trace/recorder.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vss::debug
{

/// A program that has said hello and waits to be let in.
struct client_request
{
    /// What the program calls itself.  A hint for the person: the program
    /// path below is what the operating system says.
    std::string name;
    /// The path of the program that connected, as the system reports it.
    std::string program;
    std::uint64_t process_id = 0;
    scope_set wanted = 0;
};

/// The person's answer.
struct approval
{
    /// The scopes granted; 0 refuses the client.
    scope_set granted = 0;
    /// Remember it for this program until the server stops, so that the next
    /// command from the same program is not asked again.
    bool remember = false;
};

/// Asks the person.  May answer from any thread, at any time within the
/// approval timeout; not answering is a refusal.
using approve_function =
    std::function<void(const client_request&, std::function<void(approval)>)>;

/// A client that is connected.
struct client_view
{
    std::uint64_t id = 0;
    std::string name;
    std::string program;
    std::uint64_t process_id = 0;
    scope_set granted = 0;
    bool watching_trace = false;
    std::chrono::system_clock::time_point since;
};

struct server_options
{
    /// The address the clients use: empty means the process number, so two
    /// Studios on one computer never meet.
    std::string instance;
    /// What the server tells and does.  Required.
    std::shared_ptr<provider> source;
    /// Asks the person.  Without it every client is refused.
    approve_function approve;
    /// Applied to every string of every reply.
    redactor redact;
    /// Where the audit log is kept, and where a client finds this server.
    /// Both are made when missing.  Empty means no audit log, which only a
    /// test does.
    std::filesystem::path audit_directory;
    std::filesystem::path instance_directory;
    /// The calls that are listed.  Default: the global recorder.
    trace::recorder* recorder = nullptr;
    /// What `debug.status` says about the build and the process, as a JSON
    /// object.  Never holds a name, a path or a licence.
    std::string about_json = "{}";
    /// Called from the server's thread when a client comes or goes or the
    /// traffic switch changes, so a window can update its chip.
    std::function<void()> on_change;
    std::chrono::milliseconds hello_deadline{5000};
    std::chrono::milliseconds approval_timeout{60000};
    /// How long the traffic switch stays on once it is turned on.
    std::chrono::milliseconds traffic_window{std::chrono::minutes(10)};
    std::size_t max_clients = 4;
    /// Requests one client may make in a second.
    std::size_t max_requests_per_second = 50;
};

/// The debug server of one process: a pipe or socket only this user can open,
/// that exists while the server does.  See docs/debug-api.md.
///
/// Nothing listens until `start`, and destroying the server removes the
/// address.  The server runs on its own threads and never calls the window's
/// thread.
class server
{
public:
    /// `errc::invalid_argument` without a provider, `errc::permission_denied`
    /// or `protocol_errc::already_running` from the listener.
    static result<std::unique_ptr<server>> start(server_options options);

    ~server();
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    /// Stops listening, closes every client and removes the address.  Safe to
    /// call twice.
    void stop();

    /// The instance name clients use, and the pipe name or socket path.
    const std::string& instance() const noexcept;
    std::string address() const;

    std::vector<client_view> clients() const;
    /// Closes every client and forgets the approvals that were remembered.
    void disconnect_all();

    /// The person's second switch.  While it is on, a client with the data
    /// scope may ask for traffic; it turns itself off after the window.
    void set_traffic(bool on);
    bool traffic_on() const;

private:
    class impl;
    explicit server(std::shared_ptr<impl> state);
    std::shared_ptr<impl> impl_;
};

} // namespace vss::debug
