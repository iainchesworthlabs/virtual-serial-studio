//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One plugin, kept running: its process started, its conversation with
// the host carried, its output kept as log lines, and the plugin started
// again after it fails until it has failed too often.  docs/plugins.md,
// sections 5.4 to 5.6, are the design.
//
// A runner joins the three pieces that hold the rules: `plugin_process`
// (the process and what confines it), `host_session` (who may say what,
// and when) and `plugin_run` (when to start again and when to give up).
// It adds the I/O and the timers and no rule of its own.
//
// Whether the plugin may run at all is decided before a runner is made:
// its files were verified, the user or the owner's signature trusts it,
// the licence covers it.  A runner starts what it is given.
//
// Threads.  A runner does all its work on a strand of the executor it is
// given.  `start`, `stop`, `request`, `notify` and `answer` may be called
// from any thread and return at once; none of them waits for the plugin.
// `snapshot` may be called from any thread and never waits.  The handlers
// a runner is given are called on the runner's strand, so they must not
// block and must not wait for the plugin either.
//
// A runner is held by `std::shared_ptr`.  Letting go of the last one from
// outside does not stop the plugin by itself: call `stop`, or `abandon` to
// end the plugin at once, as when the program exits.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/protocol.hpp>
#include <vss/plugin/session.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vss::plugin::host
{

struct runner_options
{
    /// How to start the plugin's process, with its sandbox profile and the
    /// least level the start may reach.
    process_request process;
    /// The rules of its conversation: its id and version, the versions of
    /// the plugin API, what the host tells it in "hello_ack", the limits.
    session_options session;
    /// When it is started again after a failure.
    restart_options restart;
    /// Chooses the random part of the waits between starts.
    std::uint64_t seed = 0;
    /// How many of the latest log lines a snapshot holds.
    std::size_t kept_log_lines = 200;
    /// How many bytes of standard output and error are read each second.
    /// What is over is dropped and counted.
    std::size_t output_bytes_per_second = std::size_t{64} * 1024;
    /// The longest line of standard output or error that is kept; a longer
    /// one is cut.
    std::size_t longest_output_line = 1024;
    /// How many bytes may wait to be sent to a plugin that does not read.
    /// One byte more and the plugin is treated as not responding.
    std::size_t most_bytes_waiting = std::size_t{4} * 1024 * 1024;
    /// How long a process that was asked to end has before it is killed.
    std::chrono::milliseconds end_grace{1000};
};

/// One line of a plugin's log, as plain text that was never trusted.
struct runner_log_line
{
    /// True for a line of standard output or error; false for a "log"
    /// message of the plugin API.
    bool from_output = false;
    log_level level = log_level::info;
    std::string text;
};

/// What is known of a plugin at one moment.  Never changes once made.
struct runner_snapshot
{
    run_state state = run_state::stopped;
    /// Why the plugin last failed, with one line of detail for a log.
    /// Success when it never has.
    error_code last_failure;
    std::string detail;
    /// How the process last ended, when it has.  A plugin that ends
    /// before it says anything is often one the system could not start:
    /// on Windows the code 3221225794 (0xC0000142) means a library it
    /// needs could not be loaded where it is confined.
    std::optional<exit_status> last_exit;
    /// How many failures count now, and how many quarantine the plugin.
    std::size_t failures = 0;
    std::size_t failure_limit = 0;
    /// When the next start is due, while backing off.
    std::optional<std::chrono::steady_clock::time_point> next_start;
    /// The process, while there is one; 0 otherwise.
    std::uint64_t process_id = 0;
    /// What confines the process that runs, or that ran last.
    sandbox_report sandbox;
    /// The version of the plugin API in use, once `ready`.
    api_version api;
    /// What the plugin last said of itself.
    std::optional<status_message> status;
    /// The latest log lines, oldest first, and how many were dropped
    /// because the plugin wrote faster than it may.
    std::vector<runner_log_line> log;
    /// How many log lines there have been in all.  The last of `log` is
    /// line number `log_total`, so a reader that remembers the number it
    /// last saw knows which lines are new, and how many it missed.
    std::uint64_t log_total = 0;
    std::uint64_t output_bytes_dropped = 0;
    /// Goes up by one with every snapshot of this runner.
    std::uint64_t revision = 0;
};

class plugin_runner : public std::enable_shared_from_this<plugin_runner>
{
public:
    /// Called on the runner's strand.
    using change_handler = std::function<void()>;
    /// A message of an extension point came from the plugin.  When it has
    /// an `id` it is a request: answer it with `answer`, or call
    /// `unknown` when nobody knows its "op".
    using message_handler = std::function<void(const api_message&)>;
    /// The answer to a request made with `request`: the plugin's answer,
    /// which may itself be an "error" message; or an error and an empty
    /// message (a timeout, or the plugin went away first).
    using answer_handler = std::function<void(error_code, const api_message&)>;

    /// A runner for one plugin.  Nothing is started yet.
    static std::shared_ptr<plugin_runner>
    create(const boost::asio::any_io_executor& executor,
           runner_options options);

    ~plugin_runner();
    plugin_runner(const plugin_runner&) = delete;
    plugin_runner& operator=(const plugin_runner&) = delete;

    /// Set once, before `start`.
    void on_change(change_handler handler);
    void on_message(message_handler handler);

    /// The plugin should run.  It is started, and started again after a
    /// failure as `plugin_run` says.  Starts a quarantined plugin afresh.
    void start();

    /// The plugin should stop: it is asked to, given the time the session
    /// allows, and then ended.  `reason` is what "shutdown" tells it.
    void stop(std::string reason);

    /// Ends the plugin at once without asking, and stops the runner for
    /// good.  For the end of the program: nothing is waited for.
    void abandon();

    /// The latest snapshot.  Any thread; never waits.
    std::shared_ptr<const runner_snapshot> snapshot() const;

    /// Sends a request to the plugin.  `handler` is called exactly once,
    /// on the runner's strand.
    void request(api_message message, answer_handler handler,
                 std::chrono::milliseconds timeout = {});

    /// Sends a message that wants no answer.  Dropped when the plugin is
    /// not `ready`.
    void notify(api_message message);

    /// Answers a request of the plugin; `re` is that request's `id`.
    void answer(std::uint64_t re, api_message message);

    /// Nobody knows the "op" of a message the plugin sent.
    void unknown(api_message message);

    class impl;

private:
    explicit plugin_runner(std::shared_ptr<impl> state);
    std::shared_ptr<impl> impl_;
};

} // namespace vss::plugin::host
