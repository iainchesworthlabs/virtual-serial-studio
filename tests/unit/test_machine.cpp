//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this computer says, where the answer is the same on every system: the
// computer's name, the product's folders, and the access check against
// folders a test made itself.  The folders of each system, and what only an
// administrator can change, are in the platform tests.
//

#include <vss/machine/access.hpp>
#include <vss/machine/computer_name.hpp>
#include <vss/machine/folders.hpp>

#include <boost/system/errc.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace vss::machine;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

void write_file(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("machine name: the short name stops at the first dot")
{
    REQUIRE(short_computer_name("lab-rig-07.local") == "lab-rig-07");
    REQUIRE(short_computer_name("lab-rig-07.corp.example.com") == "lab-rig-07");
    REQUIRE(short_computer_name("LAB-RIG-07") == "LAB-RIG-07");
    REQUIRE(short_computer_name("").empty());
    REQUIRE(short_computer_name(".local").empty());
    // The case is kept: the licence compares without regard to it.
    REQUIRE(short_computer_name("Lab-Rig-07.local") == "Lab-Rig-07");
}

TEST_CASE("machine name: this computer has a name without a dot")
{
    const std::string name = computer_name();
    REQUIRE(!name.empty());
    REQUIRE(name.find('.') == std::string::npos);
    // The same each time.
    REQUIRE(computer_name() == name);
}

TEST_CASE("machine folders: both are absolute paths named for the product")
{
    const fs::path machine = machine_folder();
    REQUIRE(!machine.empty());
    REQUIRE(machine.is_absolute());
    REQUIRE(machine.has_filename());

    // The user's folder cannot be worked out where there is no home.
    const fs::path user = user_folder();
    if (!user.empty())
    {
        REQUIRE(user.is_absolute());
        REQUIRE(user.has_filename());
        REQUIRE(user.filename() == machine.filename());
    }
    // The same each time.
    REQUIRE(machine_folder() == machine);
    REQUIRE(user_folder() == user);
}

TEST_CASE("machine access: the two answers have names")
{
    REQUIRE(to_string(write_access::administrators_only) ==
            "administrators_only");
    REQUIRE(to_string(write_access::others_too) == "others_too");
}

TEST_CASE("machine access: a path that is not there is an error")
{
    const scratch_directory scratch;
    std::string detail;
    const auto access = who_can_write(scratch.path() / "missing", &detail);
    REQUIRE(access.has_error());
    REQUIRE(access.error() == boost::system::errc::no_such_file_or_directory);
    REQUIRE(detail.find("is not there") != std::string::npos);

    // The detail is optional.
    const auto without = who_can_write(scratch.path() / "missing");
    REQUIRE(without.has_error());
    REQUIRE(without.error() == boost::system::errc::no_such_file_or_directory);
}

TEST_CASE("machine access: what a test made can be changed by others")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "file.txt", "text");
    fs::create_directories(scratch.path() / "bin");

    for (const fs::path& path :
         {scratch.path(), scratch.path() / "file.txt", scratch.path() / "bin"})
    {
        std::string detail;
        const auto access = who_can_write(path, &detail);
        REQUIRE(access.has_value());
        REQUIRE(*access == write_access::others_too);
        // The line says which file or folder gave the answer.
        REQUIRE(!detail.empty());
    }
}

TEST_CASE("machine access: the detail is one line with the path in quotes")
{
    const scratch_directory scratch;
    const fs::path inner = scratch.path() / "inner";
    fs::create_directories(inner);

    std::string detail;
    const auto access = who_can_write(inner, &detail);
    REQUIRE(access.has_value());
    REQUIRE(*access == write_access::others_too);
    REQUIRE(detail.size() > 2);
    REQUIRE(detail.front() == '"');
    REQUIRE(detail.find('\n') == std::string::npos);
}
