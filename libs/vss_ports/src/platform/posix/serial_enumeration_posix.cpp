//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include "../serial_enumeration_platform.hpp"

#include <vss/ports/stable_link.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace vss::detail
{
namespace
{

bool looks_like_serial(const std::string& name)
{
    // ttyGS is a USB gadget, tnt a tty0tty pair and rfcomm a Bluetooth
    // serial port. Qt lists the same three.
    static constexpr const char* prefixes[] = {
        "ttyUSB", "ttyACM", "ttyAMA",  "ttyS",  "ttyXR", "ttyCH",
        "ttymxc", "cu.usb", "tty.usb", "ttyGS", "tnt",   "rfcomm"};
    for (const char* prefix : prefixes)
    {
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

} // namespace

std::optional<std::vector<unsigned>> platform_reserved_com_numbers()
{
    // COM numbers are a Windows idea: nothing to ask here.
    return std::nullopt;
}

std::vector<serial_device_info> platform_enumerate_serial_devices()
{
    VSS_PROFILE_ZONE("ports.enumerate.posix");
    std::vector<serial_device_info> devices;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec))
    {
        if (ec)
            break;
        const std::string name = entry.path().filename().string();
        if (!looks_like_serial(name))
            continue;

        serial_device_info device;
        device.path = entry.path().string();
        device.description = name;
        device.virtual_device = false;
        devices.push_back(std::move(device));
    }

    // The names `vss_cli vport` made: a pseudo terminal is not in /dev, so
    // without these the studio would not list its own ports.
    for (const auto& link : list_stable_links(process_environment()))
    {
        serial_device_info device;
        device.path = link.path;
        device.description =
            std::filesystem::path(link.path).filename().string() +
            " (virtual port)";
        device.virtual_device = true;
        devices.push_back(std::move(device));
    }

    return devices;
}

} // namespace vss::detail
