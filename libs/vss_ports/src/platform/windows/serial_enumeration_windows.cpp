//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/trace/recorder.hpp>

#include "../serial_enumeration_platform.hpp"

#include <windows.h>

#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>

#include <string>
#include <utility>

namespace vss::detail
{
namespace
{

std::string to_utf8(const wchar_t* text)
{
    if (!text || !*text)
        return {};

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr,
                          nullptr);
    return out;
}

std::string read_string_property(HDEVINFO info, SP_DEVINFO_DATA& data,
                                 DWORD property)
{
    DWORD type = 0;
    DWORD required = 0;
    ::SetupDiGetDeviceRegistryPropertyW(info, &data, property, &type, nullptr,
                                        0, &required);
    if (required == 0)
        return {};

    std::wstring buffer(required / sizeof(wchar_t) + 1, L'\0');
    if (!::SetupDiGetDeviceRegistryPropertyW(
            info, &data, property, &type,
            reinterpret_cast<PBYTE>(buffer.data()), required, nullptr))
        return {};

    buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
    return to_utf8(buffer.c_str());
}

std::string read_instance_id(HDEVINFO info, SP_DEVINFO_DATA& data)
{
    DWORD required = 0;
    ::SetupDiGetDeviceInstanceIdW(info, &data, nullptr, 0, &required);
    if (required == 0)
        return {};
    std::wstring buffer(required + 1, L'\0');
    if (!::SetupDiGetDeviceInstanceIdW(info, &data, buffer.data(), required,
                                       nullptr))
        return {};
    buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
    return to_utf8(buffer.c_str());
}

/// The `ComDB` bitmap of the COM name arbiter, readable by every user.
std::optional<std::vector<unsigned char>> read_com_db()
{
    DWORD size = 0;
    constexpr const wchar_t* key =
        L"SYSTEM\\CurrentControlSet\\Control\\COM Name Arbiter";
    const LONG sized =
        ::RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ComDB", RRF_RT_REG_BINARY,
                       nullptr, nullptr, &size);
    if (sized != ERROR_SUCCESS || size == 0)
    {
        trace::record_failure(
            "RegGetValueW(ComDB)", 0, "COM Name Arbiter",
            boost::system::error_code(static_cast<int>(sized),
                                      boost::system::system_category()));
        return std::nullopt;
    }
    std::vector<unsigned char> bitmap(size);
    const LONG read =
        ::RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ComDB", RRF_RT_REG_BINARY,
                       nullptr, bitmap.data(), &size);
    if (read != ERROR_SUCCESS)
    {
        trace::record_failure(
            "RegGetValueW(ComDB)", 0, "COM Name Arbiter",
            boost::system::error_code(static_cast<int>(read),
                                      boost::system::system_category()));
        return std::nullopt;
    }
    bitmap.resize(size);
    return bitmap;
}

} // namespace

std::optional<std::vector<unsigned>> platform_reserved_com_numbers()
{
    VSS_PROFILE_ZONE("ports.reserved_com_numbers.windows");
    const auto bitmap = read_com_db();
    if (!bitmap)
        return std::nullopt;
    std::vector<unsigned> numbers = com_numbers_in_bitmap(*bitmap);

    // A name can resolve with no entry in the database (com0com, a port that
    // went but kept its name); the helper that makes ports refuses these too.
    for (unsigned number = 1; number <= 255; ++number)
    {
        const std::wstring name = L"COM" + std::to_wstring(number);
        wchar_t target[512];
        if (::QueryDosDeviceW(name.c_str(), target, 512) != 0)
            numbers.push_back(number);
    }
    return numbers;
}

std::optional<com_sources> platform_com_sources()
{
    VSS_PROFILE_ZONE("ports.com_sources.windows");
    const auto bitmap = read_com_db();
    if (!bitmap)
        return std::nullopt;

    com_sources sources;
    sources.database = com_numbers_in_bitmap(*bitmap);
    for (unsigned number = 1; number <= 255; ++number)
    {
        const std::wstring name = L"COM" + std::to_wstring(number);
        wchar_t target[512];
        if (::QueryDosDeviceW(name.c_str(), target, 512) != 0)
            sources.resolving.push_back(number);
    }

    // Without DIGCF_PRESENT: a port that is gone stays in the class with its
    // PortName.
    HDEVINFO info = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr,
                                           nullptr, 0);
    if (info == INVALID_HANDLE_VALUE)
        return sources;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; ::SetupDiEnumDeviceInfo(info, index, &data); ++index)
    {
        const HKEY key = ::SetupDiOpenDevRegKey(info, &data, DICS_FLAG_GLOBAL,
                                                0, DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        wchar_t name[64]{};
        DWORD size = sizeof(name) - sizeof(wchar_t);
        DWORD type = 0;
        const bool read =
            ::RegQueryValueExW(key, L"PortName", nullptr, &type,
                               reinterpret_cast<LPBYTE>(name), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ;
        ::RegCloseKey(key);
        if (!read)
            continue;
        const std::string port = to_utf8(name);
        if (port.size() < 4 || port.size() > 6 ||
            (port.rfind("COM", 0) != 0 && port.rfind("com", 0) != 0))
            continue;
        unsigned number = 0;
        bool digits = true;
        for (std::size_t i = 3; i < port.size(); ++i)
        {
            if (port[i] < '0' || port[i] > '9')
                digits = false;
            else
                number = number * 10 + static_cast<unsigned>(port[i] - '0');
        }
        if (!digits || number < 1 || number > 255)
            continue;

        com_port_entry entry;
        entry.number = number;
        entry.instance_id = read_instance_id(info, data);
        entry.description = read_string_property(info, data, SPDRP_FRIENDLYNAME);
        DEVINST node = 0;
        std::wstring wide_id(entry.instance_id.begin(), entry.instance_id.end());
        entry.present = ::CM_Locate_DevNodeW(&node, wide_id.data(),
                                             CM_LOCATE_DEVNODE_NORMAL) ==
                        CR_SUCCESS;
        sources.entries.push_back(std::move(entry));
    }
    ::SetupDiDestroyDeviceInfoList(info);
    return sources;
}

std::vector<serial_device_info> platform_enumerate_serial_devices()
{
    VSS_PROFILE_ZONE("ports.enumerate.windows");
    std::vector<serial_device_info> devices;

    HDEVINFO info = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr,
                                           nullptr, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE)
        return devices;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);

    for (DWORD index = 0; ::SetupDiEnumDeviceInfo(info, index, &data); ++index)
    {
        const std::string friendly =
            read_string_property(info, data, SPDRP_FRIENDLYNAME);
        const std::string hardware =
            read_string_property(info, data, SPDRP_HARDWAREID);
        const std::string instance = read_instance_id(info, data);

        serial_device_info device;
        device.path = extract_com_name(friendly);
        device.description = friendly;
        device.hardware_id = hardware;

        if (device.path.empty())
            continue;

        if (instance.rfind("COM0COM", 0) == 0 ||
            hardware.find("com0com") != std::string::npos ||
            hardware.find("COM0COM") != std::string::npos)
        {
            device.com0com = true;
            device.virtual_device = true;
            parse_com0com_instance(instance, device.com0com_pair,
                                   device.com0com_side);
        }

        devices.push_back(std::move(device));
    }

    ::SetupDiDestroyDeviceInfoList(info);
    return devices;
}

} // namespace vss::detail
