//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/job_plan.hpp"

#include "backend/driver_error.hpp"

#include <vss/core/profile.hpp>

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <optional>

namespace vss::gui::simple
{

namespace
{

std::optional<port_view> find(const std::vector<port_view>& ports,
                              vss::endpoint_id id)
{
    for (const auto& port : ports)
    {
        if (port.id == id)
            return port;
    }
    return std::nullopt;
}

end_plan end_of(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    end_plan end;
    end.existing = id;
    const auto port = find(ports, id);
    if (!port)
        return end;
    switch (port->kind)
    {
    case vss::endpoint_kind::virtual_port:
        end.what = end_plan::kind::program;
        end.label = program_label(*port);
        end.com_number = port->com_number;
        break;
    case vss::endpoint_kind::physical:
        end.what = end_plan::kind::device;
        end.device_path = port_shown_name(*port);
        break;
    default:
    {
        end.what = end_plan::kind::network_device;
        end.rfc2217 = port->kind == vss::endpoint_kind::rfc2217;
        // The path of a network port reads host:port.
        const QString where = port->path.section(QStringLiteral("://"), -1);
        end.address = where.section(QLatin1Char(':'), 0, 0);
        end.network_port = where.section(QLatin1Char(':'), 1, 1)
                               .section(QLatin1Char('?'), 0, 0)
                               .toUInt();
        if (end.network_port == 0)
            end.network_port = 4001;
        break;
    }
    }
    return end;
}

/// A port name that no other port has: the label, or "program", with a
/// number added where needed.
QString unique_name(const QString& wanted, const QSet<QString>& taken)
{
    QString base = wanted.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("program");
    if (!taken.contains(base) && base != QStringLiteral("program"))
        return base;
    for (int index = 2;; ++index)
    {
        const QString candidate = QStringLiteral("%1-%2").arg(base).arg(index);
        if (!taken.contains(candidate))
            return candidate;
    }
}

vss::result<vss::link_id> relink(StudioBackend& backend, const link_view& link)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.relink");
    const std::vector<vss::endpoint_id> sources(link.sources.begin(),
                                                link.sources.end());
    const std::vector<vss::endpoint_id> targets(link.targets.begin(),
                                                link.targets.end());
    switch (link.shape)
    {
    case link_shape::two_way:
        if (sources.size() == 1 && targets.size() == 1)
            return backend.connect_ports(sources[0], targets[0], link.options);
        break;
    case link_shape::split:
        if (sources.size() == 1)
        {
            auto made = backend.split(sources[0], targets, link.options);
            if (made && link.owner != 0)
                static_cast<void>(backend.set_owner(*made, link.owner));
            return made;
        }
        break;
    case link_shape::join:
        if (targets.size() == 1)
        {
            auto made = backend.join(sources, targets[0], link.options);
            if (made && link.owner != 0)
                static_cast<void>(backend.set_owner(*made, link.owner));
            return made;
        }
        break;
    case link_shape::bus:
        return backend.bus(sources);
    default:
        break;
    }
    return vss::make_error_code(vss::errc::unsupported);
}

/// Whether `old` can be changed into what `kind` builds with `add_target` and
/// `remove_target`: a duplex link from the same device with the merge policy
/// the kind needs.  The shape is not asked: the real engine reports a duplex
/// link with one target as `two_way`, and with more as `split`.
bool edits_in_place(const link_view& old, job kind, vss::endpoint_id device)
{
    if (kind != job::attach && kind != job::share && kind != job::turns)
        return false;
    const vss::merge_policy wanted = kind == job::turns
                                         ? vss::merge_policy::owner
                                         : vss::merge_policy::interleave;
    return old.mode == vss::link_mode::duplex && old.sources.size() == 1 &&
           old.sources.front() == device && old.options.policy == wanted;
}

/// Makes the targets of `old` the `programs`: adds the new ones first, so the
/// link always has a target, then takes off the ones that are not wanted.  The
/// link keeps its id, options and counters.  A failure puts the targets back,
/// so the link is as it was.
vss::result<void> edit_targets(StudioBackend& backend, const link_view& old,
                               const std::vector<vss::endpoint_id>& programs)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.edit_targets");
    std::vector<vss::endpoint_id> added;
    std::vector<vss::endpoint_id> removed;
    const auto roll_back = [&]
    {
        for (const auto id : removed)
            static_cast<void>(backend.add_target(old.id, id));
        for (auto it = added.rbegin(); it != added.rend(); ++it)
            static_cast<void>(backend.remove_target(old.id, *it));
        // Taking the owner off the link released it.
        if (old.owner != 0 && std::find(removed.begin(), removed.end(),
                                        old.owner) != removed.end())
            static_cast<void>(backend.set_owner(old.id, old.owner));
    };

