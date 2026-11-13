//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Running the commands of vss_license in a test.
//

#pragma once

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <tool.hpp>

#include <vss/license/license.hpp>
#include <vss/license/signed_file.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace vss_test_support::license_tool
{

using vss_test_support::scratch_directory;

struct outcome
{
    int status = 0;
    std::string out;
    std::string err;
};

using variable_reader =
    std::function<std::optional<std::string>(const std::string&)>;

/// Runs one command at 1 June 2027, inside the term of the golden licence.
inline outcome run_tool(std::initializer_list<std::string> words,
                        variable_reader variable = {})
{
    const std::vector<std::string> arguments(words);
    std::ostringstream out;
    std::ostringstream err;
    vss::license::tool::surroundings where{
        out, err, std::move(variable),
        *vss::license::parse_time("2027-06-01T00:00:00Z")};
    outcome result;
    result.status = vss::license::tool::run(arguments, where);
    result.out = out.str();
    result.err = err.str();
    return result;
}

inline std::string path_in(const scratch_directory& scratch,
                           std::string_view name)
{
    return (scratch.path() / name).string();
}

inline std::string read_text(const std::string& path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

inline std::string write_text(const scratch_directory& scratch,
                              std::string_view name, std::string_view text)
{
    const std::string path = path_in(scratch, name);
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return path;
}

inline bool exists(const std::string& path)
{
    return std::filesystem::exists(std::filesystem::path(path));
}

/// The licence signed with the published test key that the library's own
/// tests pin (tests/data/licence).
inline std::string golden_path()
{
    return (std::filesystem::path(VSS_TEST_DATA_DIR) / "licence" /
            "annual-test-key.vsslic")
        .string();
}

/// The payload of the golden licence: what a request file holds.
inline std::string golden_payload()
{
    const auto file =
        vss::license::signed_file::parse(read_text(golden_path()));
    if (!file)
        return {};
    return {file->payload.begin(), file->payload.end()};
}

inline std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace vss_test_support::license_tool
