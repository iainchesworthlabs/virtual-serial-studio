//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a command is asked to stop, which differs between systems.  CMake builds
// the file of platform/posix or platform/windows that fits.
//

#pragma once

namespace vss_cli::platform
{

/// Starts catching what asks a command to stop: Ctrl-C and SIGTERM on every
/// system, SIGHUP on Linux and macOS (a terminal window or an SSH session
/// closes), and on Windows Ctrl-Break and the console window closing, the user
/// logging off or the machine shutting down.  `request_stop` is called from
/// the signal, or from a thread Windows starts, and may only set a flag.
///
/// Linux and macOS: a SIGHUP that was set to be ignored on purpose (`nohup`
/// does that) stays ignored, and SIGPIPE is ignored, so that a closed standard
/// output makes a write fail, not end the command before it has taken its
/// ports down.
///
/// Windows: Ctrl-C and Ctrl-Break are handled as often as they come, so a
/// second press cannot end the command before its ports are gone.  For a
/// closing console, a logoff and a shutdown Windows ends the process as soon as
/// the handler returns, so the handler waits, for a few seconds at most, for
/// `command_finished`.
void catch_stop_signals(void (*request_stop)(int));

/// Says that the command has returned and its ports are down.  A handler that
/// is waiting for it (Windows) lets the process end.  Does nothing on Linux
/// and macOS.
void command_finished();

} // namespace vss_cli::platform
