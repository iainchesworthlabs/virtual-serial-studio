//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <string_view>

namespace vss::gui::support
{

/// What the build system knew when this binary was built.  The values are in
/// a source file generated before every build (cmake/VssBuildStamp.cmake), so
/// no preprocessor definition carries them.  Anything it could not find is an
/// empty string, not a guess.
struct build_stamp
{
    /// The semantic version, for example "0.1.0-rc.3"; "0.1.0-dev" when the
    /// build could not see a release tag.
    std::string_view version;
    /// The version with what the build knows of itself after a plus sign, for
    /// example "0.1.0-rc.3+18.g3de4617": the commits after the tag, the commit,
    /// ".build.<CI run number>" in a CI build and ".dirty" for a modified tree.
    /// cmake/VssVersion.cmake writes it. It is still the semantic version
    /// "0.1.0-rc.3" to anything that compares versions.
    std::string_view full_version;
    /// The git tag the build is on or after; empty when none was visible.
    std::string_view tag;
    /// Commits between the tag and this build; 0 on the tag itself.
    int commits_since_tag = 0;
    std::string_view commit;      ///< Seven characters.
    std::string_view commit_full; ///< The full revision.
    std::string_view branch;
    bool dirty = false; ///< The working tree differed from the commit.
    /// The CI run number; empty for a build outside CI.
    std::string_view build_number;
    std::string_view build_time; ///< UTC, ISO 8601.
    std::string_view build_type;
    std::string_view compiler;
};

const build_stamp& generated_build_stamp() noexcept;

} // namespace vss::gui::support
