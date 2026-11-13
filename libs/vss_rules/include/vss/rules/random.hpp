//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seeded source of a rule set.  Everything here is whole-number
// arithmetic of fixed width, so a seed gives the same draws on every
// platform, compiler and standard library.  The distributions of <random> do
// not, which is why none of them is used.
//

#pragma once

#include <vss/rules/model.hpp>

#include <cstdint>
#include <string_view>

namespace vss::rules {

/// One step of SplitMix64: advances `state` and returns the next value.
std::uint64_t splitmix64(std::uint64_t& state) noexcept;

/// The seed of one rule's own stream, from the set's seed and the two names.
/// A rule's draws therefore do not move when another rule is added, removed
/// or reordered.
std::uint64_t derive_stream(std::uint64_t seed, std::string_view set_name,
                            std::string_view rule_name) noexcept;

/// PCG32 (XSH-RR, 64 bits of state).
class generator
{
public:
    explicit generator(std::uint64_t stream_seed = 0) noexcept;

    std::uint32_t next() noexcept;
    std::uint64_t next64() noexcept;

    /// A whole number from `low` to `high`, both included, every one as
    /// likely.  `low` must not be above `high`.
    std::uint64_t between(std::uint64_t low, std::uint64_t high) noexcept;

    /// How many 32-bit values have been drawn, for the log.
    std::uint64_t drawn() const noexcept { return drawn_; }

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_ = 1;
    std::uint64_t drawn_ = 0;
};

/// The delay one action gets.  `position` is how many times this delay has
/// been drawn before, which a table taken in order uses and nothing else
/// does.  The result is always within the bounds of the kind.
///
/// - `uniform`: every microsecond from `min` to `max` as likely.
/// - `normal`: the sum of twelve uniform draws (Irwin-Hall), which has the
///   mean and the deviation asked for and reaches six deviations either
///   side, then cut at `min` and `max`.
/// - `exponential`: `-mean * ln(u)` with a fixed-point logarithm, cut at
///   `min` and `max`.
/// - `table`: `values[position % size]` in order, or one picked uniformly.
microseconds draw_delay(const delay_spec& spec, generator& source,
                        std::uint64_t position) noexcept;

/// `log2(value)` as a fixed-point number with 24 fractional bits, rounded
/// down.  `value` must not be zero.  Exposed for its tests.
std::uint32_t log2_q24(std::uint32_t value) noexcept;

} // namespace vss::rules
