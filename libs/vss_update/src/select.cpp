//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/update/select.hpp>

#include <algorithm>

namespace vss::update
{

std::optional<studio_update> choose_studio(const feed& source,
                                           const version& running,
                                           bool include_prerelease,
                                           file_kind kind, architecture arch)
{
    const bool preview_wanted = include_prerelease || running.is_prerelease();

    const studio_release* best = nullptr;
    for (const studio_release& release : source.studio)
    {
        if (release.channel == channel::preview && !preview_wanted)
            continue;
        if (best == nullptr || compare(release.version, best->version) > 0)
            best = &release;
    }
    if (best == nullptr || compare(best->version, running) <= 0)
        return std::nullopt;

    studio_update out;
    out.release = best;
    const auto file = std::ranges::find_if(
        best->files, [&](const studio_file& candidate)
        { return candidate.kind == kind && candidate.arch == arch; });
    if (file != best->files.end())
        out.file = &*file;
    return out;
}

std::vector<plugin_update>
choose_plugins(const feed& source, const version& running, operating_system os,
               architecture arch, std::span<const installed_plugin> installed)
{
    std::vector<plugin_update> out;
    for (const installed_plugin& plugin : installed)
    {
        if (plugin.shadowed || plugin.came_with_studio)
            continue;
        const auto entry = std::ranges::find_if(
            source.plugins, [&](const plugin_release& candidate)
            { return candidate.id == plugin.id; });
        if (entry == source.plugins.end() ||
            compare(entry->version, plugin.version) <= 0)
            continue;

        plugin_update offer;
        offer.release = &*entry;
        offer.studio_too_old = compare(entry->requires_studio, running) > 0;
        const auto file = std::ranges::find_if(
            entry->files, [&](const plugin_file& candidate)
            { return candidate.os == os && candidate.arch == arch; });
        if (file != entry->files.end())
            offer.file = &*file;
        out.push_back(offer);
    }
    return out;
}

} // namespace vss::update
