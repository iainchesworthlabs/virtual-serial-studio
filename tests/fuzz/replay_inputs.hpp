//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the fuzz replay driver turns the command line into the files it reads.
// It is a header so that a unit test can exercise it without the driver's
// main().
//

#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace vss_fuzz_replay
{

namespace fs = std::filesystem;

/// Adds the file named by `root`, or every regular file below it when it is a
/// directory, to `found`. The path is resolved to its canonical form first, so
/// the files that are read are the ones the path really leads to (a link or a
/// `..` is followed once, here) and a path that does not exist is refused
/// rather than opened. A file whose name starts with a dot is skipped: such
/// files (.gitkeep) keep an empty directory in version control.
inline bool collect(const fs::path& requested, std::vector<fs::path>& found)
{
    std::error_code ec;
    const fs::path root = fs::canonical(requested, ec);
    if (ec)
    {
        std::cerr << "fuzz replay: cannot read " << requested.string() << ": "
                  << ec.message() << '\n';
        return false;
    }
    if (fs::is_regular_file(root, ec))
    {
        found.push_back(root);
        return true;
    }
    if (!fs::is_directory(root, ec))
    {
        std::cerr << "fuzz replay: cannot read " << requested.string() << '\n';
        return false;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, ec))
    {
        if (entry.is_regular_file() &&
            !entry.path().filename().string().starts_with('.'))
        {
            found.push_back(entry.path());
        }
    }
    if (ec)
    {
        std::cerr << "fuzz replay: " << requested.string() << ": "
                  << ec.message() << '\n';
        return false;
    }
    return true;
}

/// The bytes of a collected file. An unreadable input must fail the replay,
/// not run as an empty one.
inline std::optional<std::vector<char>> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
    if (in.bad())
        return std::nullopt;
    return bytes;
}

} // namespace vss_fuzz_replay
