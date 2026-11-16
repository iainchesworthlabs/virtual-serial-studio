//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/job_plan.hpp"

#include "backend/driver_error.hpp"
#include "dialogs/network_uri.hpp"

#include <vss/core/link.hpp>
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
        // The path of a network port reads scheme://host:port, with an IPv6
        // host in brackets.
        const auto parsed =
            parse_network_uri(port->path.contains(QStringLiteral("://"))
                                  ? port->path
                                  : QStringLiteral("tcp://") + port->path);
        if (parsed)
        {
            end.address = parsed->host;
            end.network_port = static_cast<unsigned>(parsed->port);
        }
        else
        {
            const QString where = port->path.section(QStringLiteral("://"), -1);
            end.address = where.section(QLatin1Char(':'), 0, 0);
            end.network_port = where.section(QLatin1Char(':'), 1, 1)
                                   .section(QLatin1Char('?'), 0, 0)
                                   .toUInt();
        }
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
        return backend.bus(sources, link.options);
    default:
        break;
    }
    return vss::make_error_code(vss::errc::unsupported);
}

/// Which list of a link an in-place edit changes.
enum class edit_list
{
    /// The targets of a duplex link (`add_target`, `remove_target`).
    targets,
    /// The sources of a one-way link (`add_source`, `remove_source`).
    sources,
    /// The members of a bus (`add_member`, `remove_member`).
    members
};

/// What `build_setup` does to a link without replacing it: the list it
/// changes, and what that list is to hold afterwards.
struct in_place_edit
{
    edit_list list = edit_list::targets;
    std::vector<vss::endpoint_id> wanted;
};

/// Whether `old` can be changed into what `kind` builds by adding and taking
/// off the ends of one of its lists, and if so how.  `first` and `second` are
/// the plan's ends, made or kept.
///
/// - attach, share, turns: a duplex link from the same device with the merge
///   policy the kind needs, whose targets become the programs.  The shape is
///   not asked: the real engine reports a duplex link with one target as
///   `two_way`, and with more as `split`.
/// - combine: a one-way link into the same receiver, with the interleave
///   policy, whose sources become the senders.
/// - bus: a bus, whose members become the plan's.  Every new member is on the
///   bus before an old one leaves, so a plan that swaps most of a large bus
///   is made again instead of going over the most a bus takes.
std::optional<in_place_edit>
edits_in_place(const link_view& old, job kind,
               const std::vector<vss::endpoint_id>& first,
               const std::vector<vss::endpoint_id>& second)
{
    switch (kind)
    {
    case job::attach:
    case job::share:
    case job::turns:
    {
        const bool turns = kind == job::turns;
        const auto& programs = turns ? first : second;
        const vss::endpoint_id device =
            turns ? (second.empty() ? vss::endpoint_id{0} : second.front())
                  : first.front();
        const vss::merge_policy wanted =
            turns ? vss::merge_policy::owner : vss::merge_policy::interleave;
        if (!programs.empty() && device != 0 &&
            old.mode == vss::link_mode::duplex && old.sources.size() == 1 &&
            old.sources.front() == device && old.options.policy == wanted)
            return in_place_edit{edit_list::targets, programs};
        break;
    }
    case job::combine:
        if (!first.empty() && !second.empty() &&
            old.mode == vss::link_mode::one_way && old.targets.size() == 1 &&
            old.targets.front() == second.front() &&
            old.options.policy == vss::merge_policy::interleave)
            return in_place_edit{edit_list::sources, first};
        break;
    case job::bus:
    {
        if (old.mode != vss::link_mode::bus)
            break;
        std::size_t newcomers = 0;
        for (const auto id : first)
        {
            if (!old.sources.contains(id))
                ++newcomers;
        }
        if (static_cast<std::size_t>(old.sources.size()) + newcomers <=
            vss::max_bus_members)
            return in_place_edit{edit_list::members, first};
        break;
    }
    default:
        break;
    }
    return std::nullopt;
}

