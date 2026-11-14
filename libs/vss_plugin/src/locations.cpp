//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/locations.hpp>

#include "platform/locations_platform.hpp"

#include <filesystem>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

/// Studio's own plugin folder: `studio-plugins` next to the folder that
/// holds the programs.  The text is cut and joined and nothing is looked
/// up, so a link in the path is neither followed nor checked here.
fs::path studio_folder(const fs::path& program_directory)
{
    fs::path programs = program_directory;
    // "a/b/" has an empty last name; its parent is "a/b", the folder itself.
    if (!programs.has_filename())
        programs = programs.parent_path();
    return programs.parent_path() / "studio-plugins";
}

void add_root(std::vector<plugin_root>& roots, fs::path directory,
              plugin_scope scope)
{
    if (directory.empty())
        return;
    plugin_root root;
    root.directory = std::move(directory);
    root.scope = scope;
    roots.push_back(std::move(root));
}

} // namespace

plugin_locations plugin_locations_for(const fs::path& program_directory)
{
    try
    {
        plugin_locations found;
        if (!program_directory.empty())
        {
            add_root(found.roots, studio_folder(program_directory),
                     plugin_scope::with_studio);
        }
        add_root(found.roots, platform::machine_plugin_folder(),
                 plugin_scope::for_everyone);
        add_root(found.roots, platform::user_plugin_folder(),
                 plugin_scope::for_user);
        found.policy_file = platform::policy_file_path();
        return found;
    }
    catch (...)
    {
        // Out of memory, or a path that cannot be built: nothing is found.
        return {};
    }
}

} // namespace vss::plugin
