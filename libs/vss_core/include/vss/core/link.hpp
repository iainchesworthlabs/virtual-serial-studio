//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_LINK_HPP
#define VSS_CORE_LINK_HPP

#include <vss/core/control_lines.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vss {

/// The most members one bus takes.  A bus of `n` members holds `n * (n - 1)`
/// routes, so the limit keeps a saved file from asking for millions of them.
inline constexpr std::size_t max_bus_members = 64;

/// The longest `delay_ms` a link takes, one minute.  A saved file cannot ask
/// for a delay that holds bytes for hours.
inline constexpr std::uint32_t max_link_delay_ms = 60'000;

/// The slowest and the fastest line rate a half-duplex bus takes, in bits
/// per second.
inline constexpr std::uint32_t min_bus_bit_rate = 50;
inline constexpr std::uint32_t max_bus_bit_rate = 4'000'000;

/// The bits one byte takes on the wire of a half-duplex bus: a start bit,
/// eight data bits and a stop bit.
inline constexpr std::uint32_t bus_bits_per_byte = 10;

/// How far ahead of the line rate a member of a half-duplex bus may get.  A
/// chunk that could not start within this long of being read is dropped and
/// reported (`bus_fault::overrun`), so a program that writes faster than the
/// line for ever cannot fill the memory.
inline constexpr std::uint32_t max_bus_backlog_ms = 60'000;

/// Makes a bus behave like a two-wire RS-485 line: one member sends at a
/// time.  For #hub::bus only; every other link refuses it.
///
/// The line has a rate.  A chunk of `n` bytes occupies the wire for
/// `n * bus_bits_per_byte / bit_rate` seconds from the moment the hub reads
/// it (or from the end of the same member's chunk before it), and the other
/// members hear it when its last byte has left the wire, not before.  A chunk
/// from another member that starts inside that time has collided with it,
/// and one that starts less than `turnaround_ms` after it has come too soon.
/// Either is counted in `link_stats::collisions` and published as an
/// `event_kind::bus_fault`.
struct half_duplex_options
{
    /// Off (the default) leaves the bus exactly as it is without the option.
    bool enabled = false;

    /// Bits per second, #min_bus_bit_rate to #max_bus_bit_rate.
    std::uint32_t bit_rate = 9600;

    /// How long the line stays taken after a member's last byte, while its
    /// transmitter lets go, in milliseconds (at most #max_link_delay_ms).
    /// Another member that starts inside it has answered too soon.  It
    /// delays nothing: `link_options::delay_ms` is the delay.
    std::uint32_t turnaround_ms = 0;

    /// What the receivers get of the chunks that collided.
    collision_action on_collision = collision_action::report;

    /// A member whose transport reports the other side's RTS
    /// (#transport::reports_peer_rts) sends only while that RTS is high, and
    /// holds the line for as long as it is.  Its bytes sent with RTS low are
    /// dropped, counted and reported.  A member whose transport does not
    /// report it is taken to switch its transmitter by itself: it always
    /// sends and never holds the line.
    bool rts_gating = false;

    friend bool operator==(const half_duplex_options&,
                           const half_duplex_options&) = default;
};

/// Tuning for one link.
struct link_options
{
    /// Cross DTR→DSR and RTS→CTS between the two ends, as a null-modem
    /// cable does.  Disable for a one-way tap that must not disturb the
    /// device.  A bus has no modem lines (a multi-drop wire pair carries
    /// none), so #hub::bus stores `false` here whatever the caller passed.
    bool emulate_modem_lines = true;

    /// Which control line drives which across the link, when
    /// `emulate_modem_lines` is on.  The default is the null-modem cable.  It
    /// lives in the running hub only: the saved topology does not store it.
    modem_wiring wiring;

    /// Arbitration between concurrent publishers into the same target.  A
    /// bus supports `interleave` only.
    merge_policy policy = merge_policy::interleave;

    /// How long each chunk is held between its source and its target, in
    /// milliseconds, to rehearse a slow wire or a network hop.  Zero (the
    /// default) forwards at once, as before.  It applies to every route of
    /// the link, so on a bus each receiving member hears a byte this long
    /// after it was sent.
    std::uint32_t delay_ms = 0;

    /// Each chunk's delay varies by up to this much either side of
    /// `delay_ms`, drawn uniformly.  It must not exceed `delay_ms`, so a delay
    /// is never negative.  Bytes of one route stay in order whatever the
    /// draw: a chunk never overtakes the one before it.
    std::uint32_t jitter_ms = 0;

    /// The half-duplex line of a bus.  Off by default, and
    /// `errc::unsupported` on any link but a bus.
    half_duplex_options half_duplex;

    bool has_latency() const noexcept
    {
        return delay_ms != 0 || jitter_ms != 0;
    }
};

/// True when the link's delay and jitter are within the limits.
inline constexpr bool valid_latency(const link_options& options) noexcept
{
    return options.delay_ms <= max_link_delay_ms &&
           options.jitter_ms <= options.delay_ms;
}

/// True when the half-duplex option is off or its values are within the
/// limits.
inline constexpr bool
valid_half_duplex(const half_duplex_options& options) noexcept
{
    return !options.enabled ||
           (options.bit_rate >= min_bus_bit_rate &&
            options.bit_rate <= max_bus_bit_rate &&
            options.turnaround_ms <= max_link_delay_ms);
}

/// Snapshot of a link for the GUI and the CLI.
struct link_info
{
    link_id id = 0;
    link_mode mode = link_mode::duplex;
    link_options options;
    /// For a bus: its members, and `targets` is empty.
    std::vector<endpoint_id> sources;
    std::vector<endpoint_id> targets;
    endpoint_id owner = 0;
    link_stats stats;
};

} // namespace vss

#endif // VSS_CORE_LINK_HPP