/// Makes the ends of one list of `old` the `wanted` ones: adds the new ones
/// first, so the link always has an end of that kind, then takes off the ones
/// that are not wanted.  The link keeps its id, options and counters.  A
/// failure puts the list back, so the link is as it was (an end that was
/// taken off and is put back goes to the end of the list).
vss::result<void> edit_ends(StudioBackend& backend, const link_view& old,
                            const in_place_edit& edit)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.edit_ends");
    const QVector<vss::endpoint_id>& current =
        edit.list == edit_list::targets ? old.targets : old.sources;
    const auto add = [&](vss::endpoint_id id)
    {
        switch (edit.list)
        {
        case edit_list::targets:
            return backend.add_target(old.id, id);
        case edit_list::sources:
            return backend.add_source(old.id, id);
        case edit_list::members:
            return backend.add_member(old.id, id);
        }
        return vss::result<void>{vss::make_error_code(vss::errc::unsupported)};
    };
    const auto take = [&](vss::endpoint_id id)
    {
        switch (edit.list)
        {
        case edit_list::targets:
            return backend.remove_target(old.id, id);
        case edit_list::sources:
            return backend.remove_source(old.id, id);
        case edit_list::members:
            return backend.remove_member(old.id, id);
        }
        return vss::result<void>{vss::make_error_code(vss::errc::unsupported)};
    };

    std::vector<vss::endpoint_id> added;
    std::vector<vss::endpoint_id> removed;
    const auto roll_back = [&]
    {
        for (const auto id : removed)
            static_cast<void>(add(id));
        for (auto it = added.rbegin(); it != added.rend(); ++it)
            static_cast<void>(take(*it));
        // Taking the owner off the link released it.
        if (old.owner != 0 && std::find(removed.begin(), removed.end(),
                                        old.owner) != removed.end())
            static_cast<void>(backend.set_owner(old.id, old.owner));
    };

    for (const auto id : edit.wanted)
    {
        if (current.contains(id) ||
            std::find(added.begin(), added.end(), id) != added.end())
            continue;
        const auto result = add(id);
        if (!result)
        {
            roll_back();
            return result.error();
        }
        added.push_back(id);
    }
    for (const auto id : current)
    {
        if (std::find(edit.wanted.begin(), edit.wanted.end(), id) !=
            edit.wanted.end())
            continue;
        const auto result = take(id);
        if (!result)
        {
            roll_back();
            return result.error();
        }
        removed.push_back(id);
    }
    return {};
}

/// The name a device end's port starts from, before `unique_name`.
QString device_name_base(const end_plan& end)
{
    const QString path =
        end.what == end_plan::kind::device ? end.device_path : end.address;
    QString base = QStringLiteral("device-%1").arg(path).toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                 QStringLiteral("-"));
    while (base.endsWith(QLatin1Char('-')))
        base.chop(1);
    return base;
}

QSet<QString> port_names(const StudioBackend& backend)
{
    QSet<QString> names;
    for (const auto& port : backend.ports())
        names.insert(port.name);
    return names;
}

/// Opens the device of a "let other computers use my device" plan, opens the
/// network port other computers connect to, and links them.  The device keeps
/// every setting of the plan.  The ports made are removed again when a step
/// fails.  Returns the device's port.
vss::result<vss::endpoint_id> open_network_job(StudioBackend& backend,
                                               const job_plan& plan)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.open_network_job");
    if (plan.first.empty())
        return vss::make_error_code(vss::errc::invalid_argument);
    QSet<QString> taken = port_names(backend);
    const end_plan& end = plan.first.front();

    std::vector<vss::endpoint_id> made;
    const auto undo = [&backend, &made]
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
            static_cast<void>(backend.remove_port(*it));
    };

    const QString device_name = unique_name(device_name_base(end), taken);
    taken.insert(device_name);
    const auto device = backend.attach_serial(
        device_name, device_uri(end, plan.serial), plan.serial, true);
    if (!device)
        return device.error();
    made.push_back(*device);

    // `tcp://` would connect out to the address: only `tcp-listen://` waits
    // for other computers.
    const QString listen_name =
        unique_name(QStringLiteral("network-%1").arg(plan.listen_port), taken);
    const auto listener = backend.attach_serial(
        listen_name,
        build_network_uri(network_transport::tcp, network_role::server,
                          QStringLiteral("0.0.0.0"),
                          static_cast<int>(plan.listen_port)),
        vss::serial_settings{}, true);
    if (!listener)
    {
        undo();
        return listener.error();
    }
    made.push_back(*listener);

    const auto linked = backend.connect_ports(*device, *listener, {});
    if (!linked)
    {
        undo();
        return linked.error();
    }
    return *device;
}

