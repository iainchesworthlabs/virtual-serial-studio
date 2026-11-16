//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/update/version.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace vss::update
{

namespace
{

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool is_identifier_char(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-';
}

bool all_digits(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, is_digit);
}

/// A number written without a leading zero and no larger than the limit.
std::optional<std::uint32_t> number_from(std::string_view text) noexcept
{
    if (!all_digits(text) || (text.size() > 1 && text.front() == '0'))
        return std::nullopt;
    // Six digits at most, so the value cannot overflow.
    if (text.size() > 6)
        return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : text)
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    if (value > max_version_number)
        return std::nullopt;
    return value;
}

bool valid_identifier(std::string_view text) noexcept
{
    if (text.empty() || !std::ranges::all_of(text, is_identifier_char))
        return false;
    // A numeric identifier has no leading zero.
    return !(all_digits(text) && text.size() > 1 && text.front() == '0');
}

/// One identifier against another, as the specification orders them.
std::strong_ordering compare_identifier(std::string_view left,
                                        std::string_view right) noexcept
{
    const bool left_number = all_digits(left);
    const bool right_number = all_digits(right);
    if (left_number != right_number)
    {
        // A number is lower than a word.
        return left_number ? std::strong_ordering::less
                           : std::strong_ordering::greater;
    }
    if (left_number)
    {
        // Neither has a leading zero, so the longer number is the larger one
        // however long the digits are.
        if (const auto order = left.size() <=> right.size(); order != 0)
            return order;
    }
    return left.compare(right) <=> 0;
}

} // namespace

std::optional<version> parse_version(std::string_view text)
{
    if (text.empty() || text.size() > max_version_length)
        return std::nullopt;

    const std::size_t hyphen = text.find('-');
    const std::string_view numbers = text.substr(0, hyphen);

    version out;
    const std::array<std::uint32_t*, 3> parts{&out.major, &out.minor,
                                              &out.patch};
    std::string_view rest = numbers;
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::size_t dot = rest.find('.');
        // The last number has no dot after it, and the first two must.
        if ((dot == std::string_view::npos) != (i == 2))
            return std::nullopt;
        const auto value = number_from(rest.substr(0, dot));
        if (!value)
            return std::nullopt;
        *parts[i] = *value;
        if (i < 2)
            rest.remove_prefix(dot + 1);
    }

    if (hyphen == std::string_view::npos)
        return out;

    std::string_view identifiers = text.substr(hyphen + 1);
    while (true)
    {
        const std::size_t dot = identifiers.find('.');
        const std::string_view identifier = identifiers.substr(0, dot);
        if (!valid_identifier(identifier) ||
            out.prerelease.size() >= max_prerelease_identifiers)
            return std::nullopt;
        out.prerelease.emplace_back(identifier);
        if (dot == std::string_view::npos)
            break;
        identifiers.remove_prefix(dot + 1);
    }
    return out;
}

std::string to_string(const version& value)
{
    std::string out = std::to_string(value.major);
    out += '.';
    out += std::to_string(value.minor);
    out += '.';
    out += std::to_string(value.patch);
    for (std::size_t i = 0; i < value.prerelease.size(); ++i)
    {
        out += i == 0 ? '-' : '.';
        out += value.prerelease[i];
    }
    return out;
}

std::strong_ordering compare(const version& left, const version& right) noexcept
{
    if (const auto order = left.major <=> right.major; order != 0)
        return order;
    if (const auto order = left.minor <=> right.minor; order != 0)
        return order;
    if (const auto order = left.patch <=> right.patch; order != 0)
        return order;

    // A pre-release is lower than the final release it leads up to.
    if (left.prerelease.empty() || right.prerelease.empty())
        return left.prerelease.empty() <=> right.prerelease.empty();

    const std::size_t shared =
        std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t i = 0; i < shared; ++i)
    {
        if (const auto order =
                compare_identifier(left.prerelease[i], right.prerelease[i]);
            order != 0)
            return order;
    }
    // Every shared identifier is equal: the longer list is the higher one.
    return left.prerelease.size() <=> right.prerelease.size();
}

bool is_developer_build(const version& value) noexcept
{
    return value.prerelease.size() == 1 && value.prerelease.front() == "dev";
}

} // namespace vss::update
