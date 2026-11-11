//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Windows: the security identifier of the user in the process token.

#include "support/account_id.hpp"

#include <QtCore/qt_windows.h>

#include <sddl.h>

#include <vector>

namespace vss::gui::support
{

QString account_id()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return {};

    QString out;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<TOKEN_USER> buffer(size / sizeof(TOKEN_USER) + 1);
    if (GetTokenInformation(
            token, TokenUser, buffer.data(),
            static_cast<DWORD>(buffer.size() * sizeof(TOKEN_USER)), &size) != 0)
    {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(buffer.front().User.Sid, &text) != 0)
        {
            out = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

} // namespace vss::gui::support
