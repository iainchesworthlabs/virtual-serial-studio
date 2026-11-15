//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this library asks the operating system.  One implementation for each
// platform, in src/platform/<windows|linux|macos>, with the access check
// shared by Linux and macOS in src/platform/posix; CMake picks the files.
// `machine_folder` and `user_folder` are declared in folders.hpp and defined
// in the platform files.  This header is not installed.
//

#pragma once

#include <boost/system/result.hpp>

#include <filesystem>
#include <string>

namespace vss::machine::platform
{

/// What is wrong with one file or folder for the access check, in words.
struct access_finding
{
    /// Whether someone who is not an administrator can change it.
    bool others = false;
    /// For `others`: why, as the end of a sentence that starts with the
    /// path, such as "belongs to a user".
    std::string why;
};

/// Looks at one file or folder, which exists and is not a link.
///
/// `last` says that it is the file or folder the question is about, and
/// not a folder above it.  For the last one, any right to write counts; for
/// a folder above, only the rights that let someone replace or remove what
/// is inside it.
///
/// `boost::system::errc::io_error` when it cannot be examined.
boost::system::result<access_finding> examine(const std::filesystem::path& path,
                                              bool last);

/// The name the system gives this computer, in UTF-8 and not cut: a name
/// that has dots keeps them.  Empty when there is none.
std::string full_computer_name();

} // namespace vss::machine::platform
