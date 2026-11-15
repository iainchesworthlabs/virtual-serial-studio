//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installing a plugin where files have owners' permissions, hard links and
// named pipes: what an install keeps of a file's permissions, and what it
// does with the kinds of file a plugin folder must not hold.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/install.hpp>

#include "plugin_fixtures.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <string>
#include <system_error>

using namespace vss;
using namespace vss::plugin;
using namespace vss_test_support;

namespace
{

namespace fs = std::filesystem;

bool has(fs::perms what, fs::perms bit)
{
    return (what & bit) != fs::perms::none;
}

} // namespace

TEST_CASE("plugin install posix: the program is executable for its owner and "
          "no copy can be written by group or others")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    // The program as a file made by a program that does not set the bit, and
    // the manifest open to everyone to write.
    fs::permissions(source / "bin" / "prog",
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_read | fs::perms::group_write |
                        fs::perms::others_read | fs::perms::others_write,
                    fs::perm_options::replace);
    fs::permissions(source / "plugin.json",
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::replace);

    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, unsigned_install_options());
    REQUIRE(done.has_value());

    const fs::perms program =
        fs::status(done.value().directory / "bin" / "prog").permissions();
    REQUIRE(has(program, fs::perms::owner_exec));
    REQUIRE(has(program, fs::perms::owner_read));
    REQUIRE(has(program, fs::perms::owner_write));
    REQUIRE(has(program, fs::perms::group_read));
    REQUIRE(has(program, fs::perms::others_read));
    REQUIRE(!has(program, fs::perms::group_write));
    REQUIRE(!has(program, fs::perms::others_write));
    REQUIRE(!has(program, fs::perms::group_exec));

    const fs::perms manifest =
        fs::status(done.value().directory / "plugin.json").permissions();
    REQUIRE(has(manifest, fs::perms::owner_read));
    REQUIRE(has(manifest, fs::perms::owner_write));
    REQUIRE(!has(manifest, fs::perms::owner_exec));
    REQUIRE(!has(manifest, fs::perms::group_write));
    REQUIRE(!has(manifest, fs::perms::others_write));
}

TEST_CASE("plugin install posix: a file that is not the program keeps its "
          "owner's execute bit and is not made executable")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    fs::permissions(source / "bin" / "prog", fs::perms::owner_all,
                    fs::perm_options::replace);
    fs::permissions(source / "plugin.json", fs::perms::owner_read |
                                                fs::perms::owner_write,
                    fs::perm_options::replace);
    const auto done = install_plugin(source, scratch.path() / "installed",
                                     unsigned_install_options());
    REQUIRE(done.has_value());
    REQUIRE(has(fs::status(done.value().directory / "bin" / "prog")
                    .permissions(),
                fs::perms::owner_exec));
    REQUIRE(!has(fs::status(done.value().directory / "plugin.json")
                     .permissions(),
                 fs::perms::owner_exec));
}

TEST_CASE("plugin install posix: a hard link in the source is copied as a "
          "separate file")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    // The program is a second name for a file outside the plugin.
    const fs::path outside = scratch.path() / "elsewhere.txt";
    write_plugin_file(outside, "program bytes");
    std::error_code ec;
    fs::remove(source / "bin" / "prog", ec);
    fs::create_hard_link(outside, source / "bin" / "prog", ec);
    if (ec)
        VSS_SKIP("hard links cannot be made here");

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, unsigned_install_options(), &detail);
    if (done.has_error())
    {
        // Refusing a file that something else also names is as safe.
        REQUIRE(done.error() == plugin_errc::layout_invalid);
        REQUIRE(names_in_folder(root).empty());
        return;
    }
    const fs::path copy = done.value().directory / "bin" / "prog";
    REQUIRE_EQ(read_plugin_file(copy), std::string("program bytes"));
    REQUIRE_EQ(fs::hard_link_count(copy), std::uintmax_t{1});
    // Changing the copy leaves the other name alone.
    write_plugin_file(copy, "changed");
    REQUIRE_EQ(read_plugin_file(outside), std::string("program bytes"));
}

TEST_CASE("plugin install posix: a named pipe in the source is refused and "
          "does not stop the install")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    const fs::path pipe = source / "bin" / "pipe";
    if (::mkfifo(pipe.c_str(), 0600) != 0)
        VSS_SKIP("named pipes cannot be made here");

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, unsigned_install_options(), &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(names_in_folder(root).empty());
}
