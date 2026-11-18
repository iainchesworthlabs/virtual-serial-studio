//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/driver_package.hpp>

#include "platform/vcom_driver_platform.hpp"

#include <vss/core/profile.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace vss
{
namespace
{

/// Days from 1970-01-01 to a civil date (proleptic Gregorian).
std::int64_t days_from_civil(std::int64_t year, unsigned month,
                             unsigned day) noexcept
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month,
                     unsigned& day) noexcept
{
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
                                  day_of_era / 36524 - day_of_era / 146096) /
                                 365;
    year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * shifted + 2) / 5 + 1;
    month = shifted < 10 ? shifted + 3 : shifted - 9;
    year += month <= 2 ? 1 : 0;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool read_number(std::string_view& text, unsigned& out, unsigned limit)
{
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data() || value > limit)
        return false;
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    out = value;
    return true;
}

bool skip(std::string_view& text, char separator)
{
    if (text.empty() || text.front() != separator)
        return false;
    text.remove_prefix(1);
    return true;
}

} // namespace

std::string driver_parts_text(const driver_package_version& version)
{
    std::string text;
    for (std::size_t i = 0; i < version.parts.size(); ++i)
    {
        if (i != 0)
            text += '.';
        text += std::to_string(version.parts[i]);
    }
    return text;
}

std::string driver_date_text(const driver_package_version& version)
{
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(version.day, year, month, day);
    std::string text = std::to_string(year);
    text += month < 10 ? "-0" : "-";
    text += std::to_string(month);
    text += day < 10 ? "-0" : "-";
    text += std::to_string(day);
    return text;
}

std::string describe_driver_package(const driver_package_version& version)
{
    return driver_parts_text(version) + " (" + driver_date_text(version) + ")";
}

std::optional<driver_package_version> parse_driver_ver(std::string_view text)
{
    text = trim(text);
    unsigned month = 0;
    unsigned day = 0;
    unsigned year = 0;
    if (!read_number(text, month, 12) || !skip(text, '/') ||
        !read_number(text, day, 31) || !skip(text, '/') ||
        !read_number(text, year, 9999) || month == 0 || day == 0)
        return std::nullopt;
    text = trim(text);
    if (!skip(text, ','))
        return std::nullopt;
    text = trim(text);
    driver_package_version out;
    out.day = days_from_civil(year, month, day);
    for (std::size_t i = 0; i < out.parts.size(); ++i)
    {
        if (i != 0 && !skip(text, '.'))
            return std::nullopt;
        if (!read_number(text, out.parts[i], 65535))
            return std::nullopt;
    }
    if (!trim(text).empty())
        return std::nullopt;
    return out;
}

std::optional<driver_package_version> parse_inf_driver_ver(std::string_view inf)
{
    constexpr std::string_view key = "DriverVer";
    std::size_t at = 0;
    while (at < inf.size())
    {
        const std::size_t end = std::min(inf.find('\n', at), inf.size());
        std::string_view line = trim(inf.substr(at, end - at));
        at = end + 1;
        if (line.substr(0, key.size()) != key)
            continue;
        line.remove_prefix(key.size());
        line = trim(line);
        if (!skip(line, '='))
            continue;
        return parse_driver_ver(line);
    }
    return std::nullopt;
}

std::optional<driver_package_version>
read_inf_driver_version(const std::filesystem::path& inf)
{
    std::ifstream file(inf, std::ios::binary);
    if (!file)
        return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return parse_inf_driver_ver(text);
}

driver_package_order
compare_driver_packages(const driver_package_version& have,
                        const driver_package_version& wanted) noexcept
{
    if (have.day != wanted.day)
        return have.day < wanted.day ? driver_package_order::older
                                     : driver_package_order::newer;
    if (have.parts != wanted.parts)
        return have.parts < wanted.parts ? driver_package_order::older
                                         : driver_package_order::newer;
    return driver_package_order::same;
}

std::optional<driver_package_version>
driver_package_from_store_value(const unsigned char* bytes, std::size_t size)
{
    // What Windows keeps for a package: 8 bytes of its own, the class GUID
    // (16 bytes), the DriverVer date as a FILETIME and the version as one
    // 64-bit number with the first part in the highest 16 bits.
    constexpr std::size_t date_at = 24;
    constexpr std::size_t version_at = 32;
    if (bytes == nullptr || size < version_at + sizeof(std::uint64_t))
        return std::nullopt;
    std::uint64_t filetime = 0;
    std::uint64_t packed = 0;
    std::memcpy(&filetime, bytes + date_at, sizeof(filetime));
    std::memcpy(&packed, bytes + version_at, sizeof(packed));
    constexpr std::uint64_t ticks_per_day = 864000000000ULL;
    constexpr std::int64_t days_1601_to_1970 = 134774;
    driver_package_version out;
    out.day =
        static_cast<std::int64_t>(filetime / ticks_per_day) - days_1601_to_1970;
    for (std::size_t i = 0; i < out.parts.size(); ++i)
        out.parts[i] =
            static_cast<unsigned>((packed >> (48 - 16 * i)) & 0xFFFF);
    return out;
}

std::optional<driver_package_version> installed_driver_package()
{
    VSS_PROFILE_ZONE("ports.driver_package.installed");
    return detail::platform_installed_driver_package();
}

} // namespace vss
