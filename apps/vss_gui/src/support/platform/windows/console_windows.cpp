//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Windows: attach to the console of the parent process, never create one.

#include "support/console.hpp"

#include <QtCore/qt_windows.h>

#include <cstdio>

#include <io.h>

namespace vss::gui::support
{

namespace
{

/// True when the process has no usable handle for `which`, which is what a
/// program of the Windows subsystem gets when nothing was redirected.
bool is_missing(DWORD which)
{
    const auto handle = GetStdHandle(which);
    return handle == nullptr || handle == INVALID_HANDLE_VALUE;
}

/// Points a C stream at the console and, so that code that goes through the
/// handle follows it, the process's standard handle as well.
void reopen(FILE* stream, DWORD which, const char* device)
{
    FILE* reopened = nullptr;
    if (freopen_s(&reopened, device, "w", stream) != 0 || reopened == nullptr)
        return;
    // The C runtime hands the operating-system handle back as an integer.
    SetStdHandle(which,
                 // NOLINTNEXTLINE(performance-no-int-to-ptr)
                 reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))));
}

} // namespace

void attach_parent_console()
{
    const bool need_out = is_missing(STD_OUTPUT_HANDLE);
    const bool need_err = is_missing(STD_ERROR_HANDLE);
    if (!need_out && !need_err)
        return;

    // Fails when the parent has no console (Explorer, the Start menu, the
    // Run key, a service): the program then has nowhere to print, and that is
    // right.
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
        return;

    if (need_out)
        reopen(stdout, STD_OUTPUT_HANDLE, "CONOUT$");
    if (need_err)
        reopen(stderr, STD_ERROR_HANDLE, "CONOUT$");
}

} // namespace vss::gui::support
