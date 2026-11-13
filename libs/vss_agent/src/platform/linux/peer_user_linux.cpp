//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is at the other end of a Unix socket, on Linux: the kernel records the
// credentials of the process that connected and answers SO_PEERCRED with them.
//

#include "platform/posix/peer_user.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>

namespace vss::agent::platform
{

result<std::uint32_t> peer_user(int descriptor)
{
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &size) != 0)
        return error_code(errno, boost::system::system_category());
    return static_cast<std::uint32_t>(credentials.uid);
}

std::uint64_t peer_process(int descriptor) noexcept
{
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &size) != 0 ||
        credentials.pid <= 0)
        return 0;
    return static_cast<std::uint64_t>(credentials.pid);
}

std::string process_program(std::uint64_t process)
{
    if (process == 0)
        return {};
    const std::string link = "/proc/" + std::to_string(process) + "/exe";
    std::array<char, PATH_MAX> target{};
    const ssize_t size = ::readlink(link.c_str(), target.data(), target.size());
    if (size <= 0)
        return {};
    return {target.data(), static_cast<std::size_t>(size)};
}

} // namespace vss::agent::platform
