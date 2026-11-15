//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system half of the test plugin on Windows.  The channel may be a
// handle opened for overlapped I/O or not, so every read and write is made
// with an OVERLAPPED structure and waited for, which suits both.
//

#include "platform.hpp"

#include <winsock2.h>

#include <windows.h>

#include <cstring>

#include <fcntl.h>
#include <io.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace test_plugin
{

namespace
{

/// A Windows event that closes itself.
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

HANDLE as_handle(std::intptr_t value)
{
    // The number is a HANDLE the operating system gave out.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<HANDLE>(value);
}

/// Whether the error is how Windows says a pipe has reached its end.
bool is_end_of_stream(DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
           error == ERROR_PIPE_NOT_CONNECTED;
}

/// Waits for a transfer that has been started with `overlapped`.  The number
/// of bytes moved is stored in `done`; the result is 1 for a transfer made,
/// 0 for the end of the stream and -1 for an error.
int finish(HANDLE handle, OVERLAPPED& overlapped, DWORD& done)
{
    done = 0;
    if (::GetOverlappedResult(handle, &overlapped, &done, TRUE))
        return 1;
    return is_end_of_stream(::GetLastError()) ? 0 : -1;
}

} // namespace

void binary_output()
{
    static_cast<void>(::_setmode(::_fileno(stdin), _O_BINARY));
    static_cast<void>(::_setmode(::_fileno(stdout), _O_BINARY));
    static_cast<void>(::_setmode(::_fileno(stderr), _O_BINARY));
}

std::optional<std::intptr_t> channel_handle()
{
    std::array<char, 64> text{};
    const DWORD length = ::GetEnvironmentVariableA(
        "VSS_PLUGIN_CHANNEL", text.data(), static_cast<DWORD>(text.size()));
    if (length == 0 || length >= text.size())
        return std::nullopt;
    const std::string_view digits(text.data(), length);
    std::intptr_t value = 0;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
        return std::nullopt;
    return value;
}

long long read_handle(std::intptr_t handle, char* data, std::size_t size)
{
    const event_handle event;
    if (event.get() == nullptr)
        return -1;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();

    const DWORD wanted = size > 65536 ? 65536 : static_cast<DWORD>(size);
    DWORD done = 0;
    if (!::ReadFile(as_handle(handle), data, wanted, &done, &overlapped))
    {
        const DWORD error = ::GetLastError();
        if (error != ERROR_IO_PENDING)
            return is_end_of_stream(error) ? 0 : -1;
    }
    const int state = finish(as_handle(handle), overlapped, done);
    if (state <= 0)
        return state;
    return static_cast<long long>(done);
}

bool write_handle(std::intptr_t handle, const char* data, std::size_t size)
{
    const event_handle event;
    if (event.get() == nullptr)
        return false;

    std::size_t sent = 0;
    while (sent < size)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        const std::size_t left = size - sent;
        const DWORD wanted = left > 65536 ? 65536 : static_cast<DWORD>(left);
        DWORD done = 0;
        if (!::WriteFile(as_handle(handle), data + sent, wanted, &done,
                         &overlapped))
        {
            if (::GetLastError() != ERROR_IO_PENDING)
                return false;
        }
        if (finish(as_handle(handle), overlapped, done) <= 0)
            return false;
        sent += done;
    }
    return true;
}

std::string open_descriptors()
{
    return "unsupported";
}

bool handle_is_open(std::intptr_t handle)
{
    DWORD flags = 0;
    return ::GetHandleInformation(as_handle(handle), &flags) != 0;
}

std::vector<std::string> environment()
{
    std::vector<std::string> entries;
    char* block = ::GetEnvironmentStringsA();
    if (block == nullptr)
        return entries;
    for (const char* item = block; *item != '\0';)
    {
        const std::string_view entry(item);
        // The system keeps the current directory of each drive in entries
        // whose name starts with '='; they are not variables of the plugin.
        if (entry.front() != '=')
            entries.emplace_back(entry);
        item += entry.size() + 1;
    }
    static_cast<void>(::FreeEnvironmentStringsA(block));
    return entries;
}

long long start_idle_child(const char* self)
{
    std::array<char, 32768> path{};
    const DWORD length = ::GetModuleFileNameA(nullptr, path.data(),
                                              static_cast<DWORD>(path.size()));
    const bool known = length > 0 && length < path.size();
    const std::string program =
        known ? std::string(path.data(), length) : std::string(self);

    std::string command_line = "\"";
    command_line += program;
    command_line += "\" --hang";

    STARTUPINFOA startup{};
    startup.cb = static_cast<DWORD>(sizeof startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION started{};
    if (!::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                          0, nullptr, nullptr, &startup, &started))
    {
        return -1;
    }
    static_cast<void>(::CloseHandle(started.hThread));
    static_cast<void>(::CloseHandle(started.hProcess));
    return static_cast<long long>(started.dwProcessId);
}

void ignore_stop_request() {}

void crash()
{
    static_cast<void>(
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX));
    ::RaiseException(0xC0000005u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

long long process_id()
{
    return static_cast<long long>(::GetCurrentProcessId());
}

bool connect_loopback(unsigned short port)
{
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return false;
    bool connected = false;
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket != INVALID_SOCKET)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        sockaddr generic{};
        std::memcpy(&generic, &address, sizeof(generic));
        connected = ::connect(socket, &generic, sizeof(generic)) == 0;
        ::closesocket(socket);
    }
    ::WSACleanup();
    return connected;
}

bool can_open_process(long long pid)
{
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    ::CloseHandle(process);
    return true;
}

} // namespace test_plugin
