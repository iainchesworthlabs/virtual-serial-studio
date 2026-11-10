//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/undo_steps.hpp"

#include <QVector>

#include <algorithm>
#include <vector>

namespace vss::gui
{
namespace
{

QVector<vss::endpoint_id> sorted(QVector<vss::endpoint_id> ids)
{
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<vss::endpoint_id> to_vector(const QVector<vss::endpoint_id>& ids)
{
    return std::vector<vss::endpoint_id>(ids.begin(), ids.end());
}

bool same_ends(const link_view& left, const link_view& right)
{
    return left.shape == right.shape &&
           sorted(left.sources) == sorted(right.sources) &&
           sorted(left.targets) == sorted(right.targets);
}

/// Makes the link again with the same shape, ports, options and owner.  False
/// when the same link is already there, because then the history no longer
/// describes the setup.
bool create_link(StudioBackend& backend, const link_view& spec)
{
    if (find_link(backend, spec) || spec.sources.isEmpty() ||
        (spec.shape != link_shape::bus && spec.targets.isEmpty()))
        return false;

    vss::result<vss::link_id> made =
        vss::make_error_code(vss::errc::internal);
    switch (spec.shape)
    {
    case link_shape::two_way:
        made = backend.connect_ports(spec.sources.first(),
                                     spec.targets.first(), spec.options);
        break;
    case link_shape::one_way:
        made = backend.redirect(spec.sources.first(), spec.targets.first(),
                                spec.options);
        break;
    case link_shape::monitor:
        made = backend.monitor(spec.sources.first(), spec.targets.first());
        break;
    case link_shape::split:
        made = backend.split(spec.sources.first(), to_vector(spec.targets),
                             spec.options);
        break;
    case link_shape::join:
        made = backend.join(to_vector(spec.sources), spec.targets.first(),
                            spec.options);
        break;
    case link_shape::bus:
        made = backend.bus(to_vector(spec.sources), spec.options);
        break;
    }
    if (!made)
        return false;
    if (spec.owner != 0 && !backend.set_owner(*made, spec.owner))
        return false;
    backend.refresh();
    return true;
}

bool remove_link(StudioBackend& backend, const link_view& spec)
{
    const auto found = find_link(backend, spec);
    if (!found || !backend.disconnect(found->id))
        return false;
    backend.refresh();
    return true;
}

bool change_owner(StudioBackend& backend, const link_view& spec,
                  vss::endpoint_id owner)
{
    const auto found = find_link(backend, spec);
    if (!found)
        return false;
    const auto changed = owner == 0 ? backend.release_owner(found->id)
                                    : backend.set_owner(found->id, owner);
    if (!changed)
        return false;
    backend.refresh();
    return true;
}

} // namespace

std::optional<link_view> find_link(const StudioBackend& backend,
                                   const link_view& spec)
{
    for (const link_view& candidate : backend.links())
    {
        if (same_ends(candidate, spec))
            return candidate;
    }
    return std::nullopt;
}

undo_step connect_step(StudioBackend& backend, const link_view& spec,
                       const QString& text)
{
    return {text, [&backend, spec] { return remove_link(backend, spec); },
            [&backend, spec] { return create_link(backend, spec); }};
}

undo_step remove_link_step(StudioBackend& backend, const link_view& spec,
                           const QString& text)
{
    return {text, [&backend, spec] { return create_link(backend, spec); },
            [&backend, spec] { return remove_link(backend, spec); }};
}

undo_step owner_step(StudioBackend& backend, const link_view& spec,
                     vss::endpoint_id before, vss::endpoint_id after,
                     const QString& text)
{
    return {text,
            [&backend, spec, before] {
                return change_owner(backend, spec, before);
            },
            [&backend, spec, after] {
                return change_owner(backend, spec, after);
            }};
}

undo_step rename_step(StudioBackend& backend, vss::endpoint_id port,
                      const QString& before, const QString& after,
                      const QString& text)
{
    const auto rename = [&backend, port](const QString& name)
    {
        if (!backend.rename_port(port, name))
            return false;
        backend.refresh();
        return true;
    };
    return {text, [rename, before] { return rename(before); },
            [rename, after] { return rename(after); }};
}

} // namespace vss::gui
