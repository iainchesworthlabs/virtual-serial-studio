//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin as a process of the operating system: started with a private
// channel and nothing else of the host's, its output captured, and ended
// together with everything it started.  docs/plugins.md, sections 5.4, 5.6
// and 7.1, are the design.
//
// What a plugin's process gets, and all it gets:
//
// - Its program, started directly with the arguments given.  There is no
//   shell, and no argument is read as anything but itself.
// - The environment given, plus `VSS_PLUGIN_CHANNEL`.  Nothing of the
//   host's own environment is passed on.
// - Standard input that is at its end at once, and one pipe as both
//   standard output and standard error, which the host reads.
// - The channel: a two-way stream of bytes that only the host holds the
//   other end of.  It has no name another program can connect to once the
//   plugin runs.  `VSS_PLUGIN_CHANNEL` is its number in the plugin: a file
//   descriptor, or on Windows the value of a handle.
// - No other descriptor or handle of the host.
//
// A request may carry a sandbox profile (sandbox.hpp).  Windows applies
// all of it (a job object and an AppContainer).  Linux and macOS apply
// nothing of it yet and report the level `none`.  Nothing in Studio starts
// a plugin.
//
// The process is the head of a group of its own (a job object on Windows; a
// session and process group elsewhere), so that `kill` ends what it started
// as well.  Without the sandbox a child that moves itself to another
// session escapes that on Linux and macOS.
//
// A `plugin_process` starts no thread.  One thread at a time may use it.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/sandbox.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

/// A handle of the operating system as a number: a file descriptor, or on
/// Windows the value of a `HANDLE`.
using native_handle = std::intptr_t;

/// The value no handle has.
inline constexpr native_handle no_handle = -1;

/// The name of the variable that tells a plugin the number of its channel.
inline constexpr std::string_view channel_variable = "VSS_PLUGIN_CHANNEL";

struct process_request
{
    /// The program, as an absolute path.
    std::filesystem::path program;
    /// The arguments after the program's own name.  UTF-8.  Each reaches
    /// the program exactly as written, whatever characters it holds, except
    /// that none may hold a zero byte.
    std::vector<std::string> arguments;
    /// The directory the program starts in, as an absolute path.
    std::filesystem::path working_directory;
    /// The whole environment, as names and values in UTF-8.  A name is not
    /// empty and holds no '=' and no zero byte; a value holds no zero byte;
    /// no name comes twice, and `VSS_PLUGIN_CHANNEL` is not among them.
    std::vector<std::pair<std::string, std::string>> environment;
    /// What to confine the process to.  Without it nothing is applied but
    /// the group that `kill` ends.  What was really applied is in
    /// `plugin_process::sandbox`, and may be less than was asked for: the
    /// start does not fail because a part could not be applied.
    std::optional<sandbox_profile> sandbox;
    /// The least level the start may reach.  When this computer cannot
    /// apply that much, nothing is started and the start fails with
    /// `plugin_errc::sandbox_insufficient`.
    sandbox_level least = sandbox_level::none;
};

/// The environment of section 7.1 for a plugin: `VSS_PLUGIN_ID`,
/// `VSS_PLUGIN_DATA`, `HOME` (the data directory), a `PATH` of the system's
/// own directories only, and `LANG`.  On Windows also `SystemRoot`, which
/// the system's own libraries need.  None of it is read from the host's
/// environment.
std::vector<std::pair<std::string, std::string>>
plugin_environment(std::string_view plugin_id,
                   const std::filesystem::path& data_directory);

/// How a process ended.
struct exit_status
{
    /// The process was ended by a signal, or on Windows by an exception
    /// nothing handled (an exit code that is an error `NTSTATUS`).  A
    /// process the host killed reports true as well.
    bool abnormal = false;
    /// The exit code; the number of the signal when `abnormal` elsewhere
    /// than on Windows.
    std::int64_t code = 0;
};

class plugin_process
{
public:
    /// Starts the program.  It runs when this returns.
    ///
    /// Fails with `plugin_errc::start_failed` when the request breaks a
    /// rule above, and with the system's own error when the system refuses
    /// (no such program, not allowed, out of memory).  Nothing is left
    /// running and nothing is left open after a failure.
    static result<plugin_process> start(const process_request& request);

    /// Kills the process and what it started, waits for the process to be
    /// gone and closes the handles still held.
    ~plugin_process();

    plugin_process(plugin_process&&) noexcept;
    plugin_process& operator=(plugin_process&&) noexcept;
    plugin_process(const plugin_process&) = delete;
    plugin_process& operator=(const plugin_process&) = delete;

    /// What confines the process: the parts that were applied when it was
    /// started.
    sandbox_report sandbox() const;

    /// The system's number for the process.
    std::uint64_t id() const noexcept;

    /// The host's end of the channel.  On Windows a pipe handle opened for
    /// overlapped I/O; elsewhere a stream socket.  `no_handle` once taken.
    native_handle channel() const noexcept;

    /// The end of the pipe that the plugin's standard output and standard
    /// error arrive on, for reading.  On Windows opened for overlapped I/O.
    /// It reaches its end when the plugin and everything that inherited the
    /// pipe from it have gone.  `no_handle` once taken.
    native_handle output() const noexcept;

    /// Hands a handle over.  The caller closes it; this object forgets it.
    native_handle take_channel() noexcept;
    native_handle take_output() noexcept;

    /// Asks the process and its group to end, the way the system asks:
    /// `SIGTERM`.  Windows has no such request for a process without a
    /// window or console, so there this does nothing and returns false.
    /// False as well when the process has already been seen to end.
    bool request_stop() noexcept;

    /// Ends the process and everything it started that is still in its
    /// group, at once and without asking.  Safe to call at any time, and
    /// more than once.
    void kill() noexcept;

    /// How the process ended, when it has; nothing while it runs.  It does
    /// not wait.  When the process is first seen to have ended, what it
    /// started and left behind is killed.
    std::optional<exit_status> poll() noexcept;

    /// The same, waiting up to `limit` for the end.
    std::optional<exit_status> wait(std::chrono::milliseconds limit) noexcept;

    class impl;

private:
    explicit plugin_process(std::unique_ptr<impl> state) noexcept;
    std::unique_ptr<impl> impl_;
};

namespace detail
{

/// The command line Windows programs take apart again into exactly these
/// words (the rules of the C runtime and of `CommandLineToArgvW`).  Here,
/// and tested on every system, because a mistake in it lets an argument be
/// read as two.
std::wstring windows_command_line(const std::vector<std::wstring>& words);

/// Whether the request keeps the rules `process_request` states.
bool valid_request(const process_request& request) noexcept;

} // namespace detail

} // namespace vss::plugin::host
