//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/setup_kinds.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace vss::gui::simple
{

namespace
{

/// The literals are marked with QT_TRANSLATE_NOOP at each call, so that lupdate
/// finds them; this looks the marked text up in the same context.
QString tr_simple(const char* text)
{
    return QCoreApplication::translate("vss::gui::simple::SetupKinds", text);
}

const port_view* find(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    const auto it =
        std::find_if(ports.begin(), ports.end(),
                     [id](const port_view& port) { return port.id == id; });
    return it == ports.end() ? nullptr : &*it;
}

vss::endpoint_kind kind_of(const std::vector<port_view>& ports,
                           vss::endpoint_id id)
{
    const port_view* port = find(ports, id);
    return port != nullptr ? port->kind : vss::endpoint_kind::mock;
}

bool is_program(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    return kind_of(ports, id) == vss::endpoint_kind::virtual_port;
}

bool is_device(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    switch (kind_of(ports, id))
    {
    case vss::endpoint_kind::physical:
    case vss::endpoint_kind::tcp:
    case vss::endpoint_kind::udp:
    case vss::endpoint_kind::rfc2217:
        return true;
    default:
        return false;
    }
}

bool is_network(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    const auto kind = kind_of(ports, id);
    return kind == vss::endpoint_kind::tcp || kind == vss::endpoint_kind::udp ||
           kind == vss::endpoint_kind::rfc2217;
}

bool all_programs(const std::vector<port_view>& ports,
                  const QVector<vss::endpoint_id>& ids)
{
    return !ids.isEmpty() &&
           std::all_of(ids.begin(), ids.end(), [&ports](vss::endpoint_id id)
                       { return is_program(ports, id); });
}

bool all_programs_or_devices(const std::vector<port_view>& ports,
                             const QVector<vss::endpoint_id>& ids)
{
    return !ids.isEmpty() &&
           std::all_of(
               ids.begin(), ids.end(), [&ports](vss::endpoint_id id)
               { return is_program(ports, id) || is_device(ports, id); });
}

/// One device shared with several programs, or attached to a single one; with
/// the owner policy, programs taking turns sending to the device while all of
/// them get its replies.
void describe_split(setup_view& view, const link_view& link,
                    const std::vector<port_view>& ports, bool duplex)
{
    if (!duplex || link.sources.size() != 1 ||
        !is_device(ports, link.sources.front()) ||
        !all_programs(ports, link.targets))
        return;
    view.device = link.sources.front();
    view.programs = link.targets;
    if (link.options.policy == vss::merge_policy::owner)
    {
        view.kind = job::turns;
        view.owner = link.owner;
        return;
    }
    view.kind = link.targets.size() == 1 ? job::attach : job::share;
}

/// A link between two ports: a pair of programs, a device and a program, a
/// program and a test device, or a bridge between a device and a network port.
void describe_two_way(setup_view& view, const link_view& link,
                      const std::vector<port_view>& ports, bool duplex)
{
    if (link.sources.size() != 1 || link.targets.size() != 1)
        return;
    const auto a = link.sources.front();
    const auto b = link.targets.front();
    if (is_program(ports, a) && is_program(ports, b))
    {
        view.kind = job::pair;
        view.programs = {a, b};
        return;
    }
    // The real engine reports a duplex link with one target as a two-way link,
    // so a device and a single program are read as a split of one.
    if (duplex && is_device(ports, a) && is_program(ports, b))
    {
        describe_split(view, link, ports, duplex);
        return;
    }
    const auto mock_side =
        kind_of(ports, a) == vss::endpoint_kind::mock
            ? a
            : (kind_of(ports, b) == vss::endpoint_kind::mock ? b : 0);
    if (mock_side != 0)
    {
        const auto other = mock_side == a ? b : a;
        if (is_program(ports, other))
        {
            view.kind = job::port;
            view.programs = {other};
            view.echo = mock_side;
        }
        return;
    }
    // A bridge: a device on this PC and a network port other computers
    // connect to.
    const auto device =
        kind_of(ports, a) == vss::endpoint_kind::physical
            ? a
            : (kind_of(ports, b) == vss::endpoint_kind::physical ? b : 0);
    if (device != 0)
    {
        const auto other = device == a ? b : a;
        if (is_network(ports, other))
        {
            view.kind = job::network;
            view.device = device;
            view.listener = other;
        }
    }
}

/// Several ports sending to one, combined into one receiver.  A join with the
/// owner policy is not something Simple mode builds (its replies do not come
/// back), so it is left as made in Advanced mode.
void describe_join(setup_view& view, const link_view& link,
                   const std::vector<port_view>& ports)
{
    if (link.targets.size() != 1 ||
        !all_programs_or_devices(ports, link.sources) ||
        !all_programs_or_devices(ports, link.targets))
        return;
    if (link.options.policy == vss::merge_policy::owner)
        return;
    view.kind = job::combine;
    view.members = link.sources;
    view.receiver = link.targets.front();
    for (const auto id : link.sources)
    {
        if (is_program(ports, id))
            view.programs.push_back(id);
    }
    if (is_program(ports, view.receiver))
        view.programs.push_back(view.receiver);
}

/// A bus: every member hears every other.
void describe_bus(setup_view& view, const link_view& link,
                  const std::vector<port_view>& ports)
{
    if (!all_programs_or_devices(ports, link.sources))
        return;
    view.kind = job::bus;
    view.members = link.sources;
    for (const auto id : link.sources)
    {
        if (is_program(ports, id))
            view.programs.push_back(id);
    }
}

void describe_one_link(setup_view& view, const link_view& link,
                       const std::vector<port_view>& ports)
{
    view.link = link.id;
    const bool duplex = link.mode == vss::link_mode::duplex;
    switch (link.shape)
    {
    case link_shape::two_way:
        describe_two_way(view, link, ports, duplex);
        return;
    case link_shape::split:
        describe_split(view, link, ports, duplex);
        return;
    case link_shape::join:
        describe_join(view, link, ports);
        return;
    case link_shape::bus:
        describe_bus(view, link, ports);
        return;
    default:
        return;
    }
}

} // namespace

setup_view describe_setup(const setup_group& group,
                          const std::vector<port_view>& ports,
                          const std::vector<link_view>& links)
{
    VSS_PROFILE_ZONE("gui.simple.setup_kinds.describe");
    setup_view view;
    view.group = group;
    if (group.ports.size() == 1 && group.links.isEmpty())
    {
        const auto id = group.ports.front();
        if (is_program(ports, id))
        {
            view.kind = job::port;
            view.programs = {id};
        }
        else if (kind_of(ports, id) == vss::endpoint_kind::physical ||
                 is_network(ports, id))
        {
            view.kind = job::talk;
            view.device = id;
        }
        return view;
    }
    if (group.links.size() != 1)
        return view;
    const auto it =
        std::find_if(links.begin(), links.end(), [&group](const link_view& link)
                     { return link.id == group.links.front(); });
    if (it != links.end())
    {
        describe_one_link(view, *it, ports);
        // Anything the link does not account for makes the setup custom.
        QSet<vss::endpoint_id> covered(it->sources.begin(), it->sources.end());
        for (const auto id : it->targets)
            covered.insert(id);
        if (covered.size() != group.ports.size())
            view.kind = job::custom;
    }
    if (view.kind == job::custom)
    {
        view.device = 0;
        view.programs.clear();
        view.members.clear();
        view.receiver = 0;
        view.echo = 0;
        view.listener = 0;
        view.owner = 0;
    }
    return view;
}

std::vector<setup_view> list_setups(const std::vector<port_view>& ports,
                                    const std::vector<link_view>& links)
{
    VSS_PROFILE_ZONE("gui.simple.setup_kinds.list");
    std::vector<setup_view> result;
    QSet<vss::endpoint_id> seen;
    for (const auto& port : ports)
    {
        if (seen.contains(port.id))
            continue;
        const setup_group group = find_setup(ports, links, port.id);
        for (const auto id : group.ports)
            seen.insert(id);
        if (!group.empty())
            result.push_back(describe_setup(group, ports, links));
    }
    return result;
}

QString program_label(const port_view& port)
{
    static const QRegularExpression made_up(QStringLiteral(
        "^(program|port|listener|pair|device|share|bus|sender|receiver|"
        "watch|monitor|fixture|echo)(-[a-z0-9]+)*$"));
    if (port.name.isEmpty() || made_up.match(port.name).hasMatch())
        return {};
    if (!port.path.isEmpty() && port.name == port.path)
        return {};
    return port.name;
}

QString port_shown_name(const port_view& port)
{
    QString path = port.path;
    // A device opened by address reads "serial://COM3?baud=115200".
    if (path.startsWith(QStringLiteral("serial://")))
        path = path.mid(9).section(QLatin1Char('?'), 0, 0);
    return path.isEmpty() ? port.name : path;
}

QString job_title(job kind)
{
    switch (kind)
    {
    case job::port:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Give my program a serial port"));
    case job::pair:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Connect two programs to each other"));
    case job::attach:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Use my device through a new port"));
    case job::share:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Share my device with several programs"));
    case job::talk:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Try my device from this window"));
    case job::network:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Let other computers use my device"));
    case job::bus:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Put several on one line"));
    case job::combine:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Send several into one"));
    case job::turns:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Let programs take turns sending to a device"));
    case job::custom:
        break;
    }
    return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                       "Made in Advanced mode"));
}

