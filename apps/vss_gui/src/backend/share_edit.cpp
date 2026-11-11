//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/share_edit.hpp"

#include <vss/core/profile.hpp>

#include <QSet>

#include <algorithm>

namespace vss::gui
{
namespace
{

const port_view* find_port(const std::vector<port_view>& ports,
                           vss::endpoint_id id)
{
    for (const port_view& port : ports)
    {
        if (port.id == id)
            return &port;
    }
    return nullptr;
}

std::optional<share_ref> as_share(const std::vector<port_view>& ports,
                                  const link_view& link)
{
    if (link.mode != vss::link_mode::duplex ||
        (link.shape != link_shape::two_way && link.shape != link_shape::split))
        return std::nullopt;

    QVector<vss::endpoint_id> ends = link.sources;
    ends += link.targets;
    share_ref share;
    share.link = link.id;
    for (const vss::endpoint_id id : ends)
    {
        const port_view* port = find_port(ports, id);
        if (port == nullptr)
            return std::nullopt;
        if (port->kind == vss::endpoint_kind::virtual_port)
        {
            share.programs.push_back(id);
        }
        else if (share.device == 0)
        {
            share.device = id;
        }
        else
        {
            return std::nullopt;
        }
    }
    if (share.device == 0 || share.programs.isEmpty())
        return std::nullopt;
    return share;
}

} // namespace

std::optional<share_ref> find_share(const std::vector<port_view>& ports,
                                    const std::vector<link_view>& links,
                                    vss::endpoint_id port)
{
    for (const link_view& link : links)
    {
        const auto share = as_share(ports, link);
        if (!share)
            continue;
        if (share->device == port || share->programs.contains(port))
            return share;
    }
    return std::nullopt;
}

std::optional<share_ref> find_share_of_link(const std::vector<port_view>& ports,
                                            const std::vector<link_view>& links,
                                            vss::link_id link)
{
    for (const link_view& candidate : links)
    {
        if (candidate.id == link)
            return as_share(ports, candidate);
    }
    return std::nullopt;
}

QString next_program_name(const std::vector<port_view>& ports)
{
    QSet<QString> used;
    for (const port_view& port : ports)
        used.insert(port.name);
    for (int number = 1;; ++number)
    {
        const QString name = QStringLiteral("program-%1").arg(number);
        if (!used.contains(name))
            return name;
    }
}

vss::result<share_added> add_program_port(StudioBackend& backend,
                                          const share_ref& share,
                                          const QString& name, bool capture)
{
    VSS_PROFILE_ZONE("gui.backend.share.add_program");
    if (!backend.link(share.link))
        return vss::make_error_code(vss::errc::not_found);

    const auto created = backend.create_auto_port(name, capture);
    if (!created)
        return created.error();

    const auto added = backend.add_target(share.link, *created);
    if (!added)
    {
        static_cast<void>(backend.remove_port(*created));
        return added.error();
    }
    return share_added{share.link, *created};
}

vss::result<vss::link_id> remove_program_port(StudioBackend& backend,
                                              const share_ref& share,
                                              vss::endpoint_id program)
{
    VSS_PROFILE_ZONE("gui.backend.share.remove_program");
    if (!share.programs.contains(program))
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!backend.link(share.link))
        return vss::make_error_code(vss::errc::not_found);

    if (share.programs.size() == 1)
    {
        const auto removed = backend.remove_port(program);
        if (!removed)
            return removed.error();
        return vss::link_id{0};
    }

    // Take the port off the link first, so the link and the other programs
    // are not disturbed when the port goes.
    const auto detached = backend.remove_target(share.link, program);
    if (!detached)
        return detached.error();

    const auto removed = backend.remove_port(program);
    if (!removed)
    {
        // Put the port back on the link so the setup is as it was.
        static_cast<void>(backend.add_target(share.link, program));
        return removed.error();
    }
    return share.link;
}

} // namespace vss::gui
