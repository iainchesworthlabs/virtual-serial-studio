//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `create_private_file`, the file that `vss_cli control token create --out`
// leaves a token in: what is written, and that a name that is taken is never
// used.  Who can read the file is read back from the operating system in
// unit/platform/<windows|posix>/test_control_storage_*.cpp.
//

#include <vss/control/private_file.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace control = vss::control;

namespace
{

std::string read_all(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void write_all(const std::filesystem::path& file, const std::string& bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out << bytes;
    REQUIRE(static_cast<bool>(out));
}

vss::error_code exists_already()
{
    return vss::make_error_code(vss::errc::already_exists);
}

} // namespace

TEST_CASE("control private file: the bytes are written to a new file")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "rig.token";

    const std::string line = "vsst1_example\n";
    REQUIRE(!control::create_private_file(file, line));
    REQUIRE_EQ(read_all(file), line);

    // Bytes of every value, more than one write long, and nothing added.
    std::string large;
    for (std::size_t index = 0; index < 300 * 1000 + 17; ++index)
        large.push_back(static_cast<char>(index % 251));
    const auto other = scratch.path() / "large.bin";
    REQUIRE(!control::create_private_file(other, large));
    REQUIRE(read_all(other) == large);

    // No bytes is a file too.
    const auto empty = scratch.path() / "empty";
    REQUIRE(!control::create_private_file(empty, ""));
    REQUIRE(std::filesystem::exists(empty));
    REQUIRE_EQ(std::filesystem::file_size(empty), std::uintmax_t{0});
}

TEST_CASE("control private file: a file that exists is refused and left alone")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "rig.token";
    write_all(file, "an earlier file\n");

    const vss::error_code refused =
        control::create_private_file(file, "vsst1_new\n");
    REQUIRE_EQ(refused, exists_already());
    REQUIRE_EQ(read_all(file), std::string("an earlier file\n"));

    // An empty one counts as a file that exists.
    const auto empty = scratch.path() / "empty";
    write_all(empty, "");
    REQUIRE_EQ(control::create_private_file(empty, "vsst1_new\n"),
               exists_already());
    REQUIRE_EQ(std::filesystem::file_size(empty), std::uintmax_t{0});

    // And so does a directory.
    const auto directory = scratch.path() / "folder";
    std::filesystem::create_directory(directory);
    REQUIRE_EQ(control::create_private_file(directory, "vsst1_new\n"),
               exists_already());
    REQUIRE(std::filesystem::is_directory(directory));
}

TEST_CASE("control private file: a second call for one name is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "rig.token";

    REQUIRE(!control::create_private_file(file, "first\n"));
    REQUIRE_EQ(control::create_private_file(file, "second\n"),
               exists_already());
    REQUIRE_EQ(read_all(file), std::string("first\n"));
}

TEST_CASE("control private file: a directory that is missing is an error and "
          "leaves nothing")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "missing" / "rig.token";

    const vss::error_code failed = control::create_private_file(file, "x\n");
    REQUIRE(static_cast<bool>(failed));
    REQUIRE(failed != exists_already());
    REQUIRE(!std::filesystem::exists(scratch.path() / "missing"));
}