QString job_description(job kind)
{
    switch (kind)
    {
    case job::port:
        return tr_simple(QT_TRANSLATE_NOOP(
            "vss::gui::simple::SetupKinds",
            "For a program that needs a COM port when there is "
            "no device, or before the device arrives."));
    case job::pair:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Like a null-modem cable between two programs on "
                              "this PC. What one sends, the other receives."));
    case job::attach:
        return tr_simple(QT_TRANSLATE_NOOP(
            "vss::gui::simple::SetupKinds",
            "Your program opens a new port instead of the "
            "device. Then you can watch, share or test without "
            "changing the program."));
    case job::share:
        return tr_simple(QT_TRANSLATE_NOOP(
            "vss::gui::simple::SetupKinds",
            "A device can normally be open in one program at a "
            "time. Here each program gets its own port, and all "
            "of them talk to the device."));
    case job::talk:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Type to the device and read its replies here, "
                              "without another program. Use it to check that "
                              "the device works."));
    case job::network:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Other computers connect to this PC over the "
                              "network and talk to the device."));
    case job::bus:
        return tr_simple(QT_TRANSLATE_NOOP(
            "vss::gui::simple::SetupKinds",
            "Like an RS-485 line. Everyone hears what the others "
            "send, and nobody hears themself."));
    case job::combine:
        return tr_simple(
            QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                              "Everything the senders write arrives at one "
                              "receiver, in the order it arrives. Nothing goes "
                              "back."));
    case job::turns:
        return tr_simple(QT_TRANSLATE_NOOP(
            "vss::gui::simple::SetupKinds",
            "Several programs send to one device, but only one "
            "at a time, so their bytes never mix. You choose "
            "which. All of them get the device's replies."));
    case job::custom:
        break;
    }
    return tr_simple(QT_TRANSLATE_NOOP(
        "vss::gui::simple::SetupKinds",
        "Made in Advanced mode. You can use it here, and change "
        "it in Advanced mode."));
}

