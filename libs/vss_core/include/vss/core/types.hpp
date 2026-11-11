//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_TYPES_HPP
#define VSS_CORE_TYPES_HPP

#include <cstdint>
#include <string_view>

namespace vss {

/// Stable identifier of an endpoint inside a hub.
using endpoint_id = std::uint64_t;

/// Stable identifier of a link (a set of directional routes) inside a hub.
using link_id = std::uint64_t;

/// Identifier handed out by #hub::subscribe.
using subscriber_id = std::uint64_t;

/// What a port actually is.
enum class endpoint_kind
{
    /// An OS visible virtual port created by this application.
    virtual_port,
    /// A real serial device, e.g. COM3 or /dev/ttyUSB0.
    physical,
    /// A raw TCP byte pipe.
    tcp,
    /// A connected UDP byte pipe.
    udp,
    /// An RFC 2217 (Telnet COM Port Control) client.
    rfc2217,
    /// A scriptable in-memory device used by the demo and the tests.
    mock
};

enum class endpoint_state
{
    created,
    opening,
    open,
    closed,
    failed
};

enum class data_direction
{
    rx,
    tx
};

/// Coarse link shape, used by the GUI and by the configuration file.
enum class link_mode
{
    /// Traffic flows both ways between one source and its targets.
    duplex,
    /// Traffic flows from the sources to the targets only.
    one_way,
    /// A shared multi-drop bus.  Every member hears every other member and
    /// never itself; the members are the link's `sources` and it has no
    /// `targets`.
    bus
};

/// How concurrent publishers into the same target are arbitrated.
enum class merge_policy
{
    /// Every publisher is forwarded; the target sees an interleaving.
    interleave,
    /// Exactly one publisher owns the target until ownership changes.
    owner
};

/// What a half-duplex bus does with bytes that shared the wire with another
/// member's (see #half_duplex_options).
enum class collision_action
{
    /// Deliver the bytes as they were sent, count the collision and say so.
    report,
    /// Deliver every byte of the chunks that collided inverted, count them
    /// and say so, as a receiver on a real line reads rubbish.
    garble
};

/// Why a chunk sent on a half-duplex bus was not a clean transmission.
enum class bus_fault
{
    none,
    /// Another member's bytes were on the wire at the same time.
    overlap,
    /// Another member had stopped sending less than the turn-around time
    /// before.
    too_soon,
    /// Another member held its RTS high, so its transmitter held the line.
    line_held,
    /// The sender's own RTS was low, so its transmitter was off and the
    /// bytes never reached the wire.
    transmitter_off,
    /// The sender is so far ahead of the line rate that the bytes could not
    /// start within `max_bus_backlog_ms`; they were dropped.
    overrun
};

const char* to_string(endpoint_kind value) noexcept;
const char* to_string(endpoint_state value) noexcept;
const char* to_string(link_mode value) noexcept;
const char* to_string(merge_policy value) noexcept;
const char* to_string(data_direction value) noexcept;
const char* to_string(collision_action value) noexcept;
const char* to_string(bus_fault value) noexcept;

} // namespace vss

#endif // VSS_CORE_TYPES_HPP
