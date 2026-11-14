//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the access check asks the operating system.  One implementation for
// each platform family, in src/platform/<windows|posix>; CMake picks the
// file.  This header is not installed.
//

#pragma once

#include <vss/core/error.hpp>

#include <filesystem>
#include <string>

namespace vss::plugin::platform
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
/// `errc::io_error` when it cannot be examined.
result<access_finding> examine(const std::filesystem::path& path, bool last);

} // namespace vss::plugin::platform
