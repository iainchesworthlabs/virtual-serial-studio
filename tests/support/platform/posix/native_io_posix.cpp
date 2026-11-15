//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `native_io` on POSIX systems (Linux and macOS): poll for the wait, read and
// write for the transfer.
//

#include "native_io.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vss_test_support
{

namespace
{

using steady = std::chrono::steady_clock;

/// The longest any wait lasts, so that a huge limit cannot overflow a clock.
constexpr std::chrono::milliseconds longest_wait{std::chrono::hours{24}};

steady::time_point deadline_after(std::chrono::milliseconds limit)
{
    return steady::now() +
           std::clamp(limit, std::chrono::milliseconds{0}, longest_wait);
}

/// Whole milliseconds from now until `deadline`, rounded up, and never
/// negative.
int milliseconds_until(steady::time_point deadline)
{
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - steady::now());
    return static_cast<int>(
        std::clamp<std::int64_t>(left.count(), 0, std::int64_t{86'400'000}));
}

/// Waits until the descriptor is ready for `events` or `deadline` passes.
/// True when it is ready (an error or a hang-up counts: the next read or write
/// reports it).
bool wait_for(int fd, short events, steady::time_point deadline)
{
    for (;;)
    {
        pollfd watch{fd, events, 0};
        const int ready = ::poll(&watch, 1, milliseconds_until(deadline));
        if (ready > 0)
            return true;
        if (ready == 0)
            return false;
        if (errno != EINTR)
            return false;
    }
}

} // namespace

std::optional<std::string> read_some(std::intptr_t handle,
                                     std::chrono::milliseconds limit)
{
    const int fd = static_cast<int>(handle);
    const auto deadline = deadline_after(limit);
    for (;;)
    {
        if (!wait_for(fd, POLLIN, deadline))
            return std::nullopt;
        std::array<char, 4096> chunk;
        const auto got = ::read(fd, chunk.data(), chunk.size());
        if (got > 0)
            return std::string(chunk.data(), static_cast<std::size_t>(got));
        if (got == 0)
            return std::string{};
        if (errno == EINTR || errno == EAGAIN)
            continue;
        // The other end was closed with bytes of its own still unread: for a
        // test that is the end of the stream.
        if (errno == ECONNRESET)
            return std::string{};
        return std::nullopt;
    }
}

bool write_all(std::intptr_t handle, std::string_view bytes,
               std::chrono::milliseconds limit)
{
    const int fd = static_cast<int>(handle);
    const auto deadline = deadline_after(limit);
    std::size_t done = 0;
    while (done < bytes.size())
    {
        if (!wait_for(fd, POLLOUT, deadline))
            return false;
        const auto put = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (put >= 0)
        {
            done += static_cast<std::size_t>(put);
            continue;
        }
        if (errno != EINTR && errno != EAGAIN)
            return false;
    }
    return true;
}

void close_handle(std::intptr_t handle)
{
    static_cast<void>(::close(static_cast<int>(handle)));
}

std::uint64_t own_process_id()
{
    return static_cast<std::uint64_t>(::getpid());
}

bool process_exists(std::uint64_t pid)
{
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    // Another user's process exists, and this one may not signal it.
    return errno == EPERM;
}

std::intptr_t open_inheritable_handle()
{
    // No O_CLOEXEC: a program started from here would inherit it.
    return static_cast<std::intptr_t>(::open("/dev/null", O_RDONLY));
}

} // namespace vss_test_support
