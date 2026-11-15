//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The product's two folders on this computer, which hold what is kept for
// everyone who uses the computer and what is kept for one user:
//
//   Windows  per computer  <ProgramData>\Virtual Serial Studio
//            per user      <LocalAppData>\Virtual Serial Studio
//   Linux    per computer  /etc/virtual-serial-studio
//            per user      $XDG_DATA_HOME/virtual-serial-studio, or
//                          $HOME/.local/share/virtual-serial-studio when
//                          XDG_DATA_HOME is unset, empty or not an absolute
//                          path
//   macOS    per computer  /Library/Application Support/Virtual Serial Studio
//            per user      $HOME/Library/Application Support/
//                          Virtual Serial Studio
//
// Windows is asked for its known folders and the environment variables of the
// same names are never read: anyone can set a variable for a program they
// start, and a `ProgramData` that points at a folder of the user's own would
// move the computer's folder there.  On Linux and macOS a value from the
// environment counts only when it is an absolute path.
//
// Nothing here creates a folder or looks at one.  Whether the per-computer
// folder can be trusted is `who_can_write`'s question.
//

#pragma once

#include <filesystem>

namespace vss::machine
{

/// The product's folder for every user of this computer.  Empty when it
/// cannot be worked out.
std::filesystem::path machine_folder();

/// The product's folder for the user who runs this program.  Empty when it
/// cannot be worked out.
std::filesystem::path user_folder();

} // namespace vss::machine
