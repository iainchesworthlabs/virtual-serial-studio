//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/topology_text.hpp"
#include "language/bidi.hpp"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>

namespace vss::gui::topology_text
{
namespace
{

QString port_name(const QVector<vss::endpoint_id>& ids,
                  const QHash<vss::endpoint_id, QString>& names)
{
    QStringList list;
    for (const vss::endpoint_id id : ids)
    {
        const QString name = isolate_name(names.value(id));
        if (!name.isEmpty())
            list.append(name);
    }
    return join_names(list);
}

QString display_path(const port_view& port)
{
    const QString scheme = QStringLiteral("serial://");
    if (port.path.startsWith(scheme, Qt::CaseInsensitive))
        return port.path.mid(scheme.size());
    return port.path;
}

QString with_states(const QString& base, const link_view& link)
{
    QStringList states;
    if (link.mode == vss::link_mode::duplex &&
        (link.shape == link_shape::split || link.shape == link_shape::join))
    {
        states.append(QCoreApplication::translate("Topology", "two-way"));
    }
    if (link.options.policy == vss::merge_policy::owner)
        states.append(QCoreApplication::translate("Topology", "owner"));

    if (states.size() == 1)
        return QCoreApplication::translate("Topology", "%1, %2")
            .arg(base, states.first());
    if (states.size() == 2)
        return QCoreApplication::translate("Topology", "%1, %2, %3")
            .arg(base, states.at(0), states.at(1));
    return base;
}

} // namespace

QString view_name()
{
    return QCoreApplication::translate("Topology", "Topology");
}

QString links_list_name()
{
    return QCoreApplication::translate("Topology", "Links");
}

QString empty_state()
{
    return QCoreApplication::translate("Topology",
                                       "Add a port to see it here.");
}

QString hint()
{
    return QCoreApplication::translate(
        "Topology",
        "Click a box to inspect it. Click a line to select the link.");
}

QString loop_note()
{
    return QCoreApplication::translate(
        "Topology",
        "That link would create a loop. Data would travel from device to "
        "terminal and back to device forever. Nothing was changed. "
        "Remove a link first, or choose different ports.");
}

QString show_loop()
{
    return QCoreApplication::translate("Topology", "Show the loop");
}

QString owner_badge()
{
    return QCoreApplication::translate("Topology", "Owner");
}

QString dropped_badge()
{
    return QCoreApplication::translate("Topology", "Dropped");
}

QString unlinked_badge()
{
    return QCoreApplication::translate("Topology", "not linked");
}

QString no_links()
{
    return QCoreApplication::translate("Topology", "No links yet.");
}

QString inspect_action()
{
    return QCoreApplication::translate("Topology", "Inspect");
}

QString copy_name_action()
{
    return QCoreApplication::translate("Topology", "Copy name");
}

QString rename_action()
{
    return QCoreApplication::translate("Topology", "Rename");
}

QString connect_from_here_action()
{
    return QCoreApplication::translate("Topology", "Connect from here");
}

QString connect_needs_two()
{
    return QCoreApplication::translate(
        "Topology", "Connect ports needs at least two ports.");
}

QString remove_action()
{
    return QCoreApplication::translate("Topology", "Remove");
}

QString remove_link_action()
{
    return QCoreApplication::translate("Topology", "Remove link");
}

QString tidy_layout_action()
{
    return QCoreApplication::translate("Topology", "Tidy layout");
}

QString fit_action()
{
    return QCoreApplication::translate("Topology", "Fit");
}

QString layout_tidied()
{
    return QCoreApplication::translate("Topology", "Layout tidied.");
}

QString port_count(int count)
{
    return count == 1
               ? QCoreApplication::translate("Topology", "1 port")
               : QCoreApplication::translate("Topology", "%1 ports").arg(count);
}

QString link_count(int count)
{
    return count == 1
               ? QCoreApplication::translate("Topology", "1 link")
               : QCoreApplication::translate("Topology", "%1 links").arg(count);
}

QString join_names(const QStringList& names)
{
    if (names.isEmpty())
        return {};
    if (names.size() == 1)
        return names.first();
    if (names.size() == 2)
        return QCoreApplication::translate("Topology", "%1 and %2")
            .arg(names.at(0), names.at(1));
    return QCoreApplication::translate("Topology", "%1 and %2")
        .arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")),
             names.last());
}

QString node_detail(const port_view& port)
{
    switch (port.kind)
    {
    case vss::endpoint_kind::physical:
        return QCoreApplication::translate("Topology", "Real device, %1")
            .arg(display_path(port));
    case vss::endpoint_kind::mock:
        return QCoreApplication::translate("Topology", "Fixture (mock)");
    case vss::endpoint_kind::virtual_port:
    case vss::endpoint_kind::tcp:
    case vss::endpoint_kind::udp:
    case vss::endpoint_kind::rfc2217:
        break;
    }
    if (port.studio_only)
        return QCoreApplication::translate("Topology", "Studio only");
    return port.path;
}

bool node_detail_is_mono(const port_view& port)
{
    return port.kind != vss::endpoint_kind::physical &&
           port.kind != vss::endpoint_kind::mock && !port.studio_only;
}

