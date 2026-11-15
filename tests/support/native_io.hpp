//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timed reads and writes on the raw handles of a plugin process (its output
// pipe and its channel), for the tests of the parent's side.  A handle is
// given as a number: a file descriptor, or on Windows the value of a handle
// opened for overlapped I/O.  The platform half lives in platform/posix and
// platform/windows; CMake builds the one that fits.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vss_test_support
{

/// Reads up to 4096 bytes, waiting up to `limit` for some.  An empty string
/// means the stream reached its end (on Windows a broken pipe is the end);
/// nothing means the limit passed or the read failed in some other way.
std::optional<std::string> read_some(std::intptr_t handle,
                                     std::chrono::milliseconds limit);

/// Writes all of the bytes within `limit`.  False on an error or when the
/// limit passed first.  The caller must not write to a stream whose other end
/// has gone: on POSIX that raises SIGPIPE.
bool write_all(std::intptr_t handle, std::string_view bytes,
               std::chrono::milliseconds limit);

/// Reads until what was read holds `marker`, the stream ends or `limit`
/// passes.  Everything read is returned, the marker included.
inline std::string read_until(std::intptr_t handle, std::string_view marker,
                              std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::string collected;
    while (collected.find(marker) == std::string::npos)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            break;
        const std::optional<std::string> part = read_some(handle, left);
        if (!part.has_value() || part->empty())
            break;
        collected += *part;
    }
    return collected;
}

/// Reads until the stream ends or `limit` passes.
inline std::string read_to_end(std::intptr_t handle,
                               std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::string collected;
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            break;
        const std::optional<std::string> part = read_some(handle, left);
        if (!part.has_value() || part->empty())
            break;
        collected += *part;
    }
    return collected;
}

/// Closes the handle.
void close_handle(std::intptr_t handle);

/// Whether a process with this number exists.  On POSIX a process that has
/// ended and has not been waited for yet still counts, so a test that waits
/// for one to go polls.
bool process_exists(std::uint64_t pid);

/// Something this process holds open and has marked inheritable, to show that
/// a plugin does NOT get it: an event on Windows, /dev/null opened without
/// close-on-exec elsewhere.  Close it with `close_handle`.  -1 when it could
/// not be made.
std::intptr_t open_inheritable_handle();

} // namespace vss_test_support
