//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the administrator's policy are looked for on this
// computer.  docs/plugins.md, sections 5.2 and 8.6, are the design, with one
// change: Studio's own plugin folder is named `studio-plugins`, not
// `plugins`.  The Linux package already puts Qt's own plug-ins in
// `<install>/plugins`, and a plugin folder must not be mixed up with that
// one.
//
// Nothing here reads a folder or a file.  It works out names, from the
// folder that holds the programs and from the environment, and the caller
// looks at what is there.
//

#pragma once

#include <vss/plugin/discover.hpp>

#include <filesystem>
#include <vector>

namespace vss::plugin
{

/// Where plugins and the administrator's policy are looked for on this
/// computer, for the copy of Studio whose programs are in
/// `program_directory`.
struct plugin_locations
{
    /// The plugin folders, widest scope first: Studio's own, the machine's
    /// and the user's.  The user's is left out when it cannot be worked out.
    std::vector<plugin_root> roots;
    /// The administrator's policy file.
    std::filesystem::path policy_file;
};

/// The folders for the copy of Studio whose programs are in
/// `program_directory`, which is the folder that holds `vss_cli` and
/// `vss_gui`, such as `<install>/bin`.
///
/// - Studio's own folder (`plugin_scope::with_studio`) is
///   `<install>/studio-plugins`, next to the folder of the programs.  It is
///   made from the text of `program_directory`: a trailing separator is
///   removed and no link is resolved.  It is left out when
///   `program_directory` is empty.
/// - The machine folder (`plugin_scope::for_everyone`) and the policy file
///   are at a fixed place on each system.
/// - The user's folder (`plugin_scope::for_user`) comes from the user's
///   home or data folder, which is read from the environment where the
///   system keeps it there.  A value of the environment is used only when
///   it is an absolute path.  When no folder can be worked out for a root,
///   the root is left out, and when there is no policy file name,
///   `policy_file` is empty.
///
/// Nothing escapes as an exception.
plugin_locations
plugin_locations_for(const std::filesystem::path& program_directory);

} // namespace vss::plugin
