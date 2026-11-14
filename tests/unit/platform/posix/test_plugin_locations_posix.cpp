//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the user's plugin folder is on Linux and macOS, as far as both
// systems agree: it rests on $HOME, and a $HOME that is not an absolute path
// is not used.  The exact folders are in the tests of each system.
//

#include <vss/plugin/discover.hpp>
#include <vss/plugin/locations.hpp>

#include "scoped_environment_posix.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <filesystem>

using namespace vss::plugin;
using vss_test_support::scoped_environment;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

const plugin_root* user_root(const plugin_locations& found)
{
    for (const plugin_root& root : found.roots)
    {
        if (root.scope == plugin_scope::for_user)
            return &root;
    }
    return nullptr;
}

/// Whether `path` is `base` or is inside it.
bool is_inside(const fs::path& path, const fs::path& base)
{
    const fs::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != fs::path("..");
}

} // namespace

TEST_CASE("plugin locations: the user's folder is inside $HOME")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", "/home/tester");
    const scratch_directory scratch;

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    const plugin_root* user = user_root(found);
    REQUIRE(user != nullptr);
    REQUIRE(is_inside(user->directory, "/home/tester"));
    REQUIRE(user->directory.filename() == fs::path("plugins"));
    // The user's folder is the last.
    REQUIRE(&found.roots.back() == user);
}

TEST_CASE("plugin locations: a $HOME that is not absolute is not used")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", "tester/home");
    const scratch_directory scratch;

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    REQUIRE(user_root(found) == nullptr);
    // The other folders do not depend on it.
    REQUIRE(found.roots.size() == 2);
}

TEST_CASE("plugin locations: no $HOME gives no user folder")
{
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scratch_directory scratch;
    {
        const scoped_environment home("HOME", nullptr);
        REQUIRE(user_root(plugin_locations_for(scratch.path() / "bin")) ==
                nullptr);
    }
    {
        const scoped_environment home("HOME", "");
        REQUIRE(user_root(plugin_locations_for(scratch.path() / "bin")) ==
                nullptr);
    }
}

TEST_CASE("plugin locations: the machine folder and policy do not use $HOME")
{
    const scratch_directory scratch;
    const scoped_environment data("XDG_DATA_HOME", nullptr);
    const scoped_environment home("HOME", nullptr);

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    REQUIRE(found.roots.size() == 2);
    REQUIRE(found.roots[1].scope == plugin_scope::for_everyone);
    REQUIRE(found.roots[1].directory.is_absolute());
    REQUIRE(found.policy_file.is_absolute());
}
