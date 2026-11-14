//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin folders and the policy file on macOS, written out in full.
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

TEST_CASE("plugin locations on macOS: the machine folder and the policy")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "install" / "bin");

    REQUIRE(found.roots.size() >= 2);
    REQUIRE(found.roots[0].directory ==
            scratch.path() / "install" / "studio-plugins");
    REQUIRE(found.roots[1].scope == plugin_scope::for_everyone);
    REQUIRE(found.roots[1].directory ==
            fs::path("/Library/Application Support/VirtualSerialStudio/"
                     "plugins"));
    REQUIRE(found.policy_file ==
            fs::path("/Library/Application Support/VirtualSerialStudio/"
                     "policy.json"));
}

TEST_CASE("plugin locations on macOS: the user's folder is under $HOME")
{
    // The XDG variable means nothing on macOS.
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");
    const scoped_environment home("HOME", "/Users/tester");
    const scratch_directory scratch;

    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");
    const plugin_root* user = user_root(found);
    REQUIRE(user != nullptr);
    REQUIRE(user->directory ==
            fs::path("/Users/tester/Library/Application Support/"
                     "VirtualSerialStudio/plugins"));
}