/// Makes the network setup of `plan`, or changes `previous` into it.  Both
/// the old and the new setup may want the same device and the same network
/// port, so the old one is closed first.  When the new one cannot be made,
/// the old one is made again from its plan; `previous_lost` is set when that
/// fails too.
vss::result<vss::endpoint_id> build_network(StudioBackend& backend,
                                            const job_plan& plan,
                                            const setup_view* previous,
                                            bool* previous_lost)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.build_network");
    if (plan.first.empty())
        return vss::make_error_code(vss::errc::invalid_argument);
    if (previous == nullptr)
        return open_network_job(backend, plan);

    const job_plan old_plan = plan_from(*previous, backend.ports());
    for (const auto id : previous->group.ports)
        static_cast<void>(backend.remove_port(id));
    const auto made = open_network_job(backend, plan);
    if (made)
        return made;
    const bool restorable =
        previous->kind == job::network && !old_plan.first.empty();
    if (!restorable || !open_network_job(backend, old_plan))
    {
        if (previous_lost != nullptr)
            *previous_lost = true;
    }
    return made.error();
}

} // namespace

std::optional<end_plan> resolved_network_end(const end_plan& end,
                                             QString* error)
{
    if (error != nullptr)
        error->clear();
    if (end.what != end_plan::kind::network_device)
        return end;
    const auto typed = parse_device_address(end.address, error);
    if (!typed)
        return std::nullopt;
    end_plan resolved = end;
    resolved.address = typed->host;
    if (typed->port)
        resolved.network_port = static_cast<unsigned>(*typed->port);
    if (typed->transport)
        resolved.rfc2217 = *typed->transport == network_transport::rfc2217;
    return resolved;
}

