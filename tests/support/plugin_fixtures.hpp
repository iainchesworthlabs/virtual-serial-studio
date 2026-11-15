//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small unsigned plugin folder for the tests of installing that need a
// platform's own file kinds (links, junctions, permissions).  The tests that
// need signatures have their own, in unit/test_plugin_install.cpp.
//

#pragma once

#include <vss/plugin/hash.hpp>
#include <vss/plugin/install.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vss_test_support
{

inline void write_plugin_file(const std::filesystem::path& path,
                              std::string_view content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

inline std::string read_plugin_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// An unsigned plugin `parent/folder` whose only file is `bin/prog`.
inline std::filesystem::path make_unsigned_plugin(
    const std::filesystem::path& parent, std::string_view folder,
    std::string_view id = "com.example.modbus",
    std::string_view program = "program bytes")
{
    namespace plug = vss::plugin;
    const std::filesystem::path directory = parent / std::string(folder);
    std::string manifest = "{\n  \"manifest_version\": 1,\n  \"id\": \"";
    manifest += id;
    manifest += "\",\n  \"name\": \"Test\",\n  \"version\": \"1.0.0\",\n";
    manifest += "  \"author\": { \"name\": \"Example Ltd\" },\n";
    manifest += "  \"requires\": { \"api\": { \"min\": \"1.0\", ";
    manifest += "\"max\": \"1.x\" } },\n";
    manifest += "  \"run\": { \"executable\": \"bin/prog\", ";
    manifest += "\"platforms\": [\"linux-x64\"] },\n";
    manifest += "  \"extension_points\": [\n";
    manifest += "    { \"kind\": \"generator\", \"id\": \"pulse\", ";
    manifest += "\"name\": \"Pulse\" }\n  ],\n";
    manifest += "  \"files\": [ { \"path\": \"bin/prog\", \"size\": ";
    manifest += std::to_string(program.size());
    manifest += ", \"sha512\": \"";
    manifest += plug::to_hex(plug::sha512_of(program));
    manifest += "\" } ]\n}\n";
    write_plugin_file(directory / "plugin.json", manifest);
    write_plugin_file(directory / "bin" / "prog", program);
    return directory;
}

/// Options that install an unsigned plugin on this computer's terms.
inline vss::plugin::install_options unsigned_install_options()
{
    vss::plugin::install_options out;
    out.allow_unsigned = true;
    out.context.today = std::chrono::sys_days{std::chrono::year{2026} /
                                              std::chrono::month{11} /
                                              std::chrono::day{14}};
    out.context.sandbox = vss::plugin::sandbox_level::full;
    return out;
}

/// The names in a folder, sorted; none when it is not there.
inline std::vector<std::string>
names_in_folder(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
        return names;
    for (const std::filesystem::directory_entry& each :
         std::filesystem::directory_iterator(directory))
    {
        names.push_back(each.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

} // namespace vss_test_support
