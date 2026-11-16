//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system half of the test plugin on POSIX systems (Linux and macOS).
//

#include "platform.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern char** environ;

namespace test_plugin
{

void binary_output() {}

std::optional<std::intptr_t> channel_handle()
{
    const char* text = std::getenv("VSS_PLUGIN_CHANNEL");
    if (text == nullptr)
        return std::nullopt;
    const std::string_view digits(text);
    std::intptr_t value = 0;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
        return std::nullopt;
    return value;
}

long long read_handle(std::intptr_t handle, char* data, std::size_t size)
{
    for (;;)
    {
        const auto got = ::read(static_cast<int>(handle), data, size);
        if (got >= 0)
            return static_cast<long long>(got);
        if (errno == EINTR)
            continue;
        // The other end was closed with bytes of its own still unread.
        if (errno == ECONNRESET)
            return 0;
        return -1;
    }
}

bool write_handle(std::intptr_t handle, const char* data, std::size_t size)
{
    // A write to a stream whose other end is gone is an error, not a signal.
    static_cast<void>(::signal(SIGPIPE, SIG_IGN));
    std::size_t done = 0;
    while (done < size)
    {
        const auto put =
            ::write(static_cast<int>(handle), data + done, size - done);
        if (put < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += static_cast<std::size_t>(put);
    }
    return true;
}

std::string open_descriptors()
{
    DIR* directory = ::opendir("/dev/fd");
    if (directory == nullptr)
        return "unlisted";
    const int own = ::dirfd(directory);

    std::vector<int> numbers;
    while (const dirent* entry = ::readdir(directory))
    {
        const std::string_view name(entry->d_name);
        int number = 0;
        const auto result =
            std::from_chars(name.data(), name.data() + name.size(), number);
        if (result.ec != std::errc{} ||
            result.ptr != name.data() + name.size() || number == own)
        {
            continue;
        }
        numbers.push_back(number);
    }
    static_cast<void>(::closedir(directory));

    std::sort(numbers.begin(), numbers.end());
    std::string text;
    for (const int number : numbers)
    {
        if (!text.empty())
            text += ',';
        text += std::to_string(number);
    }
    return text;
}

bool handle_is_open(std::intptr_t handle)
{
    return ::fcntl(static_cast<int>(handle), F_GETFD) != -1;
}

std::vector<std::string> environment()
{
    std::vector<std::string> entries;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item)
        entries.emplace_back(*item);
    return entries;
}

long long start_idle_child(const char* self)
{
    const pid_t child = ::fork();
    if (child < 0)
        return -1;
    if (child == 0)
    {
        ::execl(self, self, "--hang", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return static_cast<long long>(child);
}

void ignore_stop_request()
{
    static_cast<void>(::signal(SIGTERM, SIG_IGN));
}

void crash()
{
    // No core file: a test that crashes on purpose leaves nothing behind.
    const rlimit none{0, 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &none));
    static_cast<void>(::signal(SIGABRT, SIG_DFL));
    std::abort();
}

long long process_id()
{
    return static_cast<long long>(::getpid());
}

bool connect_loopback(unsigned short port)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0)
        return false;
    // Not blocking, and three seconds at most.
    ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr generic{};
    std::memcpy(&generic, &address, sizeof(generic));
    bool connected = ::connect(descriptor, &generic, sizeof(generic)) == 0;
    if (!connected && errno == EINPROGRESS)
    {
        pollfd waiting{};
        waiting.fd = descriptor;
        waiting.events = POLLOUT;
        if (::poll(&waiting, 1, 3000) == 1)
        {
            int failure = 0;
            socklen_t size = sizeof(failure);
            connected = ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &failure,
                                     &size) == 0 &&
                        failure == 0;
        }
    }
    ::close(descriptor);
    return connected;
}

bool can_open_process(long long pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

} // namespace test_plugin
