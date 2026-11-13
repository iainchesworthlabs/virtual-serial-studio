//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/rules/random.hpp>

#include <algorithm>
#include <bit>

namespace vss::rules {

namespace {

constexpr std::uint64_t pcg_multiplier = 6364136223846793005ULL;

/// FNV-1a over the bytes of a name, continued from `hash`.
std::uint64_t fnv1a(std::uint64_t hash, std::string_view text) noexcept
{
    for (const char c : text)
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    // A separator, so ("ab", "c") and ("a", "bc") differ.
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
    return hash;
}

std::int64_t clamp_us(std::int64_t value, const delay_spec& spec) noexcept
{
    return std::clamp(value, spec.min.count(), spec.max.count());
}

} // namespace

std::uint64_t splitmix64(std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::uint64_t derive_stream(std::uint64_t seed, std::string_view set_name,
                            std::string_view rule_name) noexcept
{
    std::uint64_t names = 14695981039346656037ULL;
    names = fnv1a(names, set_name);
    names = fnv1a(names, rule_name);
    std::uint64_t state = seed ^ names;
    // Two steps, so that seeds that differ in one bit give streams that
    // differ in about half of theirs.
    splitmix64(state);
    return splitmix64(state);
}

generator::generator(std::uint64_t stream_seed) noexcept
{
    std::uint64_t mixer = stream_seed;
    const std::uint64_t initial = splitmix64(mixer);
    // The increment of a PCG stream must be odd.
    increment_ = (splitmix64(mixer) << 1U) | 1U;
    state_ = 0;
    next();
    state_ += initial;
    next();
    drawn_ = 0;
}

std::uint32_t generator::next() noexcept
{
    const std::uint64_t old = state_;
    state_ = old * pcg_multiplier + increment_;
    ++drawn_;
    const auto shifted =
        static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
    const int rotation = static_cast<int>(old >> 59U);
    return std::rotr(shifted, rotation);
}

std::uint64_t generator::next64() noexcept
{
    const std::uint64_t high = next();
    return (high << 32U) | next();
}

std::uint64_t generator::between(std::uint64_t low,
                                 std::uint64_t high) noexcept
{
    const std::uint64_t span = high - low;
    if (span == 0)
        return low;
    if (span == ~std::uint64_t{0})
        return next64();
    // Rejection: draws at or above `limit` would make the low values of the
    // remainder more likely, so they are drawn again.  `limit` is the
    // largest multiple of `count` that fits, so fewer than half are.
    const std::uint64_t count = span + 1;
    const std::uint64_t limit = ~std::uint64_t{0} - (~std::uint64_t{0} % count);
    for (;;)
    {
        const std::uint64_t value = next64();
        if (value < limit)
            return low + value % count;
    }
}

std::uint32_t log2_q24(std::uint32_t value) noexcept
{
    const auto whole = static_cast<std::uint32_t>(std::bit_width(value) - 1);
    // The value scaled into [1, 2) with 31 fractional bits.
    std::uint64_t x = static_cast<std::uint64_t>(value) << (31U - whole);
    std::uint32_t fraction = 0;
    for (int bit = 0; bit < 24; ++bit)
    {
        x = (x * x) >> 31U;
        fraction <<= 1U;
        if (x >= (std::uint64_t{1} << 32U))
        {
            x >>= 1U;
            fraction |= 1U;
        }
    }
    return (whole << 24U) | fraction;
}

microseconds draw_delay(const delay_spec& spec, generator& source,
                        std::uint64_t position) noexcept
{
    switch (spec.kind)
    {
    case delay_kind::fixed:
        return spec.fixed;
    case delay_kind::uniform:
        return microseconds(static_cast<std::int64_t>(
            source.between(static_cast<std::uint64_t>(spec.min.count()),
                           static_cast<std::uint64_t>(spec.max.count()))));
    case delay_kind::normal:
    {
        // Twelve uniform draws have a variance of one (in units of 2^32)
        // and a mean of six.
        std::int64_t sum = 0;
        for (int i = 0; i < 12; ++i)
            sum += static_cast<std::int64_t>(source.next());
        const std::int64_t centred = sum - (std::int64_t{6} << 32U);
        // |centred| is under 2^35 and the deviation under 2^26, so the
        // product fits.  Dividing rounds towards zero on both sides.
        const std::int64_t offset =
            centred * spec.sd.count() / (std::int64_t{1} << 32U);
        return microseconds(clamp_us(spec.mean.count() + offset, spec));
    }
    case delay_kind::exponential:
    {
        // u = (r + 1) / 2^32 is in (0, 1], so -ln(u) = (32 - log2(r + 1)) *
        // ln 2 is zero or more.  r + 1 would not fit for the largest r, and
        // that one draw in 2^32 is taken as u = 1.
        const std::uint32_t r = source.next();
        const std::uint64_t log =
            r == ~std::uint32_t{0} ? (std::uint64_t{32} << 24U)
                                   : log2_q24(r + 1U);
        const std::uint64_t bits = (std::uint64_t{32} << 24U) - log;
        // ln 2 with 32 fractional bits.
        const std::uint64_t natural = (bits * 2977044472ULL) >> 32U;
        const auto mean = static_cast<std::uint64_t>(spec.mean.count());
        const auto value = static_cast<std::int64_t>((mean * natural) >> 24U);
        return microseconds(clamp_us(value, spec));
    }
    case delay_kind::table:
    {
        if (spec.values.empty())
            return microseconds(0);
        const std::uint64_t size = spec.values.size();
        const std::uint64_t index =
            spec.in_order ? position % size : source.between(0, size - 1);
        return spec.values[static_cast<std::size_t>(index)];
    }
    }
    return microseconds(0);
}

} // namespace vss::rules
