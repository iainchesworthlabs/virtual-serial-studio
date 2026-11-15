//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `native_io` on Windows: the handles are opened for overlapped I/O, so a
// read or write is started with an OVERLAPPED structure that holds an event,
// waited for with a limit and cancelled when the limit passes.
//

#include "native_io.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
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
DWORD milliseconds_until(steady::time_point deadline)
{
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - steady::now());
    return static_cast<DWORD>(
        std::clamp<std::int64_t>(left.count(), 0, std::int64_t{86'400'000}));
}

HANDLE as_handle(std::intptr_t value)
{
    // The number is a HANDLE the operating system gave out.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<HANDLE>(value);
}

/// An event that closes itself.
class event_handle
{
public:
    event_handle() : value_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~event_handle()
    {
        if (value_ != nullptr)
            static_cast<void>(::CloseHandle(value_));
    }

    event_handle(const event_handle&) = delete;
    event_handle& operator=(const event_handle&) = delete;

    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

/// How a transfer ended.
enum class outcome
{
    done,
    ended,
    timed_out,
    failed
};

/// Whether the error is how Windows says a pipe has reached its end.
bool is_end_of_stream(DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
           error == ERROR_PIPE_NOT_CONNECTED;
}

outcome classify(DWORD error)
{
    return is_end_of_stream(error) ? outcome::ended : outcome::failed;
}

/// Waits for a transfer started with `overlapped` until `deadline`.  When the
/// deadline passes first the transfer is cancelled and waited for again, so
/// that the OVERLAPPED is no longer in use when this returns; bytes that
/// arrived during the cancel still count as done.
outcome finish(HANDLE handle, OVERLAPPED& overlapped, bool pending,
               steady::time_point deadline, DWORD& moved)
{
    moved = 0;
    if (pending)
    {
        const DWORD waited = ::WaitForSingleObject(
            overlapped.hEvent, milliseconds_until(deadline));
        if (waited != WAIT_OBJECT_0)
        {
            static_cast<void>(::CancelIoEx(handle, &overlapped));
            if (::GetOverlappedResult(handle, &overlapped, &moved, TRUE))
                return outcome::done;
            const DWORD error = ::GetLastError();
            return error == ERROR_OPERATION_ABORTED ? outcome::timed_out
                                                    : classify(error);
        }
    }
    if (::GetOverlappedResult(handle, &overlapped, &moved, TRUE))
        return outcome::done;
    return classify(::GetLastError());
}

} // namespace

std::optional<std::string> read_some(std::intptr_t handle,
                                     std::chrono::milliseconds limit)
{
    const auto deadline = deadline_after(limit);
    const event_handle event;
    if (event.get() == nullptr)
        return std::nullopt;

    for (;;)
    {
        // Declared in front of the OVERLAPPED so that it outlives it.
        std::array<char, 4096> chunk;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        static_cast<void>(::ResetEvent(event.get()));

        DWORD moved = 0;
        bool pending = false;
        if (!::ReadFile(as_handle(handle), chunk.data(),
                        static_cast<DWORD>(chunk.size()), &moved, &overlapped))
        {
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                if (is_end_of_stream(error))
                    return std::string{};
                return std::nullopt;
            }
            pending = true;
        }

        switch (finish(as_handle(handle), overlapped, pending, deadline, moved))
        {
        case outcome::done:
            if (moved > 0)
                return std::string(chunk.data(), moved);
            // A read that moved nothing is not the end of a pipe: try again
            // while there is time.
            if (steady::now() >= deadline)
                return std::nullopt;
            break;
        case outcome::ended:
            return std::string{};
        case outcome::timed_out:
        case outcome::failed:
            return std::nullopt;
        }
    }
}

bool write_all(std::intptr_t handle, std::string_view bytes,
               std::chrono::milliseconds limit)
{
    const auto deadline = deadline_after(limit);
    const event_handle event;
    if (event.get() == nullptr)
        return false;

    std::size_t sent = 0;
    while (sent < bytes.size())
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        static_cast<void>(::ResetEvent(event.get()));

        const std::size_t left = bytes.size() - sent;
        const DWORD wanted = left > 65536 ? 65536 : static_cast<DWORD>(left);
        DWORD moved = 0;
        bool pending = false;
        if (!::WriteFile(as_handle(handle), bytes.data() + sent, wanted, &moved,
                         &overlapped))
        {
            if (::GetLastError() != ERROR_IO_PENDING)
                return false;
            pending = true;
        }
        if (finish(as_handle(handle), overlapped, pending, deadline, moved) !=
            outcome::done)
        {
            return false;
        }
        sent += moved;
    }
    return true;
}

void close_handle(std::intptr_t handle)
{
    static_cast<void>(::CloseHandle(as_handle(handle)));
}

std::uint64_t own_process_id()
{
    return ::GetCurrentProcessId();
}

bool process_exists(std::uint64_t pid)
{
    HANDLE process =
        ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                      static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    // A process that has ended can still be opened while something holds a
    // handle to it, so ask whether it is still running.
    const bool running = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    static_cast<void>(::CloseHandle(process));
    return running;
}

std::intptr_t open_inheritable_handle()
{
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = static_cast<DWORD>(sizeof inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE event = ::CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return -1;
    return reinterpret_cast<std::intptr_t>(event);
}

} // namespace vss_test_support
