//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin folders and the policy file on Linux, written out in full, and
// the rules for $XDG_DATA_HOME.
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

} // namespace

TEST_CASE("plugin locations on Linux: the machine folder and the policy")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "install" / "bin");

    REQUIRE(found.roots.size() >= 2);
    REQUIRE(found.roots[0].directory ==
            scratch.path() / "install" / "studio-plugins");
    REQUIRE(found.roots[1].scope == plugin_scope::for_everyone);
    REQUIRE(found.roots[1].directory ==
            fs::path("/usr/local/share/virtual-serial-studio/plugins"));
    REQUIRE(found.policy_file ==
            fs::path("/etc/virtual-serial-studio/policy.json"));
}

TEST_CASE("plugin locations on Linux: $XDG_DATA_HOME is used when absolute")
{
    const scoped_environment home("HOME", "/home/tester");
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");
    const scratch_directory scratch;

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    const plugin_root* user = user_root(found);
    REQUIRE(user != nullptr);
    REQUIRE(user->directory ==
            fs::path("/srv/data/virtual-serial-studio/plugins"));
}

TEST_CASE("plugin locations on Linux: $HOME is the fallback")
{
    const scoped_environment home("HOME", "/home/tester");
    const scratch_directory scratch;
    const fs::path expected =
        "/home/tester/.local/share/virtual-serial-studio/plugins";

    {
        // Unset.
        const scoped_environment data("XDG_DATA_HOME", nullptr);
        const plugin_locations found =
            plugin_locations_for(scratch.path() / "bin");
        const plugin_root* user = user_root(found);
        REQUIRE(user != nullptr);
        REQUIRE(user->directory == expected);
    }
    {
        // Empty.
        const scoped_environment data("XDG_DATA_HOME", "");
        const plugin_locations found =
            plugin_locations_for(scratch.path() / "bin");
        const plugin_root* user = user_root(found);
        REQUIRE(user != nullptr);
        REQUIRE(user->directory == expected);
    }
    {
        // Not an absolute path.
        const scoped_environment data("XDG_DATA_HOME", "data/relative");
        const plugin_locations found =
            plugin_locations_for(scratch.path() / "bin");
        const plugin_root* user = user_root(found);
        REQUIRE(user != nullptr);
        REQUIRE(user->directory == expected);
    }
}

TEST_CASE("plugin locations on Linux: no usable variable gives no user folder")
{
    const scoped_environment home("HOME", nullptr);
    const scoped_environment data("XDG_DATA_HOME", "data/relative");
    const scratch_directory scratch;

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    REQUIRE(user_root(found) == nullptr);
}
