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

#include <cerrno>

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

} // namespace vss::agent::platform
