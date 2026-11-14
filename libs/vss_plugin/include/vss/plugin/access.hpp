//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who can change a file or a folder: only an administrator, or others too.
// Two things rest on the answer.  docs/plugins.md, sections 5.2 and 8.6, are
// the design.
//
// - The kind of install.  A copy of Studio that only an administrator can
//   change is installed for everyone; any other copy is one the person
//   using it controls anyway.
// - The administrator's policy.  A policy file counts only when nobody but
//   an administrator could have written it.
//
// The answer is about the access rules on disk, not about who is asking: it
// is the same for an administrator and for an ordinary user.  The check is
// strict on purpose.  Where it cannot tell, it says "others too", which
// makes a copy of Studio count as one user's and a policy file count as not
// trusted, and a policy file that is not trusted lets only official plugins
// run.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vss::plugin
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
/// `errc::not_found` when `path` is not there; `errc::io_error` when it or
/// a folder above it cannot be examined.  `detail`, when given, receives
/// one line: for `others_too`, which file or folder gave that answer and
/// why.
result<write_access> who_can_write(const std::filesystem::path& path,
                                   std::string* detail = nullptr);

/// The kind of install a copy of Studio is, from the folder that holds its
/// programs: `for_everyone` when only an administrator can change that
/// folder, and `for_one_user` otherwise.  The errors of `who_can_write`.
result<install_kind>
kind_of_install(const std::filesystem::path& program_directory,
                std::string* detail = nullptr);

/// What reading the administrator's policy file gave.
struct loaded_policy
{
    /// The policy to judge plugins by, or nothing when there is no policy
    /// file.  When the file is there and could not be used, this is
    /// `official_only_policy()`.
    std::optional<policy> rules;
    /// Why the file was not used as written, or success.
    /// `errc::permission_denied` when someone who is not an administrator
    /// can change it; `plugin_errc::layout_invalid` when it is a link or
    /// not a plain file; `errc::config_error` when it is too large or is
    /// not a policy this version reads; `errc::io_error` when it cannot be
    /// read.
    error_code problem;
    /// One line for a person about `problem`, or empty.
    std::string detail;
};

/// Reads the administrator's policy from `file`.
///
/// A file that is not there is no policy: `rules` is empty and there is no
/// problem.  A file that is there is used only when it is a plain file,
/// nobody but an administrator can change it (`who_can_write`), and it
/// reads as a policy (`parse_policy`).  In every other case `problem` says
/// why and `rules` is `official_only_policy()`: a policy file that cannot
/// be trusted or understood never counts as no policy.
///
/// Nothing escapes as an exception.
loaded_policy load_policy(const std::filesystem::path& file);

} // namespace vss::plugin
