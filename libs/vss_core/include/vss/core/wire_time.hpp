//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/serial_settings.hpp>
#include <vss/core/stats.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vss
{

/// How a character is framed on the wire: the data bits, the parity bit and
/// the stop bits.  The start bit is always there.  `8N1` is the default.
struct wire_framing
{
    unsigned data_bits = 8;
    parity_mode parity = parity_mode::none;
    stop_bits_mode stop_bits = stop_bits_mode::one;

    friend bool operator==(const wire_framing&, const wire_framing&) = default;
};

/// The framing of `format`, or `8N1` when the format does not know it.
constexpr wire_framing framing_of(const line_format& format) noexcept
{
    if (!format.framing_known)
        return {};
    return {format.data_bits, format.parity, format.stop_bits};
}

/// True when the data bits are 5 to 8.
constexpr bool valid_framing(const wire_framing& framing) noexcept
{
    return framing.data_bits >= 5 && framing.data_bits <= 8;
}

/// Twice the bits one character takes on the wire (start, data, parity and
/// stop), so that 1.5 stop bits stay a whole number: 8N1 is 20, 8E1 is 22,
/// 8N2 is 22 and 5N1.5 is 15.
constexpr std::uint32_t half_bits_per_character(
    const wire_framing& framing) noexcept
{
    std::uint32_t half_bits = 2 + 2 * framing.data_bits;
    if (framing.parity != parity_mode::none)
        half_bits += 2;
    switch (framing.stop_bits)
    {
    case stop_bits_mode::one: half_bits += 2; break;
    case stop_bits_mode::one_point_five: half_bits += 3; break;
    case stop_bits_mode::two: half_bits += 4; break;
    }
    return half_bits;
}

/// How long `bytes` characters take on a wire at `bit_rate` bits per second
/// with `framing`.  Zero when the rate is zero.  A chunk is at most a read
/// buffer, so the arithmetic stays far inside 64 bits.
inline clock_type::duration wire_time(std::uint32_t bit_rate,
                                      const wire_framing& framing,
                                      std::size_t bytes) noexcept
{
    if (bit_rate == 0)
        return {};
    const std::uint64_t half_bits =
        static_cast<std::uint64_t>(bytes) * half_bits_per_character(framing);
    const std::uint64_t nanoseconds =
        half_bits * 500'000'000ULL / bit_rate;
    return std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds(static_cast<std::int64_t>(nanoseconds)));
}

/// The text of a framing, `8N1`, `7E2`, `5N1.5`.
std::string to_text(const wire_framing& framing);

/// Reads the text of a framing: one digit 5 to 8, a parity letter (`N`, `O`,
/// `E`, `M`, `S`, either case) and `1`, `1.5` or `2`.
std::optional<wire_framing> parse_framing(std::string_view text);

} // namespace vss
