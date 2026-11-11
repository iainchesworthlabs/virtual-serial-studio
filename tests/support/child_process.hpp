//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Another program of this project, started as a child process by a system
// test.  The platform half lives in platform/posix and platform/windows;
// CMake builds the one that fits.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vss_test_support
{

/// Another program of this project, started as a child process with its
/// standard output and standard error captured together.
class child_process
{
public:
    /// Starts `program` with `arguments`.  Throws `std::runtime_error` when it
    /// cannot be started.
    child_process(const std::string& program,
                  const std::vector<std::string>& arguments);

    /// Kills the child if it still runs, and waits for it.
    ~child_process();

    child_process(const child_process&) = delete;
    child_process& operator=(const child_process&) = delete;

    /// Waits up to `limit` for the child to exit.  The exit code, or nothing
    /// on timeout.  A child ended by a signal (POSIX) reports 128 plus the
    /// signal number.
    std::optional<int> wait(std::chrono::milliseconds limit);

    /// Ends the child at once, with no chance to clean up (SIGKILL or
    /// TerminateProcess), and waits for it.
    void kill();

    /// Asks the child to stop the polite way: SIGTERM on POSIX,
    /// CTRL_BREAK_EVENT on Windows.  False when the request could not be
    /// delivered.
    bool request_stop();

    /// One line of output without its line ending ("\n" or "\r\n"); nothing
    /// on timeout, or when the output has ended and no partial line is left.
    std::optional<std::string> read_line(std::chrono::milliseconds limit);

    /// Everything the child printed that has not been read yet, read until the
    /// output ends or `limit` passes.
    std::string read_all(std::chrono::milliseconds limit);

    /// The operating system's process id.
    std::uint64_t id() const noexcept;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vss_test_support
