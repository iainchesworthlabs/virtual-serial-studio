//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The part of catching the stop signals that differs between systems.  CMake
// builds the file of platform/posix or platform/windows that fits.
//

#pragma once

namespace vss_cli::platform
{

/// Catches the signal that says the terminal the command runs in has gone, in
/// the same way as Ctrl-C: `handler` is called from the signal and may only
/// set a flag.  This is SIGHUP on Linux and macOS, which is sent when a
/// terminal window or an SSH session closes, and SIGBREAK on Windows, which is
/// raised for Ctrl-Break and when the console window closes.
///
/// A signal that was set to be ignored on purpose (`nohup` does that for
/// SIGHUP) stays ignored.
void catch_terminal_close(void (*handler)(int));

} // namespace vss_cli::platform
