//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "loopback_platform.hpp"

#include <sys/socket.h>

namespace vss::control::platform
{

void claim_listening_port(std::uintptr_t native_socket) noexcept
{
    // The gateway closes a connection first when it stops, refuses a hello,
    // kicks or revokes, so its side of the port is in TIME_WAIT for a minute
    // and a bind without this option fails with EADDRINUSE.  With it the bind
    // succeeds then.  It still does not let anyone else bind the port while
    // this socket listens: on Linux a listening socket always blocks it, and
    // on macOS and the BSDs a second bind of the same address and port needs
    // SO_REUSEPORT on both, which nothing here sets.
    const int enabled = 1;
    static_cast<void>(::setsockopt(static_cast<int>(native_socket), SOL_SOCKET,
                                   SO_REUSEADDR, &enabled, sizeof(enabled)));
}

} // namespace vss::control::platform
