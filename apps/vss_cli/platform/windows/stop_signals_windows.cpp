//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "stop_signals.hpp"

#include <windows.h>

#include <atomic>
#include <csignal>

namespace vss_cli::platform
{

namespace
{

/// How long Windows lets a console program clean up after its window is
/// closed, the user logs off or the machine shuts down; it ends the process
/// after about five seconds whatever the handler does.
constexpr DWORD cleanup_wait_ms = 4000;

void (*g_request_stop)(int) = nullptr;
std::atomic<HANDLE> g_finished{nullptr};

BOOL WINAPI on_console_event(DWORD type)
{
    switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        // Handled every time: the C runtime's own handler is taken off after
        // the first one, and a second press then ended the process.
        g_request_stop(0);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    {
        // Windows ends the process when this returns, so the ports would stay
        // behind.  Ask the command to stop and wait until it has.
        g_request_stop(0);
        if (const auto finished = g_finished.load(); finished != nullptr)
            static_cast<void>(WaitForSingleObject(finished, cleanup_wait_ms));
        return TRUE;
    }
    default:
        return FALSE;
    }
}

} // namespace

void catch_stop_signals(void (*request_stop)(int))
{
    g_request_stop = request_stop;
    if (g_finished.load() == nullptr)
        g_finished.store(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    // Only raise() sends SIGTERM here, but the commands have always taken it.
    std::signal(SIGTERM, request_stop);
    static_cast<void>(SetConsoleCtrlHandler(on_console_event, TRUE));
}

void command_finished()
{
    if (const auto finished = g_finished.load(); finished != nullptr)
        static_cast<void>(SetEvent(finished));
}

} // namespace vss_cli::platform
