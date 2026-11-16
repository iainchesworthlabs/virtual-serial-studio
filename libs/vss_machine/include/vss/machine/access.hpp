//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who can change a file or a folder: only an administrator, or others too.
// Whatever Studio reads from a folder that every user of the computer shares
// rests on the answer: the administrator's plugin policy, the kind of install
// a copy of Studio is, and the licences an administrator installs.
//
// The answer is about the access rules on disk, not about who is asking: it
// is the same for an administrator and for an ordinary user.  The check is
// strict on purpose.  Where it cannot tell, it says "others too", and the
// caller then does not trust what is in the folder.
//

#pragma once

#include <boost/system/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace vss::machine
{

enum class write_access
{
    /// Nobody but an administrator can change, replace or remove it.
    administrators_only,
    /// Someone who is not an administrator can.
    others_too
};

std::string_view to_string(write_access value) noexcept;

/// Says who can change what `path` names, or put something else in its
/// place.  The path is resolved first, links included, and then it and
/// every folder above it are looked at, because whoever can replace a
/// folder can replace what is in it.
///
/// On Linux and macOS it is `administrators_only` when the file or folder
/// and every folder above it belong to root and neither their group nor
/// others may write to them.  A group that may write is "others too" even
/// when it is an administrators' group: its members change the files
/// without being asked.
///
/// On Windows it is `administrators_only` when the file or folder and every
/// folder above it belong to Administrators, SYSTEM or TrustedInstaller,
/// and their access lists give nobody else the right to write to, delete or
/// take over the file or folder itself, or to delete, take over or remove
/// things from a folder above it.
///
/// `boost::system::errc::no_such_file_or_directory` when `path` is not
/// there; `boost::system::errc::io_error` when it or a folder above it
/// cannot be examined.  `detail`, when given, receives one line: for
/// `others_too`, which file or folder gave that answer and why; for an
/// error, which file or folder it was.  Nothing escapes as an exception.
boost::system::result<write_access>
who_can_write(const std::filesystem::path& path, std::string* detail = nullptr);

} // namespace vss::machine
