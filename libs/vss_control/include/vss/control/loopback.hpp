//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/agent/local_stream.hpp>
#include <vss/core/error.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace vss::control
{

/// What every connection over TCP begins with: `VSSCTL` and the major
/// version of the API, eight bytes.  A connection that begins with anything
/// else is closed and told nothing, so a browser, an HTTP client and a port
/// scanner learn only that something closed the connection
/// (docs/remote-control.md, section 3).
inline constexpr std::array<std::uint8_t, 8> control_preface{'V', 'S', 'S', 'C',
                                                             'T', 'L', 0,   1};

/// Listens for the control API on the loopback addresses, `127.0.0.1` and
/// `::1`, and on nothing else: the caller chooses a port and never an
/// address, so the listener cannot be made to face a network.  A port of 0
/// takes any free one, for tests.
///
/// The port must be free on both addresses.  One that is taken is an error
/// and never a reason to listen somewhere else.  A computer without IPv6
/// listens on `127.0.0.1` alone.
///
/// A connection that somehow arrives from an address that is not loopback
/// is closed and never handed on.  `address()` is `127.0.0.1:<port>`.
///
/// Who is at the other end of a TCP connection cannot be asked of the
/// system without a race, so `peer()` of an accepted stream says only where
/// it came from and never that it is this user: whoever connects must
/// present a token.
result<std::unique_ptr<agent::local_listener>>
listen_loopback(const boost::asio::any_io_executor& executor,
                std::uint16_t port);

/// Connects to `127.0.0.1:<port>`.  There is no way to name another host: a
/// token is never sent to one over a connection that is not encrypted.
result<std::unique_ptr<agent::local_stream>>
connect_loopback(const boost::asio::any_io_executor& executor,
                 std::uint16_t port);

/// The port of an address `listen_loopback` reported, or 0.
std::uint16_t loopback_port_of(const std::string& address) noexcept;

} // namespace vss::control
