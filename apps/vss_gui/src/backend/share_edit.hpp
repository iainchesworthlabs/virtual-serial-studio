//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "backend/view_types.hpp"

#include <QString>
#include <QVector>

#include <optional>
#include <vector>

namespace vss::gui
{

/// A real device (or other non-virtual endpoint) shared with programs through
/// the virtual ports on its duplex link.  An attached device is the same
/// thing with one program port, and a share is the same thing with many.
struct share_ref
{
    vss::link_id link = 0;
    vss::endpoint_id device = 0;
    QVector<vss::endpoint_id> programs;
};

/// The share a port belongs to, as the device or as one of the program
/// ports.  Empty for a pair of virtual ports, a port with no link, a bus, a
/// join or a monitor.
std::optional<share_ref> find_share(const std::vector<port_view>& ports,
                                    const std::vector<link_view>& links,
                                    vss::endpoint_id port);

/// The share a link is. Empty when the link is not one.
std::optional<share_ref> find_share_of_link(const std::vector<port_view>& ports,
                                            const std::vector<link_view>& links,
                                            vss::link_id link);

struct share_added
{
    vss::link_id link = 0;
    vss::endpoint_id port = 0;
};

/// A name for a new program port that no port uses: `program-N`.
QString next_program_name(const std::vector<port_view>& ports);

/// Adds one more program port to an existing share.
///
/// This creates the port and adds it as one more target of the share's link.
/// The link keeps its id, its options, its owner and its counters, and the
/// other programs lose no bytes.  When the port cannot be added to the link it
/// is removed again, so the setup is as it was.
vss::result<share_added> add_program_port(StudioBackend& backend,
                                          const share_ref& share,
                                          const QString& name, bool capture);

/// Removes one program port from a share and keeps the others shared.
///
/// The port is taken off the link first and then removed, so the link, its id
/// and the other programs are not disturbed and no bytes are lost.  Returns
/// the share's link, or 0 when the port was the only program and the link
/// went with it.  When the port cannot be taken off the link, or cannot be
/// removed, the setup is as it was.
vss::result<vss::link_id> remove_program_port(StudioBackend& backend,
                                              const share_ref& share,
                                              vss::endpoint_id program);

} // namespace vss::gui
