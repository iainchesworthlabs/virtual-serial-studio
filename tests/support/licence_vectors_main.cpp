//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_licence_vectors <folder>: writes the licence test vectors and their
// index into a folder, which is how the files of sdk/licence-reader/vectors
// are made. It signs with the published RFC 8032 test key and has no way to
// use another. It is a test tool and is in no package.
//

#include "licence_vectors.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <string>
#include <vector>

namespace
{

namespace vectors = vss_test_support::licence_vectors;

bool write_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return !out.fail();
}

int run(const std::filesystem::path& requested)
{
    // The folder is resolved to its canonical form, so the files are written
    // where the argument really leads.
    std::error_code resolved;
    const std::filesystem::path folder =
        std::filesystem::weakly_canonical(requested, resolved);
    if (resolved)
    {
        std::cerr << "cannot use " << requested.string() << ": "
                  << resolved.message() << '\n';
        return 1;
    }
    std::filesystem::create_directories(folder);
    // A vector that was removed leaves no file behind.
    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        if (entry.path().extension() == ".vsslic")
        {
            std::filesystem::remove(entry.path());
        }
    }
    const std::vector<vectors::licence_vector> all = vectors::all();
    for (const vectors::licence_vector& vector : all)
    {
        if (!write_file(folder / (vector.name + ".vsslic"), vector.file_text))
        {
            std::cerr << "cannot write " << vector.name << ".vsslic\n";
            return 1;
        }
    }
    if (!write_file(folder / "index.json", vectors::index_text()))
    {
        std::cerr << "cannot write index.json\n";
        return 1;
    }
    std::cout << all.size() << " vectors and index.json written to "
              << folder.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: vss_licence_vectors <folder>\n";
        return 2;
    }
    try
    {
        return run(argv[1]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "vss_licence_vectors: " << error.what() << '\n';
        return 1;
    }
}
