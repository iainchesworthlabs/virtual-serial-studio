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
    //
    // What it does give away: on Linux a socket that another program bound to
    // the port earlier, with SO_REUSEADDR too and without listening, no longer
    // stops the gateway from starting, and takes the port the moment the
    // gateway stops.  After a stop any program that sets SO_REUSEADDR can bind
    // the port at once, where it would have waited for the closed connections.
    // Neither is more than another program holding a free port, which is the
    // case the token handshake of the control endpoint is for, and a program
    // that listens before the gateway starts still stops it with EADDRINUSE.
    const int enabled = 1;
    static_cast<void>(::setsockopt(static_cast<int>(native_socket), SOL_SOCKET,
                                   SO_REUSEADDR, &enabled, sizeof(enabled)));
}

} // namespace vss::control::platform
