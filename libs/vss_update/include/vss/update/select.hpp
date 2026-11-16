//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What to offer: pure functions of a feed, the running version and what is
// installed (docs/updates.md, section 4.4). The pointers they return point
// into the feed that was given, so the feed must outlive them.
//

#pragma once

#include <vss/update/feed.hpp>
#include <vss/update/version.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vss::update
{

struct studio_update
{
    const studio_release* release = nullptr;
    /// Null when the release has no file for this kind of install and this
    /// architecture: the update is worth telling about, and nothing can be
    /// installed from here.
    const studio_file* file = nullptr;
};

/// The newest Studio release that is greater than `running`, or nothing.
///
/// The entries considered are `stable`, and `preview` too when
/// `include_prerelease` is true or `running` is itself a pre-release. Of
/// those, the one with the greatest version is chosen, and it is an update
/// only when it is greater than `running`. A feed can never cause a
/// downgrade, and the same version is no update.
///
/// A developer build is not special here: the caller decides that no install
/// is offered for one (`is_developer_build`).
std::optional<studio_update> choose_studio(const feed& source,
                                           const version& running,
                                           bool include_prerelease,
                                           file_kind kind, architecture arch);

struct installed_plugin
{
    std::string id;
    update::version version;
    /// Shipped with Studio, so it is updated with Studio.
    bool came_with_studio = false;
    /// Another plugin with the same id was found first.
    bool shadowed = false;
};

struct plugin_update
{
    const plugin_release* release = nullptr;
    /// Null when the release has no file for this system and architecture.
    const plugin_file* file = nullptr;
    /// The release needs a newer Studio than the one running.
    bool studio_too_old = false;
};

/// One entry for each installed plugin, in the order given, that is not
/// shadowed, did not come with Studio and has an entry in the feed with the
/// same id and a greater version. A plugin the user does not have is never
/// offered.
std::vector<plugin_update>
choose_plugins(const feed& source, const version& running, operating_system os,
               architecture arch, std::span<const installed_plugin> installed);

} // namespace vss::update
