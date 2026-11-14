//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are looked for, in what holds on every
// system.  The exact folders of each system are in the platform tests.
//

#include <vss/plugin/discover.hpp>
#include <vss/plugin/locations.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <filesystem>

using namespace vss::plugin;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

/// The root with `scope`, or null.
const plugin_root* find_root(const plugin_locations& found, plugin_scope scope)
{
    for (const plugin_root& root : found.roots)
    {
        if (root.scope == scope)
            return &root;
    }
    return nullptr;
}

} // namespace

TEST_CASE("plugin locations: the roots come widest scope first")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "install" / "bin");

    // The user's folder is left out when it cannot be worked out, so only
    // the first two are certain.
    REQUIRE(found.roots.size() >= 2);
    REQUIRE(found.roots.size() <= 3);
    REQUIRE(found.roots[0].scope == plugin_scope::with_studio);
    REQUIRE(found.roots[1].scope == plugin_scope::for_everyone);
    if (found.roots.size() == 3)
        REQUIRE(found.roots[2].scope == plugin_scope::for_user);
}

TEST_CASE("plugin locations: Studio's own folder is next to the programs")
{
    const scratch_directory scratch;
    const fs::path install = scratch.path() / "install";
    const plugin_locations found = plugin_locations_for(install / "bin");

    const plugin_root* own = find_root(found, plugin_scope::with_studio);
    REQUIRE(own != nullptr);
    REQUIRE(own->directory == install / "studio-plugins");
}

TEST_CASE("plugin locations: a trailing separator changes nothing")
{
    const scratch_directory scratch;
    const fs::path install = scratch.path() / "install";
    // `path / ""` ends the path with a separator.
    const fs::path with_separator = (install / "bin") / "";
    REQUIRE(!with_separator.has_filename());

    const plugin_locations found = plugin_locations_for(with_separator);
    const plugin_locations plain = plugin_locations_for(install / "bin");

    const plugin_root* own = find_root(found, plugin_scope::with_studio);
    REQUIRE(own != nullptr);
    REQUIRE(own->directory == install / "studio-plugins");
    REQUIRE(found.roots.size() == plain.roots.size());
    REQUIRE(found.policy_file == plain.policy_file);
}

TEST_CASE("plugin locations: the program folder is not looked up")
{
    // The text is cut and joined, and nothing is resolved: a ".." stays and
    // the folder need not be there.
    const scratch_directory scratch;
    const fs::path programs = scratch.path() / "a" / ".." / "b" / "bin";
    const plugin_locations found = plugin_locations_for(programs);

    const plugin_root* own = find_root(found, plugin_scope::with_studio);
    REQUIRE(own != nullptr);
    REQUIRE(own->directory == programs.parent_path() / "studio-plugins");
}

TEST_CASE("plugin locations: no program folder gives no Studio folder")
{
    // A relative "studio-plugins" would be looked for wherever the program
    // happens to run.
    const plugin_locations found = plugin_locations_for(fs::path{});
    REQUIRE(find_root(found, plugin_scope::with_studio) == nullptr);
    REQUIRE(find_root(found, plugin_scope::for_everyone) != nullptr);
}

TEST_CASE("plugin locations: the policy file is policy.json")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "bin");
    REQUIRE(!found.policy_file.empty());
    REQUIRE(found.policy_file.filename() == fs::path("policy.json"));
}

TEST_CASE("plugin locations: every path is absolute")
{
    const scratch_directory scratch;
    REQUIRE(scratch.path().is_absolute());
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "install" / "bin");

    REQUIRE(!found.roots.empty());
    for (const plugin_root& root : found.roots)
        REQUIRE(root.directory.is_absolute());
    REQUIRE(found.policy_file.is_absolute());
}

TEST_CASE("plugin locations: the machine and user folders end in plugins")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "bin");

    const plugin_root* machine = find_root(found, plugin_scope::for_everyone);
    REQUIRE(machine != nullptr);
    REQUIRE(machine->directory.filename() == fs::path("plugins"));
    if (const plugin_root* user = find_root(found, plugin_scope::for_user))
        REQUIRE(user->directory.filename() == fs::path("plugins"));
}

TEST_CASE("plugin locations: nothing is created")
{
    const scratch_directory scratch;
    const fs::path programs = scratch.path() / "install" / "bin";
    static_cast<void>(plugin_locations_for(programs));
    REQUIRE(!fs::exists(scratch.path() / "install"));
}
