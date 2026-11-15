//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the test plugin needs from the operating system and does differently
// on each one.  The implementations are in platform/posix and
// platform/windows; CMake builds the one that fits.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace test_plugin
{

/// Makes standard output and standard error write bytes as they are, so that
/// "\n" is not turned into "\r\n".  Does nothing where that is not a problem.
void binary_output();

/// The number in `VSS_PLUGIN_CHANNEL`: a file descriptor, or on Windows the
/// value of a handle.  Nothing when the variable is not set or is not a
/// number.
std::optional<std::intptr_t> channel_handle();

/// Reads at most `size` bytes, waiting for some.  The number of bytes read, 0
/// at the end of the stream (a broken pipe on Windows is the end) and -1 on
/// error.
long long read_handle(std::intptr_t handle, char* data, std::size_t size);

/// Writes all of the bytes.  False when that was not possible.
bool write_handle(std::intptr_t handle, const char* data, std::size_t size);

/// The numbers of the open file descriptors of this process in ascending
/// order, separated by commas ("0,1,2,3"), not counting the one used to look.
/// "unsupported" where the system has no way to list them.
std::string open_descriptors();

/// Whether the number is a descriptor or handle this process holds.
bool handle_is_open(std::intptr_t handle);

/// Every variable of the environment as "NAME=value".
std::vector<std::string> environment();

/// Starts this same program again (`self` is how it was started) with the
/// one argument `--hang`.  The child shares standard output and standard
/// error.  Its process id, or -1 when it could not be started.
long long start_idle_child(const char* self);

/// Makes this process ignore the request to stop (SIGTERM).  Does nothing on
/// Windows, which has no such request.
void ignore_stop_request();

/// Ends this process the way a fault does, with no dialog and no core file.
void crash();

/// The number the system gives this process.
long long process_id();

/// Whether a TCP connection to this computer itself (127.0.0.1) at `port`
/// could be made.
bool connect_loopback(unsigned short port);

/// Whether another process can be opened to be asked about (Windows) or
/// signalled (elsewhere).
bool can_open_process(long long pid);

} // namespace test_plugin
