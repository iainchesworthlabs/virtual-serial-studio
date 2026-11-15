//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installing and removing where a folder is a junction.  A junction can be
// made without any privilege (a symbolic link cannot), so these run on every
// Windows computer.  A junction is never followed: as the source, inside the
// source and as the folder of a plugin to remove.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/install.hpp>

#include "plugin_fixtures.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

using namespace vss;
using namespace vss::plugin;
using namespace vss_test_support;

namespace
{

namespace fs = std::filesystem;

/// Makes `link` a junction to the folder `target`.  `mklink /J` needs no
/// privilege.  Nothing when it could not be made.
bool make_junction(const fs::path& link, const fs::path& target)
{
    const std::string command = "mklink /J \"" + link.string() + "\" \"" +
                                target.string() + "\" > nul 2>&1";
    if (std::system(command.c_str()) != 0)
        return false;
    std::error_code ec;
    return fs::exists(link, ec);
}

} // namespace

TEST_CASE("plugin install windows: a source folder that is a junction is "
          "refused")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path junction = scratch.path() / "junction";
    REQUIRE(make_junction(junction, real));

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(junction, root, unsigned_install_options(), &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(names_in_folder(root).empty());
    REQUIRE(fs::exists(real / "plugin.json"));
}

TEST_CASE("plugin install windows: a junction inside the source is refused "
          "and not followed")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    write_plugin_file(scratch.path() / "outside" / "secret.txt", "secret");
    REQUIRE(make_junction(source / "bin" / "out", scratch.path() / "outside"));

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, unsigned_install_options(), &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(names_in_folder(root).empty());
    REQUIRE_EQ(read_plugin_file(scratch.path() / "outside" / "secret.txt"),
               std::string("secret"));
}

TEST_CASE("plugin remove windows: a plugin folder that is a junction is not "
          "followed and not removed")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path root = scratch.path() / "installed";
    std::error_code ec;
    fs::create_directories(root, ec);
    REQUIRE(make_junction(root / "com.example.modbus", real));

    std::string detail;
    const auto done = remove_plugin(root, "com.example.modbus", {}, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(fs::exists(real / "plugin.json"));
    REQUIRE(fs::exists(real / "bin" / "prog"));
    REQUIRE(fs::exists(root / "com.example.modbus"));
}

TEST_CASE("plugin install windows: a replace does not touch a junction in the "
          "plugin's place")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path source = make_unsigned_plugin(scratch.path(), "source",
                                                 "com.example.modbus",
                                                 "new program");
    const fs::path root = scratch.path() / "installed";
    std::error_code ec;
    fs::create_directories(root, ec);
    REQUIRE(make_junction(root / "com.example.modbus", real));

    install_options options = unsigned_install_options();
    options.replace = true;
    const auto done = install_plugin(source, root, options);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE_EQ(read_plugin_file(real / "bin" / "prog"),
               std::string("program bytes"));
}