QStringList program_names(const setup_view& setup,
                          const std::vector<port_view>& ports)
{
    QStringList names;
    for (const auto id : setup.programs)
    {
        if (const port_view* port = find(ports, id))
            names << port_shown_name(*port);
    }
    return names;
}

QString setup_title(const setup_view& setup,
                    const std::vector<port_view>& ports)
{
    const auto name = [&ports](vss::endpoint_id id)
    {
        const port_view* port = find(ports, id);
        return port != nullptr ? port_shown_name(*port) : QString();
    };
    const auto program_count = static_cast<int>(setup.programs.size());
    switch (setup.kind)
    {
    case job::port:
    {
        const port_view* port =
            setup.programs.isEmpty() ? nullptr : find(ports, setup.programs[0]);
        if (port == nullptr)
            return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                               "Program port"));
        const QString label = program_label(*port);
        return label.isEmpty()
                   ? tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                                 "Program port %1"))
                         .arg(port_shown_name(*port))
                   : tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                                 "%1 port %2"))
                         .arg(label, port_shown_name(*port));
    }
    case job::pair:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "%1 linked to %2"))
            .arg(name(setup.programs.value(0)), name(setup.programs.value(1)));
    case job::attach:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "%1 through %2"))
            .arg(name(setup.device), name(setup.programs.value(0)));
    case job::share:
        return QCoreApplication::translate("vss::gui::simple::SetupKinds",
                                           "%1 shared with %n programs",
                                           nullptr, program_count)
            .arg(name(setup.device));
    case job::talk:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "Trying %1"))
            .arg(name(setup.device));
    case job::network:
        return tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                           "%1 on the network"))
            .arg(name(setup.device));
    case job::bus:
        return QCoreApplication::translate(
            "vss::gui::simple::SetupKinds", "%n on one line", nullptr,
            static_cast<int>(setup.members.size()));
    case job::combine:
        return QCoreApplication::translate(
                   "vss::gui::simple::SetupKinds", "%n senders into %1",
                   nullptr, static_cast<int>(setup.members.size()))
            .arg(name(setup.receiver));
    case job::turns:
        return QCoreApplication::translate(
                   "vss::gui::simple::SetupKinds",
                   "%n programs take turns sending to %1", nullptr,
                   program_count)
            .arg(name(setup.device));
    case job::custom:
    default:
    {
        QStringList names;
        for (const auto id : setup.group.ports)
            names << name(id);
        return names.size() <= 3
                   ? names.join(QStringLiteral(", "))
                   : tr_simple(QT_TRANSLATE_NOOP("vss::gui::simple::SetupKinds",
                                                 "%1 and %2 more"))
                         .arg(names.mid(0, 2).join(QStringLiteral(", ")))
                         .arg(names.size() - 2);
    }
    }
}

} // namespace vss::gui::simple
