//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The name of this computer on Windows: the DNS host name, as UTF-8.
//

#include <vss/license/store.hpp>

#include <windows.h>

#include <string>

namespace vss::license
{

std::string computer_name()
{
    DWORD length = 0;
    // The first call, with no room, fails and says how much room is needed.
    ::GetComputerNameExW(ComputerNameDnsHostname, nullptr, &length);
    if (length == 0)
        return {};
    std::wstring wide(length, L'\0');
    if (!::GetComputerNameExW(ComputerNameDnsHostname, wide.data(), &length))
        return {};
    wide.resize(length);
    if (wide.empty())
        return {};

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string name(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                              static_cast<int>(wide.size()), name.data(), size,
                              nullptr, nullptr) != size)
        return {};
    return name;
}

} // namespace vss::license