    for (const auto id : programs)
    {
        if (old.targets.contains(id) ||
            std::find(added.begin(), added.end(), id) != added.end())
            continue;
        const auto result = backend.add_target(old.id, id);
        if (!result)
        {
            roll_back();
            return result.error();
        }
        added.push_back(id);
    }
    for (const auto id : old.targets)
    {
        if (std::find(programs.begin(), programs.end(), id) != programs.end())
            continue;
        const auto result = backend.remove_target(old.id, id);
        if (!result)
        {
            roll_back();
            return result.error();
        }
        removed.push_back(id);
    }
    return {};
}

} // namespace

QString device_uri(const end_plan& end, const vss::serial_settings& serial)
{
    if (end.what == end_plan::kind::network_device)
    {
        return QStringLiteral("%1://%2:%3")
            .arg(end.rfc2217 ? QStringLiteral("rfc2217")
                             : QStringLiteral("tcp"),
                 end.address.trimmed())
            .arg(end.network_port);
    }
    return QStringLiteral("serial://%1?baud=%2")
        .arg(end.device_path)
        .arg(serial.baud_rate);
}

job_plan plan_from(const setup_view& setup, const std::vector<port_view>& ports)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.plan_from");
    job_plan plan;
    plan.kind = setup.kind;
    const auto ends = [&ports](const QVector<vss::endpoint_id>& ids)
    {
        std::vector<end_plan> result;
        for (const auto id : ids)
            result.push_back(end_of(ports, id));
        return result;
    };
    if (setup.device != 0)
    {
        if (const auto device = find(ports, setup.device))
            plan.serial = device->serial;
    }
    switch (setup.kind)
    {
    case job::port:
        plan.first = ends(setup.programs);
        plan.echo = setup.echo != 0;
        break;
    case job::pair:
        plan.first = ends(setup.programs.mid(0, 1));
        plan.second = ends(setup.programs.mid(1, 1));
        break;
    case job::attach:
    case job::share:
        plan.first = ends({setup.device});
        plan.second = ends(setup.programs);
        break;
    case job::talk:
        plan.first = ends({setup.device});
        break;
    case job::network:
    {
        plan.first = ends({setup.device});
        if (const auto listener = find(ports, setup.listener))
        {
            const unsigned number = listener->path.section(QLatin1Char(':'), -1)
                                        .section(QLatin1Char('?'), 0, 0)
                                        .toUInt();
            if (number != 0)
                plan.listen_port = number;
        }
        break;
    }
    case job::bus:
        plan.first = ends(setup.members);
        break;
    case job::combine:
        plan.first = ends(setup.members);
        plan.second = ends({setup.receiver});
        break;
    case job::turns:
        plan.first = ends(setup.programs);
        plan.second = ends({setup.device});
        break;
    case job::custom:
        break;
    }
    return plan;
}

