//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the user's licence folder is on Linux and macOS, as far as both
// systems agree: it rests on $HOME, and a $HOME that is not an absolute path
// is not used. The exact folders are in the tests of each system.
//

#include <vss/license/store.hpp>

#include "scoped_environment_posix.hpp"
#include "test_framework.hpp"

#include <filesystem>

namespace lic = vss::license;
using vss_test_support::scoped_environment;

namespace
{

namespace fs = std::filesystem;

/// Whether `path` is `base` or is inside it.
bool is_inside(const fs::path& path, const fs::path& base)
{
    const fs::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != fs::path("..");
}

} // namespace

TEST_CASE("licence locations: the user's folder is inside $HOME")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", "/home/tester");

    const fs::path user = lic::user_licence_directory();
    REQUIRE(is_inside(user, "/home/tester"));
    REQUIRE(user.filename() == fs::path("licences"));
    // The clock file goes beside it.
    REQUIRE(lic::default_store_options().clock_file ==
            user.parent_path() / "licence-clock.json");
}

TEST_CASE("licence locations: a $HOME that is not absolute is not used")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", "tester/home");

    REQUIRE(lic::user_licence_directory().empty());
    // The other folder does not depend on it.
    REQUIRE(lic::machine_licence_directory().is_absolute());
    REQUIRE(lic::default_store_options().clock_file.empty());
}

TEST_CASE("licence locations: no $HOME gives no user folder")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    {
        const scoped_environment home("HOME", nullptr);
        REQUIRE(lic::user_licence_directory().empty());
    }
    {
        const scoped_environment home("HOME", "");
        REQUIRE(lic::user_licence_directory().empty());
    }
}

TEST_CASE("licence locations: the machine folder does not use $HOME")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", nullptr);
    REQUIRE(lic::machine_licence_directory().is_absolute());
}
