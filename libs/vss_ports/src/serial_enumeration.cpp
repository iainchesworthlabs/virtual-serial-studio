//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/ports/serial_enumeration.hpp>

#include "platform/serial_enumeration_platform.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace vss
{
namespace detail
{

std::string extract_com_name(std::string_view text)
{
    const auto open = text.rfind('(');
    const auto close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close < open)
        return {};

    std::string candidate(text.substr(open + 1, close - open - 1));
    if (candidate.size() < 4)
        return {};
    if (candidate.compare(0, 3, "COM") != 0 &&
        candidate.compare(0, 3, "com") != 0)
        return {};
    for (std::size_t index = 3; index < candidate.size(); ++index)
    {
        if (!std::isdigit(static_cast<unsigned char>(candidate[index])))
            return {};
    }
    return candidate;
}

bool parse_com0com_instance(std::string_view instance, int& pair,
                            std::string& side)
{
    const auto marker = instance.rfind("CNC");
    if (marker == std::string_view::npos || marker + 4 > instance.size())
        return false;

    const char which = instance[marker + 3];
    if (which != 'A' && which != 'B' && which != 'a' && which != 'b')
        return false;

    side.assign(
        1, static_cast<char>(std::toupper(static_cast<unsigned char>(which))));

    int value = 0;
    bool any = false;
    for (std::size_t index = marker + 4; index < instance.size(); ++index)
    {
        const char digit = instance[index];
        if (!std::isdigit(static_cast<unsigned char>(digit)))
        {
            if (any)
                break;
            return false;
        }
        any = true;
        value = value * 10 + (digit - '0');
    }

    if (!any)
        return false;
    pair = value;
    return true;
}

} // namespace detail

std::string parse_com_port_name(std::string_view description)
{
    return detail::extract_com_name(description);
}

std::optional<com0com_end>
parse_com0com_instance_id(std::string_view instance_id)
{
    // Instance ids are upper case in the driver, but PnP consumers on other
    // paths have been seen lower case; normalise before matching.
    std::string normalised(instance_id);
    std::transform(normalised.begin(), normalised.end(), normalised.begin(),
                   [](unsigned char value)
                   { return static_cast<char>(std::toupper(value)); });

    com0com_end parsed;
    if (!detail::parse_com0com_instance(normalised, parsed.pair, parsed.side))
        return std::nullopt;
    return parsed;
}

std::vector<serial_device_info>
normalise(std::vector<serial_device_info> devices)
{
    VSS_PROFILE_ZONE("ports.enumerate.normalise");
    std::vector<serial_device_info> out;
    out.reserve(devices.size());

    for (auto& device : devices)
    {
        if (device.path.empty())
            continue;
        const bool duplicate =
            std::any_of(out.begin(), out.end(),
                        [&device](const serial_device_info& existing)
                        { return existing.path == device.path; });
        if (!duplicate)
            out.push_back(std::move(device));
    }

    std::sort(out.begin(), out.end(),
              [](const serial_device_info& lhs, const serial_device_info& rhs)
              { return lhs.path < rhs.path; });
    return out;
}

std::vector<unsigned>
com_numbers_in_bitmap(const std::vector<unsigned char>& bitmap)
{
    std::vector<unsigned> numbers;
    for (std::size_t byte = 0; byte < bitmap.size() && byte < 32; ++byte)
    {
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            const unsigned number = static_cast<unsigned>(byte) * 8 + bit + 1;
            const unsigned mask = static_cast<unsigned>(bitmap[byte]) >> bit;
            if (number <= 255 && (mask & 1u) != 0)
                numbers.push_back(number);
        }
    }
    return numbers;
}

std::optional<std::vector<unsigned>> reserved_com_numbers()
{
    VSS_PROFILE_ZONE("ports.reserved_com_numbers");
    auto numbers = detail::platform_reserved_com_numbers();
    if (!numbers)
        return std::nullopt;
    std::sort(numbers->begin(), numbers->end());
    numbers->erase(std::unique(numbers->begin(), numbers->end()),
                   numbers->end());
    return numbers;
}

bool is_studio_port_instance_id(std::string_view instance_id)
{
    const auto starts_with = [&](std::string_view prefix)
    {
        if (instance_id.size() < prefix.size())
            return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
        {
            char c = instance_id[i];
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            if (c != prefix[i])
                return false;
        }
        return true;
    };
    return starts_with("SWD\\VSSCOMUM\\") || starts_with("VSSCOM\\PORT\\");
}

std::vector<com_number_holder>
classify_com_numbers(const std::vector<unsigned>& database,
                     const std::vector<unsigned>& resolving,
                     const std::vector<com_port_entry>& entries)
{
    std::vector<unsigned> numbers;
    numbers.insert(numbers.end(), database.begin(), database.end());
    numbers.insert(numbers.end(), resolving.begin(), resolving.end());
    for (const auto& entry : entries)
        numbers.push_back(entry.number);
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());

    std::vector<com_number_holder> out;
    for (const unsigned number : numbers)
    {
        if (number == 0)
            continue;
        com_number_holder holder;
        holder.number = number;

        const com_port_entry* present = nullptr;
        const com_port_entry* other_gone = nullptr;
        const com_port_entry* studio_gone = nullptr;
        for (const auto& entry : entries)
        {
            if (entry.number != number)
                continue;
            if (entry.present)
                present = present ? present : &entry;
            else if (is_studio_port_instance_id(entry.instance_id))
                studio_gone = studio_gone ? studio_gone : &entry;
            else
                other_gone = other_gone ? other_gone : &entry;
        }

        const bool resolves =
            std::find(resolving.begin(), resolving.end(), number) !=
            resolving.end();
        const com_port_entry* shown = nullptr;
        if (present)
        {
            holder.kind = com_holder_kind::present_device;
            shown = present;
        }
        else if (resolves)
            holder.kind = com_holder_kind::name_only;
        else if (other_gone)
        {
            holder.kind = com_holder_kind::other_port_gone;
            shown = other_gone;
        }
        else if (studio_gone)
        {
            holder.kind = com_holder_kind::studio_port_gone;
            shown = studio_gone;
        }
        else
            holder.kind = com_holder_kind::database_only;

        if (shown)
        {
            holder.instance_id = shown->instance_id;
            holder.description = shown->description;
        }
        out.push_back(std::move(holder));
    }
    return out;
}

std::optional<std::vector<com_number_holder>> com_number_holders()
{
    VSS_PROFILE_ZONE("ports.com_number_holders");
    const auto sources = detail::platform_com_sources();
    if (!sources)
        return std::nullopt;
    return classify_com_numbers(sources->database, sources->resolving,
                                sources->entries);
}

std::vector<serial_device_info> enumerate_serial_devices()
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.enumerate");
    auto devices = normalise(detail::platform_enumerate_serial_devices());
    VSS_PROFILE_ZONE_VALUE(zone, devices.size());
    VSS_PROFILE_PLOT("ports.enumerate.devices", devices.size());
    return devices;
}

std::vector<std::string> enumerate_serial_port_names()
{
    std::vector<std::string> names;
    for (const auto& device : enumerate_serial_devices())
        names.push_back(device.path);
    return names;
}

} // namespace vss
