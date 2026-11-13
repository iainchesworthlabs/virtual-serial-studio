//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is at the other end of a Unix socket, on macOS: getpeereid gives the
// effective user and group of the process that connected.
//

#include "platform/posix/peer_user.hpp"

#include <libproc.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>

namespace vss::agent::platform
{

result<std::uint32_t> peer_user(int descriptor)
{
    uid_t user = 0;
    gid_t group = 0;
    if (::getpeereid(descriptor, &user, &group) != 0)
        return error_code(errno, boost::system::system_category());
    return static_cast<std::uint32_t>(user);
}

std::uint64_t peer_process(int descriptor) noexcept
{
    pid_t process = 0;
    socklen_t size = sizeof(process);
    if (::getsockopt(descriptor, SOL_LOCAL, LOCAL_PEERPID, &process, &size) !=
            0 ||
        process <= 0)
        return 0;
    return static_cast<std::uint64_t>(process);
}

std::string process_program(std::uint64_t process)
{
    if (process == 0)
        return {};
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    const int size = ::proc_pidpath(static_cast<int>(process), path.data(),
                                    static_cast<std::uint32_t>(path.size()));
    if (size <= 0)
        return {};
    return {path.data(), static_cast<std::size_t>(size)};
}

} // namespace vss::agent::platform