QString device_uri(const end_plan& end, const vss::serial_settings& serial)
{
    if (end.what == end_plan::kind::network_device)
    {
        // An address that cannot be read is passed on as typed: the form does
        // not let it get this far.
        const end_plan target = resolved_network_end(end).value_or(end);
        return build_network_uri(target.rfc2217 ? network_transport::rfc2217
                                                : network_transport::tcp,
                                 network_role::client, target.address,
                                 static_cast<int>(target.network_port));
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
            const int number = network_port_from_uri(listener->path);
            if (number > 0)
                plan.listen_port = static_cast<unsigned>(number);
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
    // The network setup is one engine operation that is made again: the
    // device and the network port close, so the computers connected lose
    // their connection, and a device and a network port open.
    if (plan.kind == job::network)
    {
        for (const auto id : previous.group.ports)
            changes.closed.push_back(id);
        changes.added = 2;
        return changes;
    }
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
                                          const setup_view* previous,
                                          unsigned* clashed_com,
                                          bool* previous_lost)
{
    VSS_PROFILE_ZONE("gui.simple.job_plan.build_setup");
    if (clashed_com != nullptr)
        *clashed_com = 0;
    if (previous_lost != nullptr)
        *previous_lost = false;

    // Letting other computers in opens the device and the network port, and a
    // change closes the old pair first.
    if (plan.kind == job::network)
        return build_network(backend, plan, previous, previous_lost);

    const auto before = backend.ports();
    QSet<QString> taken;
    for (const auto& port : before)
        taken.insert(port.name);

    // Device settings chosen in the form reach the devices that stay open
    // (a new device is opened with them).  Nothing is asked of a change that
    // leaves the settings as they were.
    std::vector<std::pair<vss::endpoint_id, vss::serial_settings>> to_change;
    if (previous != nullptr &&
        plan.serial != plan_from(*previous, before).serial)
    {
        for (const auto* ends : {&plan.first, &plan.second})
        {
            for (const auto& end : *ends)
            {
                // A device on this PC, or one over RFC 2217, which takes the
                // new speed.  The engine refuses plain TCP and UDP.
                const bool takes_settings =
                    end.what == end_plan::kind::device ||
                    (end.what == end_plan::kind::network_device && end.rfc2217);
                if (end.existing == 0 || !takes_settings)
                    continue;
                if (const auto port = find(before, end.existing);
                    port && port->serial != plan.serial)
                    to_change.emplace_back(end.existing, port->serial);
            }
        }
        if (!to_change.empty() && !backend.caps().port_settings)
            return vss::make_error_code(vss::errc::unsupported);
    }

    std::vector<vss::endpoint_id> made;
    std::vector<std::pair<vss::endpoint_id, vss::serial_settings>> changed;
    const auto undo_made = [&backend, &made, &changed]
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
            static_cast<void>(backend.remove_port(*it));
        made.clear();
        for (auto it = changed.rbegin(); it != changed.rend(); ++it)
            static_cast<void>(backend.set_port_settings(it->first, it->second));
        changed.clear();
    };

    // The ends are named in the order of the plan and made in three passes:
    // devices (and ends that stay) first, so a device that is busy is found
    // before Windows spends about 27 seconds installing a new port; then the
    // ports that chose a COM number; then the Automatic ones, which take the
    // lowest number that is free, which may be one a later end chose.
    enum class pass
    {
        device,
        chosen_number,
        automatic
    };
    struct end_slot
    {
        const end_plan* end = nullptr;
        QString name;
        vss::endpoint_id id = 0;
        pass when = pass::device;
    };
    std::vector<end_slot> slots;
    const std::size_t first_count = plan.first.size();
    for (const auto* ends : {&plan.first, &plan.second})
    {
        for (const auto& end : *ends)
        {
            end_slot slot;
            slot.end = &end;
            if (end.existing == 0)
            {
                if (end.what == end_plan::kind::program)
                {
                    slot.name = unique_name(end.label, taken);
                    slot.when =
                        end.com_number != 0 && backend.caps().com_numbers
                            ? pass::chosen_number
                            : pass::automatic;
                }
                else
                {
                    slot.name = unique_name(
                        device_name_base(
                            resolved_network_end(end).value_or(end)),
                        taken);
                }
                taken.insert(slot.name);
            }
            slots.push_back(std::move(slot));
        }
    }

    const auto make_end =
        [&](const end_slot& slot) -> vss::result<vss::endpoint_id>
    {
        const end_plan& end = *slot.end;
        if (end.existing != 0)
            return end.existing;
        if (end.what == end_plan::kind::program)
        {
            auto port = slot.when == pass::chosen_number
                            ? backend.create_driver_port(slot.name, true,
                                                         end.com_number)
                            : backend.create_auto_port(slot.name, true);
            if (!port)
            {
                if (clashed_com != nullptr && end.com_number != 0 &&
                    port.error() == vss::errc::already_exists)
                    *clashed_com = end.com_number;
                // A missing driver or helper looks like any other refusal;
                // say which, so the form can offer the set-up.
                return driver_aware_error(backend, port.error());
            }
            made.push_back(*port);
            return port;
        }
        auto port = backend.attach_serial(
            slot.name, device_uri(end, plan.serial), plan.serial, true);
        if (port)
            made.push_back(*port);
        return port;
    };

    for (const pass when : {pass::device, pass::chosen_number, pass::automatic})
    {
        for (auto& slot : slots)
        {
            if (slot.when != when)
                continue;
            const auto id = make_end(slot);
            if (!id)
            {
                undo_made();
                return id.error();
            }
            slot.id = *id;
        }
    }
    std::vector<vss::endpoint_id> first_ids;
    std::vector<vss::endpoint_id> second_ids;
    for (std::size_t index = 0; index < slots.size(); ++index)
        (index < first_count ? first_ids : second_ids)
            .push_back(slots[index].id);
    if (first_ids.empty())
    {
        undo_made();
        return vss::make_error_code(vss::errc::invalid_argument);
    }

    for (const auto& [id, settings] : to_change)
    {
        const auto result = backend.set_port_settings(id, plan.serial);
        if (!result)
        {
            undo_made();
            return result.error();
        }
        changed.emplace_back(id, settings);
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

    const auto& a = first_ids;
    const auto& b = second_ids;

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

    // Programs added to or taken off a device that several programs use, the
    // senders of a combine and the members of a bus change their link in place,
    // so the link and the ports that stay are not disturbed.  The other setups
    // have no in-place edit in the engine (a pair, a lone port with its echo)
    // or are one engine operation (the network bridge), and are made again.
    if (previous != nullptr && previous->link != 0)
    {
        const auto old = backend.link(previous->link);
        const auto edit = old && !a.empty()
                              ? edits_in_place(*old, plan.kind, a, b)
                              : std::nullopt;
        if (edit)
        {
            const auto edited = edit_ends(backend, *old, *edit);
            if (!edited)
            {
                undo_made();
                return edited.error();
            }
            // Taking the owner off the link released it; a link whose owner
            // stays keeps it.
            if (plan.kind == job::turns)
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
        linked = backend.bus(a, {});
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
        if (old_link && !relink(backend, *old_link) && previous_lost != nullptr)
            *previous_lost = true;
        return linked.error();
    }

    close_dropped();
    return a[0];
}

} // namespace vss::gui::simple