QString node_tooltip(const port_view& port, bool unlinked)
{
    QString text = port.name;
    const QString detail = node_detail(port);
    if (!detail.isEmpty())
        text += QLatin1Char('\n') + detail;
    if (unlinked)
        text += QLatin1Char('\n') +
                QCoreApplication::translate("Topology",
                                            "Not linked to another port.");
    return text;
}

QString link_primary_label(link_shape shape)
{
    switch (shape)
    {
    case link_shape::split:
        return QCoreApplication::translate("Topology", "Split");
    case link_shape::join:
        return QCoreApplication::translate("Topology", "Join");
    case link_shape::two_way:
        return QCoreApplication::translate("Topology", "two-way");
    case link_shape::monitor:
        return QCoreApplication::translate("Topology", "monitor");
    case link_shape::bus:
        return QCoreApplication::translate("Topology", "Bus");
    case link_shape::one_way:
        break;
    }
    return {};
}

QString link_secondary_label(const link_view& link)
{
    if (link.options.policy == vss::merge_policy::owner)
        return QCoreApplication::translate("Topology", "owner");
    if (link.mode == vss::link_mode::duplex &&
        (link.shape == link_shape::split || link.shape == link_shape::join))
    {
        return QCoreApplication::translate("Topology", "two-way");
    }
    return {};
}

QString summary(const std::vector<port_view>& ports,
                const std::vector<link_view>& links)
{
    if (ports.empty())
        return QCoreApplication::translate(
            "Topology", "No ports yet. Add a port to see it here.");

    QHash<vss::endpoint_id, QString> names;
    for (const auto& port : ports)
        names.insert(port.id, port.name);

    const QString ports_text = port_count(static_cast<int>(ports.size()));
    if (links.empty())
        return QCoreApplication::translate("Topology", "%1, no links")
            .arg(ports_text);

    QStringList phrases;
    for (const auto& link : links)
        phrases.append(link_phrase(link, names));
    return QCoreApplication::translate("Topology", "%1, %2: %3")
        .arg(ports_text, link_count(static_cast<int>(links.size())),
             phrases.join(QStringLiteral("; ")));
}

QString link_name(const link_view& link,
                  const QHash<vss::endpoint_id, QString>& names)
{
    const QString source = port_name(link.sources, names);
    const QString target = port_name(link.targets, names);

    QString base;
    switch (link.shape)
    {
    case link_shape::split:
        if (link.targets.size() > 1)
        {
            base = QCoreApplication::translate("Topology",
                                               "Split link, %1 to %2 ports")
                       .arg(source)
                       .arg(link.targets.size());
        }
        else
        {
            base =
                QCoreApplication::translate("Topology", "Split link, %1 to %2")
                    .arg(source, target);
        }
        break;
    case link_shape::join:
        if (link.sources.size() > 1)
        {
            base = QCoreApplication::translate("Topology",
                                               "Join link, %1 ports into %2")
                       .arg(link.sources.size())
                       .arg(target);
        }
        else
        {
            base =
                QCoreApplication::translate("Topology", "Join link, %1 into %2")
                    .arg(source, target);
        }
        break;
    case link_shape::two_way:
        base = QCoreApplication::translate("Topology", "Two-way link, %1 to %2")
                   .arg(source, target);
        break;
    case link_shape::one_way:
        base = QCoreApplication::translate("Topology", "One-way link, %1 to %2")
                   .arg(source, target);
        break;
    case link_shape::monitor:
        base = QCoreApplication::translate("Topology", "Monitor link, %1 to %2")
                   .arg(source, target);
        break;
    case link_shape::bus:
        base = QCoreApplication::translate("Topology", "Bus link, %1 ports")
                   .arg(link.sources.size());
        break;
    }
    return with_states(base, link);
}

QString link_phrase(const link_view& link,
                    const QHash<vss::endpoint_id, QString>& names)
{
    const QString source = port_name(link.sources, names);
    const QString target = port_name(link.targets, names);

    switch (link.shape)
    {
    case link_shape::split:
        return QCoreApplication::translate("Topology", "split from %1 to %2")
            .arg(source, target);
    case link_shape::join:
        return QCoreApplication::translate("Topology", "join from %1 into %2")
            .arg(source, target);
    case link_shape::two_way:
        return QCoreApplication::translate("Topology",
                                           "two-way between %1 and %2")
            .arg(source, target);
    case link_shape::one_way:
        return QCoreApplication::translate("Topology", "one-way from %1 to %2")
            .arg(source, target);
    case link_shape::monitor:
        return QCoreApplication::translate("Topology", "monitor from %1 to %2")
            .arg(source, target);
    case link_shape::bus:
        return QCoreApplication::translate("Topology", "bus of %1").arg(source);
    }
    return {};
}

QString copied(const QString& name)
{
    return QCoreApplication::translate("Topology", "Copied %1.").arg(name);
}

QString selected(const QString& name)
{
    return QCoreApplication::translate("Topology", "Selected %1.").arg(name);
}

} // namespace vss::gui::topology_text
