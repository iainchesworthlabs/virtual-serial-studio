//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files that only the user who runs the gateway may read: the audit log now,
// the token store later.  One implementation per platform family in
// src/platform/<windows|posix>, and the one question Linux and macOS answer
// differently (where a program keeps its state) in src/platform/<linux|macos>.
// CMake picks the files.
//

#pragma once

#include <vss/core/error.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

namespace vss::control::platform
{

/// Where this user's programs keep state that outlives a sign-out, with the
/// product's directory added.  It is not made here.
result<std::filesystem::path> state_directory();

/// Makes `directory` (and its missing parents) and makes sure nobody but this
/// user can enter, read or change it.
///
/// POSIX: the last component is made with mode 0700; afterwards it must be a
/// real directory (`lstat`, so not a link), owned by this user, with no group
/// or other bits, or the call fails with `errc::permission_denied`.
///
/// Windows: the last component is made with the protected access list
/// `D:P(A;OICI;FA;;;<user SID>)`; a directory that already exists must be
/// owned by this user, or the call fails with `errc::permission_denied`.
error_code ensure_private_directory(const std::filesystem::path& directory);

/// A file opened for appending that only this user can read.
class private_file
{
public:
    virtual ~private_file() = default;

    /// Appends all of `bytes` and hands them to the operating system.
    virtual error_code append(std::string_view bytes) = 0;
};

/// Opens `path` for appending and makes it when it is missing.
///
/// POSIX: `O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW`, mode 0600; a file
/// that exists must be a regular file owned by this user with no group or
/// other bits (`fstat` on the open descriptor).
///
/// Windows: `FILE_APPEND_DATA`, shared for reading, not inherited by child
/// processes; a new file takes the access list of its private directory.
result<std::unique_ptr<private_file>>
open_private_append(const std::filesystem::path& path);

} // namespace vss::control::platform
