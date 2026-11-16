//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <cstdint>

namespace vss::control::platform
{

/// Sets the options of a listening socket before it is bound, so that its
/// port is its own while it listens, and can be bound again at once after the
/// listener has closed.  A socket that cannot be set so is left as it is: the
/// bind that follows fails or succeeds on its own.
///
/// POSIX: `SO_REUSEADDR`.  There it does not let a second socket bind an
/// address and port that a listening socket holds (that takes
/// `SO_REUSEPORT`, which is never set).  It lets a bind succeed while
/// connections that this program closed first are in `TIME_WAIT`, which is
/// the state of the port after the gateway stops with a client connected, and
/// lasts a minute on Linux and half of that on macOS.  The same rule works
/// the other way for a socket of another program that has the option too (see
/// the comment in the platform file).
///
/// Windows: `SO_EXCLUSIVEADDRUSE`, and never `SO_REUSEADDR`.  There
/// `SO_REUSEADDR` lets another program of any user bind a port that is in use
/// and take the connections meant for the gateway.  With exclusive use a
/// listener was bound again at once on the port it had closed, with a
/// connection still open at the stop and with connections in `TIME_WAIT`
/// (Windows 11, tested in test_control_loopback.cpp).
void claim_listening_port(std::uintptr_t native_socket) noexcept;

} // namespace vss::control::platform
