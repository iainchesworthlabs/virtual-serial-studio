//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_TOPOLOGY_TOPOLOGY_TEXT_HPP
#define VSS_GUI_TOPOLOGY_TOPOLOGY_TEXT_HPP

#include "backend/view_types.hpp"

#include <vss/core/types.hpp>

#include <QHash>
#include <QString>
#include <QStringList>

#include <vector>

namespace vss::gui::topology_text
{

/// Every string the topology shows lives here, so a copy review is one diff.
/// The translation context is "Topology".

QString view_name();
QString links_list_name();
QString empty_state();
QString hint();
QString loop_note();
QString show_loop();
QString owner_badge();
QString dropped_badge();
QString unlinked_badge();
QString no_links();

QString inspect_action();
QString copy_name_action();
QString rename_action();
QString connect_from_here_action();
QString connect_needs_two();
QString remove_action();
QString stop_action();
QString start_action();
QString remove_link_action();
/// The canvas menu: Tidy layout and Fit.
QString tidy_layout_action();
QString fit_action();
QString layout_tidied();

QString port_count(int count);
QString link_count(int count);
/// "a", "a and b", "a, b and c".
QString join_names(const QStringList& names);

/// The second line of a node: the mono path, or a short role sentence.
QString node_detail(const port_view& port);
bool node_detail_is_mono(const port_view& port);
QString node_tooltip(const port_view& port, bool unlinked);

/// The visual label above a link: Split, Join, two-way, monitor.  A one-way
/// link has no label; the arrow head carries the direction.
QString link_primary_label(link_shape shape);
/// The label below a link: "owner" or "two-way", or empty.
QString link_secondary_label(const link_view& link);

/// The generated accessible description of the whole view, for example
/// "4 ports, 1 link: split from device to terminal, test-tool and logger".
QString summary(const std::vector<port_view>& ports,
                const std::vector<link_view>& links);

/// The accessible name of one link, for example
/// "Split link, device to 3 ports, two-way".
QString link_name(const link_view& link,
                  const QHash<vss::endpoint_id, QString>& names);
/// The summary phrase for one link: "split from device to terminal, ...".
QString link_phrase(const link_view& link,
                    const QHash<vss::endpoint_id, QString>& names);

QString copied(const QString& name);
QString selected(const QString& name);

} // namespace vss::gui::topology_text

#endif // VSS_GUI_TOPOLOGY_TOPOLOGY_TEXT_HPP
