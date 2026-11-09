//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the files and directories named on the command line through a fuzz
// harness, once each. It stands in for libFuzzer's main() in ordinary builds,
// so the seed corpus and the inputs of past crashes are checked on every
// platform and compiler.
//

#include "replay_inputs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

using vss_fuzz_replay::collect;
using vss_fuzz_replay::read_file;
namespace fs = std::filesystem;

int main(int argc, char** argv)
try
{
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i)
    {
        if (!collect(argv[i], files))
        {
            return 1;
        }
    }
    // A misspelt directory must not turn into a test that checks nothing.
    if (files.empty())
    {
        std::cerr << "fuzz replay: no input files\n";
        return 1;
    }
    std::ranges::sort(files);

    for (const auto& file : files)
    {
        const auto contents = read_file(file);
        if (!contents)
        {
            std::cerr << "fuzz replay: cannot read " << file.string() << '\n';
            return 1;
        }
        const auto& bytes = *contents;
        static const std::uint8_t empty = 0;
        const auto* data =
            bytes.empty() ? &empty
                          : reinterpret_cast<const std::uint8_t*>(bytes.data());
        if (LLVMFuzzerTestOneInput(data, bytes.size()) != 0)
        {
            std::cerr << "fuzz replay: rejected " << file.string() << '\n';
            return 1;
        }
    }
    std::cout << "fuzz replay: " << files.size() << " inputs\n";
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "error: %s\n", error.what());
    return 2;
}
catch (...)
{
    std::fputs("error: unknown exception\n", stderr);
    return 2;
}
