//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The access check on Linux and macOS: the owner and the mode bits of one
// file or folder.
//

#include "platform/access_platform.hpp"

#include <sys/stat.h>

namespace vss::plugin::platform
{

result<access_finding> examine(const std::filesystem::path& path, bool /*last*/)
{
    // lstat, not stat: the caller resolved every link already, and a link
    // that appeared since then must be seen as a link, not followed.
    struct stat info = {};
    if (::lstat(path.c_str(), &info) != 0)
        return make_error_code(errc::io_error);

    access_finding found;
    if (S_ISLNK(info.st_mode))
    {
        found.others = true;
        found.why = "is a link";
    }
    else if (info.st_uid != 0)
    {
        // The owner can change the mode and then anything else.
        found.others = true;
        found.why = "does not belong to root";
    }
    else if ((info.st_mode & S_IWOTH) != 0)
    {
        // This counts for a folder above too, sticky bit or not: the rule
        // is the same for the file, its folder and the folders above, so
        // that there is one rule to reason about.
        found.others = true;
        found.why = "can be written to by everyone";
    }
    else if ((info.st_mode & S_IWGRP) != 0)
    {
        found.others = true;
        found.why = "can be written to by its group";
    }
    return found;
}

} // namespace vss::plugin::platform
