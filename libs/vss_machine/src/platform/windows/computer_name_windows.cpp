//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This computer's name on Windows: the DNS host name, which is what Qt's
// `QSysInfo::machineHostName` gave.
//

#include "platform/machine_platform.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
// clang-format on

#include <filesystem>
#include <string>

namespace vss::machine::platform
{

std::string full_computer_name()
{
    // The first call tells how many characters are needed, with the end of
    // the text; a name that grows between the two calls is tried again.
    std::wstring name(64, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        auto size = static_cast<DWORD>(name.size());
        if (::GetComputerNameExW(ComputerNameDnsHostname, name.data(), &size) !=
            0)
        {
            // On success `size` is the length without the end of the text.
            name.resize(size);
            const std::u8string text = std::filesystem::path(name).u8string();
            std::string out;
            out.reserve(text.size());
            for (const char8_t unit : text)
                out += static_cast<char>(unit);
            return out;
        }
        if (::GetLastError() != ERROR_MORE_DATA)
            return {};
        name.assign(size, L'\0');
    }
    return {};
}

} // namespace vss::machine::platform
