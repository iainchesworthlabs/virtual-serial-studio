//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A new file that only the user who runs the program can read, for what is
// written outside the gateway's own private directory: a token that
// `vss_cli control token create --out` is asked to put in a file.  One
// implementation per platform family in src/platform/<windows|posix>; CMake
// picks the file.
//

#pragma once

#include <vss/core/error.hpp>

#include <filesystem>
#include <string_view>

namespace vss::control
{

/// Makes a new file at `path`, writes all of `bytes` to it and flushes them
/// to the disk.  The file is private from the moment it exists, whatever the
/// directory is: nothing can read it between its making and the writing, and
/// the access of its directory does not matter.  A file that cannot be made
/// private is an error, and then no bytes are written and no file is left.
///
/// An existing file is never used, and nothing is replaced: `path` must not
/// exist, and `errc::already_exists` says it does.  A link at `path` (also
/// one that points nowhere) counts as existing and is not followed.  Other
/// errors are the operating system's.  The directory must exist.
///
/// POSIX: `open` with `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC`
/// and mode 0600, then `fstat` on the descriptor: a regular file of this user
/// with no group or other bits, or the file is removed and the call fails
/// with `errc::permission_denied`.
///
/// Windows: `CreateFileW` with `CREATE_NEW`, not opening a reparse point's
/// target, with a protected access list of one entry (everything for the user
/// who runs the process, nothing inherited), which is read back from the
/// open handle before anything is written.
error_code create_private_file(const std::filesystem::path& path,
                               std::string_view bytes);

} // namespace vss::control