plan_changes compare(const job_plan& plan, const setup_view& previous)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.compare");
    plan_changes changes;
    QSet<vss::endpoint_id> kept;
    for (const auto* list : {&plan.first, &plan.second})
    {
        for (const auto& end : *list)
        {
            if (end.existing != 0 &&
                previous.group.ports.contains(end.existing))
            {
                kept.insert(end.existing);
                changes.kept.push_back(end.existing);
            }
            else
            {
                ++changes.added;
            }
        }
    }
    for (const auto id : previous.group.ports)
    {
        // The echo and the network listener are not ends the user lists.
        if (id == previous.echo || id == previous.listener)
            continue;
        if (!kept.contains(id))
            changes.closed.push_back(id);
    }
    return changes;
}

vss::result<vss::endpoint_id> build_setup(StudioBackend& backend,
                                          const job_plan& plan,
                                          const setup_view* previous)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.build_setup");
    const auto before = backend.ports();
    QSet<QString> taken;
    for (const auto& port : before)
        taken.insert(port.name);

    std::vector<vss::endpoint_id> made;
    const auto undo_made = [&backend, &made]
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
            static_cast<void>(backend.remove_port(*it));
        made.clear();
    };

    // Letting other computers in is one engine operation that opens the
    // device and the network port together, so a change makes it again.
    if (plan.kind == job::network)
    {
        if (plan.first.empty())
            return vss::make_error_code(vss::errc::invalid_argument);
        if (previous != nullptr)
        {
            for (const auto id : previous->group.ports)
                static_cast<void>(backend.remove_port(id));
        }
        const auto linked = backend.bridge(
            device_uri(plan.first.front(), plan.serial),
            QStringLiteral("tcp://0.0.0.0:%1").arg(plan.listen_port));
        if (!linked)
            return linked.error();
        if (const auto link = backend.link(*linked))
        {
            if (!link->sources.isEmpty())
                return link->sources.front();
        }
        return vss::endpoint_id{0};
    }

    const auto make_end =
        [&](const end_plan& end) -> vss::result<vss::endpoint_id>
    {
        if (end.existing != 0)
            return end.existing;
        if (end.what == end_plan::kind::program)
        {
            const QString name = unique_name(end.label, taken);
            taken.insert(name);
            auto port =
                end.com_number != 0 && backend.caps().com_numbers
                    ? backend.create_driver_port(name, true, end.com_number)
                    : backend.create_auto_port(name, true);
            if (!port)
            {
                // A missing driver or helper looks like any other refusal;
                // say which, so the form can offer the set-up.
                return driver_aware_error(backend, port.error());
            }
            made.push_back(*port);
            return port;
        }
        const QString path =
            end.what == end_plan::kind::device ? end.device_path : end.address;
        QString base = QStringLiteral("device-%1").arg(path).toLower();
        base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                     QStringLiteral("-"));
        while (base.endsWith(QLatin1Char('-')))
            base.chop(1);
        const QString name = unique_name(base, taken);
        taken.insert(name);
        auto port = backend.attach_serial(name, device_uri(end, plan.serial),
                                          plan.serial, true);
        if (port)
            made.push_back(*port);
        return port;
    };

    const auto make_all = [&](const std::vector<end_plan>& ends)
        -> vss::result<std::vector<vss::endpoint_id>>
    {
        std::vector<vss::endpoint_id> ids;
        for (const auto& end : ends)
        {
            const auto id = make_end(end);
            if (!id)
                return id.error();
            ids.push_back(*id);
        }
        return ids;
    };

    const auto first = make_all(plan.first);
    if (!first)
    {
        undo_made();
        return first.error();
    }
    const auto second = make_all(plan.second);
    if (!second)
    {
        undo_made();
        return second.error();
    }
    if (first->empty())
    {
        undo_made();
        return vss::make_error_code(vss::errc::invalid_argument);
    }

    vss::endpoint_id echo = 0;
    if (plan.kind == job::port && plan.echo)
    {
        if (previous != nullptr && previous->echo != 0)
        {
            echo = previous->echo;
        }
        else
        {
            const auto mock = backend.create_mock_port(
                unique_name(QStringLiteral("echo"), taken), plan.serial, {});
            if (!mock)
            {
                undo_made();
                return mock.error();
            }
            made.push_back(*mock);
            echo = *mock;
            const auto echoing = backend.set_echo(echo, true);
            if (!echoing)
            {
                undo_made();
                return echoing.error();
            }
        }
    }

    const auto& a = *first;
    const auto& b = *second;

    // The ports of the plan stay; the ones the setup had before and the plan
    // leaves out close.
    const auto close_dropped = [&]
    {
        if (previous == nullptr)
            return;
        QSet<vss::endpoint_id> keep(a.begin(), a.end());
        for (const auto id : b)
            keep.insert(id);
        if (echo != 0)
            keep.insert(echo);
        for (const auto id : previous->group.ports)
        {
            if (!keep.contains(id))
                static_cast<void>(backend.remove_port(id));
        }
    };

    // Programs added to or taken off a device that several programs use change
    // its link in place, so the link and the other programs are not
    // disturbed.  The other setups have no in-place edit in the engine (a
    // join's sources, a bus's members), and are made again.
    if (previous != nullptr && previous->link != 0)
    {
        const bool turns = plan.kind == job::turns;
        const auto& programs = turns ? a : b;
        const vss::endpoint_id device =
            turns ? (b.empty() ? vss::endpoint_id{0} : b.front()) : a.front();
        const auto old = backend.link(previous->link);
        if (old && !programs.empty() && device != 0 &&
            edits_in_place(*old, plan.kind, device))
        {
            const auto edited = edit_targets(backend, *old, programs);
            if (!edited)
            {
                undo_made();
                return edited.error();
            }
            // Taking the owner off the link released it; a link whose owner
            // stays keeps it.
            if (turns)
            {
                const auto now = backend.link(old->id);
                if (now && now->owner == 0)
                    static_cast<void>(backend.set_owner(old->id, a[0]));
            }
            close_dropped();
            return a[0];
        }
    }

    std::optional<link_view> old_link;
    if (previous != nullptr && previous->link != 0)
    {
        old_link = backend.link(previous->link);
        const auto gone = backend.disconnect(previous->link);
        if (!gone)
        {
            undo_made();
            return gone.error();
        }
    }

    vss::result<vss::link_id> linked = vss::link_id{0};
    switch (plan.kind)
    {
    case job::port:
        if (echo != 0)
            linked = backend.connect_ports(a[0], echo, {});
        break;
    case job::pair:
        if (b.empty())
            linked = vss::make_error_code(vss::errc::invalid_argument);
        else
            linked = backend.connect_ports(a[0], b[0], {});
        break;
    case job::attach:
    case job::share:
        linked = backend.split(a[0], b, {});
        break;
    case job::bus:
        linked = backend.bus(a);
        break;
    case job::combine:
        if (b.empty())
            linked = vss::make_error_code(vss::errc::invalid_argument);
        else
            linked = backend.join(a, b[0], {});
        break;
    case job::turns:
    {
        if (b.empty())
        {
            linked = vss::make_error_code(vss::errc::invalid_argument);
            break;
        }
        // A split from the device with the owner policy: the programs send
        // to the device one at a time, and the device's replies reach every
        // program (08-engine-gaps.md).
        vss::link_options options;
        options.policy = vss::merge_policy::owner;
        linked = backend.split(b[0], a, options);
        if (linked)
        {
            vss::endpoint_id owner = a[0];
            if (previous != nullptr && previous->owner != 0 &&
                std::find(a.begin(), a.end(), previous->owner) != a.end())
                owner = previous->owner;
            static_cast<void>(backend.set_owner(*linked, owner));
        }
        break;
    }
    case job::talk:
    case job::network:
    case job::custom:
        break;
    }
    if (!linked)
    {
        undo_made();
        if (old_link)
            static_cast<void>(relink(backend, *old_link));
        return linked.error();
    }

    close_dropped();
    return a[0];
}

} // namespace vss::gui::simple
