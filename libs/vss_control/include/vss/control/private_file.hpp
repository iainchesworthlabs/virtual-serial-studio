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
#include <memory>
#include <string_view>

namespace vss::control
{

/// A name that this program has made, empty and private, and holds until
/// `keep` is called.  The file is made first and written later, so that what
/// is to go in it (a token that is made only after the file is) is never made
/// for a file that cannot be had.
///
/// When the claim is destroyed without `keep`, the file is removed: nothing
/// is left behind by a write that failed or by a caller that gave up.
class private_file_claim
{
public:
    virtual ~private_file_claim() = default;

    /// Writes all of `bytes` to the file, after what is there, and flushes
    /// them to the disk.
    virtual error_code write(std::string_view bytes) = 0;

    /// Lets the file stay when the claim is destroyed.
    virtual void keep() noexcept = 0;
};

/// Makes a new, empty file at `path` that only this user can read or change.
/// The file is private from the moment it exists, whatever the directory is:
/// nothing can read it between its making and a later write.  A file that
/// cannot be made private is an error, and then no file is left.
///
/// An existing file is never used, and nothing is replaced: `path` must not
/// exist, and `errc::already_exists` says it does.  A link at `path` (also
/// one that points nowhere) counts as existing and is not followed.  Other
/// errors are the operating system's, or `errc::permission_denied` for a
/// place that cannot be made private.  The directory must exist.
///
/// POSIX: `open` with `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC`
/// and mode 0600, then `fstat` on the descriptor: a regular file of this user
/// with no group or other bits.  Access-control entries that a directory
/// hands to new files (macOS, FreeBSD, ZFS) are not looked at.
///
/// Windows: `CreateFileW` with `CREATE_NEW`, not opening a reparse point's
/// target, with a protected access list of one entry (everything for the user
/// who runs the process, nothing inherited), which is read back from the
/// open handle before anything is written.  Nobody else can open the file
/// while the claim is held.
result<std::unique_ptr<private_file_claim>>
claim_private_file(const std::filesystem::path& path);

/// Makes the file as `claim_private_file` does, writes all of `bytes` to it,
/// and keeps it.  When anything fails no file is left.
error_code create_private_file(const std::filesystem::path& path,
                               std::string_view bytes);

} // namespace vss::control
