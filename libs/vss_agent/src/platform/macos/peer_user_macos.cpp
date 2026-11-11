//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is at the other end of a Unix socket, on macOS: getpeereid gives the
// effective user and group of the process that connected.
//

#include "platform/posix/peer_user.hpp"

#include <sys/types.h>
#include <unistd.h>

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

} // namespace vss::agent::platform
