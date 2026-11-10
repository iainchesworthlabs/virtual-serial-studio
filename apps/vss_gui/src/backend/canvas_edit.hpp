//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/capabilities.hpp"
#include "backend/studio_backend.hpp"
#include "backend/view_types.hpp"

#include <vss/config/document.hpp>
#include <vss/core/serial_settings.hpp>

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <vector>

namespace vss::gui
{

/// What a toolbox tile on the Advanced canvas adds.  See
/// `docs/gui-design/docs/screens/07-canvas-editor.md`.
enum class canvas_item
{
    port,
    fixture,
    loopback,
    device,
    network,
    pair,
    split,
    join,
    bus,
    switch_link,
    redirect,
    monitor,
    share,
    bench
};

/// What fills one end of a bundle.
enum class slot_kind
{
    new_port,
    new_fixture,
    new_loopback,
    existing
};

struct bundle_slot
{
    slot_kind kind = slot_kind::new_port;
    /// The name of a new port, or of the existing port it refers to.
    QString name;

    friend bool operator==(const bundle_slot&, const bundle_slot&) = default;
};

/// A request to add a tile's ports and link in one step.
///
/// The slots are in the order `bundle_roles` names them.  For the bundles
/// with one hub end (split, join, switch, redirect, monitor) the first slot
/// is that end.  A bench is fixture and port slots in turn, one pair each.
/// Everything is by name, so the request can be made again for a redo.
struct bundle_request
{
    canvas_item item = canvas_item::pair;
    QVector<bundle_slot> slots;
    vss::serial_settings settings;
};

/// What `build_bundle` made.
struct bundle_made
{
    QStringList created;
    QVector<vss::link_id> links;
};

/// The tiles that are bundles the canvas builds itself (not Device, Network
/// or Share, which open their own dialogs).
bool is_bundle(canvas_item item);

/// True when the build can make this tile; false means the tile is shown
/// disabled with "Not available in this build".
bool item_available(const capabilities& caps, canvas_item item);

/// How many slots a bundle starts with, and its smallest and largest count.
/// For a bench the count is the number of fixture and port pairs.
int default_count(canvas_item item);
int minimum_count(canvas_item item);
int maximum_count(canvas_item item);
/// The number of slots for `count` (a bench has two per pair, the hub
/// bundles have one more than the count).
int slot_count(canvas_item item, int count);

/// The kind of port a slot starts as: New fixture for a bench's fixtures and
/// a Fixture tile, New loopback for a Loopback tile, otherwise New port.
slot_kind default_slot_kind(canvas_item item, int index);

/// A name no port uses and `taken` does not hold: `base-N`, counting from 1.
QString next_free_name(const std::vector<port_view>& ports, const QString& base,
                       const QStringList& taken = {});

/// Fills a request for a tile with fresh names, ready for the Add sheet.
bundle_request default_request(canvas_item item,
                               const std::vector<port_view>& ports,
                               int count = 0);

/// Makes every new port, then the link.  When any step fails the steps
/// already made are undone, so the setup is as it was.
vss::result<bundle_made> build_bundle(StudioBackend& backend,
                                      const bundle_request& request);

/// The link a canvas wire makes.
enum class wire_kind
{
    two_way,
    one_way,
    monitor
};

/// Links `from` to `to` as a wire chose.
vss::result<vss::link_id> make_wire(StudioBackend& backend,
                                    vss::endpoint_id from, vss::endpoint_id to,
                                    wire_kind kind);

/// A link described by the names of its ports, so it can be made again after
/// the ports were.
struct link_recipe
{
    link_shape shape = link_shape::two_way;
    QStringList sources;
    QStringList targets;
    vss::link_options options;
    QString owner;
};

link_recipe recipe_of(const std::vector<port_view>& ports,
                      const link_view& link);
/// Makes the link again; ports are found by name.
vss::result<vss::link_id> make_link(StudioBackend& backend,
                                    const link_recipe& recipe);
/// The link with the same shape and ends, if there is one.
std::optional<link_view> find_link(const StudioBackend& backend,
                                   const link_recipe& recipe);

/// The saved description of a port (kind, address, settings, COM number),
/// from the backend's snapshot.
std::optional<vss::config::port_entry>
port_entry_of(const StudioBackend& backend, const QString& name);

/// Makes a port from its saved description, with `name`.
vss::result<vss::endpoint_id> make_port(StudioBackend& backend,
                                        const vss::config::port_entry& entry,
                                        const QString& name);

/// False for a port only one program can open (a real device, a network
/// client), which cannot be duplicated.
bool can_duplicate(const port_view& port);

/// Copies the ports and the links between them.  Each copy has the same
/// kind and settings and the next free name.  Fails, and makes nothing, when
/// any of the ports cannot be duplicated.
vss::result<bundle_made> duplicate_ports(StudioBackend& backend,
                                         const QVector<vss::endpoint_id>& ids);

/// Everything needed to make removed ports and links again.
struct removal
{
    QVector<vss::config::port_entry> ports;
    QVector<link_recipe> links;
    /// The paths the ports had, by name, so a different new path can be named.
    QVector<QPair<QString, QString>> paths;
};

/// Describes what removing these ports and links takes away: the ports, the
/// links given and every link that ends on a removed port.
removal describe_removal(const StudioBackend& backend,
                         const QVector<vss::endpoint_id>& ports,
                         const QVector<vss::link_id>& links);

/// Removes them.  Stops at the first failure.
vss::result<void> apply_removal(StudioBackend& backend, const removal& what);

/// Makes the ports and links again.  Returns, for each port whose path
/// changed, its name, the old path and the new one.
vss::result<QVector<QStringList>> restore_removal(StudioBackend& backend,
                                                  const removal& what);

/// Changes the options of a link in place (delay, jitter, policy, modem lines):
/// the link keeps its id, ports, counters and, unless the policy leaves
/// `owner`, its owner.  A delayed link discards the bytes its delay still holds
/// when the delay changes.  Nothing is changed when the engine refuses.
vss::result<void> change_link_options(StudioBackend& backend, vss::link_id id,
                                      const vss::link_options& options);

} // namespace vss::gui
