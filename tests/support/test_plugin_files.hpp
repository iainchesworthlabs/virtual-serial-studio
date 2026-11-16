//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The files that make up the test plugin as a plugin folder holds it: the
// program, and in a build with the address sanitiser of MSVC the runtime
// library that must sit beside it, because a plugin is started with an
// environment of its own and finds no library through PATH.
//

#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace vss_test_support
{

/// The program and, when `runtime_name` is not empty and a file of that name
/// is beside the program, that file.  The program is first.
inline std::vector<std::filesystem::path>
test_plugin_files(const std::filesystem::path& program,
                  std::string_view runtime_name)
{
    std::vector<std::filesystem::path> files{program};
    if (!runtime_name.empty())
    {
        const std::filesystem::path runtime =
            program.parent_path() / std::filesystem::path(runtime_name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(runtime, ec) && !ec)
            files.push_back(runtime);
    }
    return files;
}

/// Copies each of those files into `directory`, which exists, under its own
/// file name and over a file of that name.  Returns the copies, the program
/// first.  It stops at the first failure, which `ec` then holds.
inline std::vector<std::filesystem::path>
copy_test_plugin(const std::filesystem::path& program,
                 std::string_view runtime_name,
                 const std::filesystem::path& directory,
                 std::error_code& ec)
{
    std::vector<std::filesystem::path> copies;
    for (const std::filesystem::path& file :
         test_plugin_files(program, runtime_name))
    {
        const std::filesystem::path target = directory / file.filename();
        std::filesystem::copy_file(
            file, target, std::filesystem::copy_options::overwrite_existing,
            ec);
        if (ec)
            break;
        copies.push_back(target);
    }
    return copies;
}

} // namespace vss_test_support
