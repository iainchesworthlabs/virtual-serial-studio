//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/canvas_edit.hpp"
#include "backend/driver_error.hpp"

#include <vss/core/profile.hpp>

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace vss::gui
{
namespace
{

vss::error_code failure()
{
    return vss::make_error_code(vss::errc::internal);
}

std::optional<port_view> port_named(const StudioBackend& backend,
                                    const QString& name)
{
    for (const port_view& port : backend.ports())
    {
        if (port.name == name)
            return port;
    }
    return std::nullopt;
}

QString name_of(const std::vector<port_view>& ports, vss::endpoint_id id)
{
    for (const port_view& port : ports)
    {
        if (port.id == id)
            return port.name;
    }
    return {};
}

std::vector<vss::endpoint_id> to_vector(const QVector<vss::endpoint_id>& ids)
{
    return {ids.begin(), ids.end()};
}

QVector<vss::endpoint_id> sorted(QVector<vss::endpoint_id> ids)
{
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// Ids for names, or empty when a name is not a port.
std::optional<QVector<vss::endpoint_id>> ids_of(const StudioBackend& backend,
                                                const QStringList& names)
{
    QVector<vss::endpoint_id> ids;
    for (const QString& name : names)
    {
        const auto port = port_named(backend, name);
        if (!port)
            return std::nullopt;
        ids.append(port->id);
    }
    return ids;
}

QString base_for(slot_kind kind)
{
    switch (kind)
    {
    case slot_kind::new_fixture:
        return QStringLiteral("fixture");
    case slot_kind::new_loopback:
        return QStringLiteral("loopback");
    case slot_kind::new_port:
    case slot_kind::existing:
        break;
    }
    return QStringLiteral("port");
}

/// Removes what a failed step left behind.
void roll_back(StudioBackend& backend, const QVector<vss::link_id>& links,
               const QStringList& created)
{
    for (const vss::link_id id : links)
        static_cast<void>(backend.disconnect(id));
    for (const QString& name : created)
    {
        if (const auto port = port_named(backend, name))
            static_cast<void>(backend.remove_port(port->id));
    }
    backend.refresh();
}

vss::result<vss::endpoint_id>
make_slot_port(StudioBackend& backend, const bundle_slot& slot,
               const vss::serial_settings& settings)
{
    vss::result<vss::endpoint_id> made = failure();
    switch (slot.kind)
    {
    case slot_kind::new_fixture:
        return backend.create_mock_port(slot.name, settings,
                                        QStringLiteral("scripted"));
    case slot_kind::new_loopback:
        made = backend.create_virtual_port(
            slot.name, vss::virtual_port_kind::loopback, true);
        break;
    case slot_kind::new_port:
        made = backend.create_auto_port(slot.name, true);
        // A refused driver port is said in words, with the set-up to run.
        if (!made)
            return driver_aware_error(backend, made.error());
        break;
    case slot_kind::existing:
        return failure();
    }
    // A virtual port keeps the values the user chose with the setup; the
    // program that opens it sets the line itself.
    if (made && settings != vss::serial_settings{} &&
        backend.caps().port_settings)
        static_cast<void>(backend.set_port_settings(*made, settings));
    return made;
}

/// The name a copy of `name` gets: `fixture-2` from `fixture-1`, and
/// `rx-sim-copy` (then `rx-sim-copy-1`, ...) when the name has no number.
QString copy_name(const std::vector<port_view>& ports, const QString& name,
                  const QStringList& taken)
{
    static const QRegularExpression numbered(QStringLiteral("^(.*)-(\\d+)$"));
    const auto match = numbered.match(name);
    if (match.hasMatch())
        return next_free_name(ports, match.captured(1), taken);

    const QString copy = name + QStringLiteral("-copy");
    const bool used =
        taken.contains(copy) ||
        std::any_of(ports.begin(), ports.end(), [&copy](const port_view& port)
                    { return port.name == copy; });
    return used ? next_free_name(ports, copy, taken) : copy;
}

} // namespace

bool is_bundle(canvas_item item)
{
    switch (item)
    {
    case canvas_item::device:
    case canvas_item::network:
    case canvas_item::share:
        return false;
    default:
        return true;
    }
}

bool item_available(const capabilities& caps, canvas_item item)
{
    switch (item)
    {
    case canvas_item::port:
    case canvas_item::loopback:
        return caps.create_ports;
    case canvas_item::fixture:
        return caps.fixtures;
    case canvas_item::bench:
        return caps.fixtures && caps.create_ports && caps.links;
    case canvas_item::device:
        return caps.devices;
    case canvas_item::network:
        return caps.bridge;
    case canvas_item::bus:
        return caps.shared_bus && caps.create_ports;
    case canvas_item::switch_link:
        return caps.links && caps.owner && caps.create_ports;
    case canvas_item::share:
        return caps.share;
    case canvas_item::pair:
    case canvas_item::split:
    case canvas_item::join:
    case canvas_item::redirect:
    case canvas_item::monitor:
        return caps.links && caps.create_ports;
    }
    return false;
}

int default_count(canvas_item item)
{
    switch (item)
    {
    case canvas_item::split:
    case canvas_item::join:
    case canvas_item::switch_link:
    case canvas_item::bench:
        return 2;
    case canvas_item::bus:
        return 3;
    case canvas_item::pair:
    case canvas_item::redirect:
    case canvas_item::monitor:
        return 2;
    default:
        return 1;
    }
}

int minimum_count(canvas_item item)
{
    switch (item)
    {
    case canvas_item::pair:
    case canvas_item::redirect:
    case canvas_item::monitor:
    case canvas_item::split:
    case canvas_item::join:
    case canvas_item::switch_link:
    case canvas_item::bus:
        return 2;
    default:
        return 1;
    }
}

int maximum_count(canvas_item item)
{
    switch (item)
    {
    case canvas_item::pair:
    case canvas_item::redirect:
    case canvas_item::monitor:
        return 2;
    case canvas_item::split:
    case canvas_item::join:
    case canvas_item::switch_link:
    case canvas_item::bus:
        return 8;
    default:
        return 16;
    }
}

int slot_count(canvas_item item, int count)
{
    count = std::clamp(count, minimum_count(item), maximum_count(item));
    switch (item)
    {
    case canvas_item::split:
    case canvas_item::join:
    case canvas_item::switch_link:
        return count + 1;
    case canvas_item::bench:
        return count * 2;
    default:
        return count;
    }
}

slot_kind default_slot_kind(canvas_item item, int index)
{
    switch (item)
    {
    case canvas_item::fixture:
        return slot_kind::new_fixture;
    case canvas_item::loopback:
        return slot_kind::new_loopback;
    case canvas_item::bench:
        return index % 2 == 0 ? slot_kind::new_fixture : slot_kind::new_port;
    default:
        return slot_kind::new_port;
    }
}

QString next_free_name(const std::vector<port_view>& ports, const QString& base,
                       const QStringList& taken)
{
    QSet<QString> used(taken.begin(), taken.end());
    for (const port_view& port : ports)
        used.insert(port.name);
    for (int number = 1;; ++number)
    {
        const QString name = QStringLiteral("%1-%2").arg(base).arg(number);
        if (!used.contains(name))
            return name;
    }
}

bundle_request default_request(canvas_item item,
                               const std::vector<port_view>& ports, int count)
{
    bundle_request request;
    request.item = item;
    const int slots = slot_count(item, count > 0 ? count : default_count(item));
    QStringList taken;
    for (int index = 0; index < slots; ++index)
    {
        bundle_slot slot;
        slot.kind = default_slot_kind(item, index);
        slot.name = next_free_name(ports, base_for(slot.kind), taken);
        taken.append(slot.name);
        request.slots.append(slot);
    }
    return request;
}

vss::result<bundle_made> build_bundle(StudioBackend& backend,
                                      const bundle_request& request)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.build_bundle");
    if (!is_bundle(request.item) ||
        !item_available(backend.caps(), request.item))
        return vss::make_error_code(vss::errc::unsupported);

    bundle_made made;
    QVector<vss::endpoint_id> ids;
    for (const bundle_slot& slot : request.slots)
    {
        if (slot.kind == slot_kind::existing)
        {
            const auto port = port_named(backend, slot.name);
            if (!port)
            {
                roll_back(backend, made.links, made.created);
                return vss::make_error_code(vss::errc::not_found);
            }
            ids.append(port->id);
            continue;
        }
        const auto port = make_slot_port(backend, slot, request.settings);
        if (!port)
        {
            roll_back(backend, made.links, made.created);
            return port.error();
        }
        // The studio may have made the name unique.
        const auto view = backend.port(*port);
        made.created.append(view ? view->name : slot.name);
        ids.append(*port);
    }

    const auto add = [&made](vss::result<vss::link_id> link) -> bool
    {
        if (!link)
            return false;
        made.links.append(*link);
        return true;
    };

    vss::result<vss::link_id> link = failure();
    bool linked = true;
    const QVector<vss::endpoint_id> rest =
        ids.size() > 1 ? ids.mid(1) : QVector<vss::endpoint_id>{};
    switch (request.item)
    {
    case canvas_item::pair:
        if (ids.size() == 2)
            link = backend.connect_ports(ids[0], ids[1], {});
        linked = add(link);
        break;
    case canvas_item::split:
        link = backend.split(ids.value(0), to_vector(rest), {});
        linked = add(link);
        break;
    case canvas_item::join:
        link = backend.join(to_vector(rest), ids.value(0), {});
        linked = add(link);
        break;
    case canvas_item::bus:
        link = backend.bus(to_vector(ids));
        linked = add(link);
        break;
    case canvas_item::switch_link:
    {
        vss::link_options options;
        options.policy = vss::merge_policy::owner;
        // The owner of a link is one of its sources, so the programs send
        // into the device and one of them at a time is let through.
        link = backend.join(to_vector(rest), ids.value(0), options);
        linked = add(link) && !rest.isEmpty() &&
                 backend.set_owner(*link, rest.first()).has_value();
        break;
    }
    case canvas_item::redirect:
        link = backend.redirect(ids.value(0), ids.value(1), {});
        linked = add(link);
        break;
    case canvas_item::monitor:
        link = backend.monitor(ids.value(0), ids.value(1));
        linked = add(link);
        break;
    case canvas_item::bench:
        for (qsizetype index = 0; linked && index + 1 < ids.size(); index += 2)
        {
            link = backend.connect_ports(ids[index], ids[index + 1], {});
            linked = add(link);
        }
        break;
    case canvas_item::port:
    case canvas_item::fixture:
    case canvas_item::loopback:
    case canvas_item::device:
    case canvas_item::network:
    case canvas_item::share:
        break;
    }
    if (!linked)
    {
        roll_back(backend, made.links, made.created);
        return link ? failure() : link.error();
    }
    backend.refresh();
    return made;
}

vss::result<vss::link_id> make_wire(StudioBackend& backend,
                                    vss::endpoint_id from, vss::endpoint_id to,
                                    wire_kind kind)
{
    vss::result<vss::link_id> made = failure();
    switch (kind)
    {
    case wire_kind::two_way:
        made = backend.connect_ports(from, to, {});
        break;
    case wire_kind::one_way:
        made = backend.redirect(from, to, {});
        break;
    case wire_kind::monitor:
        made = backend.monitor(from, to);
        break;
    }
    if (made)
        backend.refresh();
    return made;
}

link_recipe recipe_of(const std::vector<port_view>& ports,
                      const link_view& link)
{
    link_recipe recipe;
    recipe.shape = link.shape;
    recipe.options = link.options;
    for (const vss::endpoint_id id : link.sources)
        recipe.sources.append(name_of(ports, id));
    for (const vss::endpoint_id id : link.targets)
        recipe.targets.append(name_of(ports, id));
    if (link.owner != 0)
        recipe.owner = name_of(ports, link.owner);
    return recipe;
}

std::optional<link_view> find_link(const StudioBackend& backend,
                                   const link_recipe& recipe)
{
    const auto sources = ids_of(backend, recipe.sources);
    const auto targets = ids_of(backend, recipe.targets);
    if (!sources || !targets)
        return std::nullopt;
    for (const link_view& link : backend.links())
    {
        if (link.shape == recipe.shape &&
            sorted(link.sources) == sorted(*sources) &&
            sorted(link.targets) == sorted(*targets))
            return link;
    }
    return std::nullopt;
}

vss::result<vss::link_id> make_link(StudioBackend& backend,
                                    const link_recipe& recipe)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.make_link");
    const auto sources = ids_of(backend, recipe.sources);
    const auto targets = ids_of(backend, recipe.targets);
    if (!sources || !targets || sources->isEmpty())
        return vss::make_error_code(vss::errc::not_found);
    if (recipe.shape != link_shape::bus && targets->isEmpty())
        return vss::make_error_code(vss::errc::invalid_argument);

    vss::result<vss::link_id> made = failure();
    switch (recipe.shape)
    {
    case link_shape::two_way:
        made = backend.connect_ports(sources->first(), targets->first(),
                                     recipe.options);
        break;
    case link_shape::one_way:
        made = backend.redirect(sources->first(), targets->first(),
                                recipe.options);
        break;
    case link_shape::monitor:
        made = backend.monitor(sources->first(), targets->first());
        break;
    case link_shape::split:
        made = backend.split(sources->first(), to_vector(*targets),
                             recipe.options);
        break;
    case link_shape::join:
        made =
            backend.join(to_vector(*sources), targets->first(), recipe.options);
        break;
    case link_shape::bus:
        made = backend.bus(to_vector(*sources));
        break;
    }
    if (!made)
        return made;
    if (!recipe.owner.isEmpty())
    {
        if (const auto owner = port_named(backend, recipe.owner))
            static_cast<void>(backend.set_owner(*made, owner->id));
    }
    backend.refresh();
    return made;
}

std::optional<vss::config::port_entry>
port_entry_of(const StudioBackend& backend, const QString& name)
{
    const std::string wanted = name.toStdString();
    for (const auto& entry : backend.snapshot().ports)
    {
        if (entry.name == wanted)
            return entry;
    }
    return std::nullopt;
}

vss::result<vss::endpoint_id> make_port(StudioBackend& backend,
                                        const vss::config::port_entry& entry,
                                        const QString& name)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.make_port");
    vss::result<vss::endpoint_id> made = failure();
    if (entry.kind == "mock")
    {
        return backend.create_mock_port(
            name, entry.serial,
            entry.mock_target.empty()
                ? QStringLiteral("scripted")
                : QString::fromStdString(entry.mock_target));
    }
    if (entry.kind != "virtual")
    {
        return backend.attach_serial(name, QString::fromStdString(entry.uri),
                                     entry.serial, entry.capture);
    }

    const std::string& kind = entry.virtual_kind;
    if (kind == "loopback")
        made = backend.create_virtual_port(
            name, vss::virtual_port_kind::loopback, entry.capture);
    else if (kind == "pty")
        made = backend.create_virtual_port(name, vss::virtual_port_kind::pty,
                                           entry.capture);
    else if (kind == "named-pipe")
        made = backend.create_virtual_port(
            name, vss::virtual_port_kind::named_pipe, entry.capture);
    else if (kind == "windows-driver" ||
             (entry.com_number != 0 && backend.caps().com_numbers))
        made =
            backend.create_driver_port(name, entry.capture, entry.com_number);
    else
        made = backend.create_auto_port(name, entry.capture);

    if (made && entry.serial != vss::serial_settings{} &&
        backend.caps().port_settings)
        static_cast<void>(backend.set_port_settings(*made, entry.serial));
    return made;
}

bool can_duplicate(const port_view& port)
{
    return port.kind == vss::endpoint_kind::virtual_port ||
           port.kind == vss::endpoint_kind::mock;
}

vss::result<bundle_made> duplicate_ports(StudioBackend& backend,
                                         const QVector<vss::endpoint_id>& ids)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.duplicate");
    const std::vector<port_view> ports = backend.ports();
    QHash<QString, QString> copies;
    QStringList order;
    for (const vss::endpoint_id id : ids)
    {
        const auto view = backend.port(id);
        if (!view)
            return vss::make_error_code(vss::errc::not_found);
        if (!can_duplicate(*view))
            return vss::make_error_code(vss::errc::unsupported);
        order.append(view->name);
    }

    bundle_made made;
    QStringList taken;
    for (const QString& name : order)
    {
        auto entry = port_entry_of(backend, name);
        if (!entry)
        {
            roll_back(backend, made.links, made.created);
            return vss::make_error_code(vss::errc::not_found);
        }
        // A copy of a driver port takes the next free COM number.
        entry->com_number = 0;
        const QString copy = copy_name(ports, name, taken);
        taken.append(copy);
        const auto port = make_port(backend, *entry, copy);
        if (!port)
        {
            roll_back(backend, made.links, made.created);
            return port.error();
        }
        const auto view = backend.port(*port);
        made.created.append(view ? view->name : copy);
        copies.insert(name, made.created.last());
    }

    // The links between two copied ports are copied too.
    for (const link_view& link : backend.links())
    {
        link_recipe recipe = recipe_of(ports, link);
        QStringList ends = recipe.sources + recipe.targets;
        const bool inside =
            !ends.isEmpty() &&
            std::all_of(ends.begin(), ends.end(), [&copies](const QString& n)
                        { return copies.contains(n); });
        if (!inside)
            continue;
        for (QString& name : recipe.sources)
            name = copies.value(name);
        for (QString& name : recipe.targets)
            name = copies.value(name);
        if (!recipe.owner.isEmpty())
            recipe.owner = copies.value(recipe.owner);
        const auto copied = make_link(backend, recipe);
        if (!copied)
        {
            roll_back(backend, made.links, made.created);
            return copied.error();
        }
        made.links.append(*copied);
    }
    backend.refresh();
    return made;
}

removal describe_removal(const StudioBackend& backend,
                         const QVector<vss::endpoint_id>& ports,
                         const QVector<vss::link_id>& links)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.describe_removal");
    removal what;
    const std::vector<port_view> views = backend.ports();
    QSet<vss::endpoint_id> going(ports.begin(), ports.end());
    for (const port_view& view : views)
    {
        if (!going.contains(view.id))
            continue;
        if (auto entry = port_entry_of(backend, view.name))
        {
            what.ports.append(std::move(*entry));
            what.paths.append({view.name, view.path});
        }
    }
    for (const link_view& link : backend.links())
    {
        const bool touches =
            std::any_of(link.sources.begin(), link.sources.end(),
                        [&going](vss::endpoint_id id)
                        { return going.contains(id); }) ||
            std::any_of(link.targets.begin(), link.targets.end(),
                        [&going](vss::endpoint_id id)
                        { return going.contains(id); });
        if (touches || links.contains(link.id))
            what.links.append(recipe_of(views, link));
    }
    return what;
}

vss::result<void> apply_removal(StudioBackend& backend, const removal& what)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.apply_removal");
    for (const link_recipe& recipe : what.links)
    {
        if (const auto link = find_link(backend, recipe))
        {
            if (auto done = backend.disconnect(link->id); !done)
                return done;
        }
    }
    for (const auto& entry : what.ports)
    {
        const auto port =
            port_named(backend, QString::fromStdString(entry.name));
        if (!port)
            continue;
        if (auto done = backend.remove_port(port->id); !done)
            return done;
    }
    backend.refresh();
    return {};
}

vss::result<QVector<QStringList>> restore_removal(StudioBackend& backend,
                                                  const removal& what)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.restore_removal");
    QVector<QStringList> moved;
    for (qsizetype index = 0; index < what.ports.size(); ++index)
    {
        const auto& entry = what.ports[index];
        const QString name = QString::fromStdString(entry.name);
        const auto made = make_port(backend, entry, name);
        if (!made)
            return made.error();
        const auto view = backend.port(*made);
        const QString before = what.paths.value(index).second;
        if (view && !before.isEmpty() && view->path != before)
            moved.append({view->name, before, view->path});
    }
    for (const link_recipe& recipe : what.links)
    {
        if (find_link(backend, recipe))
            continue;
        if (const auto made = make_link(backend, recipe); !made)
            return made.error();
    }
    backend.refresh();
    return moved;
}

vss::result<vss::link_id> remake_link(StudioBackend& backend, vss::link_id id,
                                      const vss::link_options& options)
{
    VSS_PROFILE_ZONE("gui.backend.canvas.remake_link");
    const auto link = backend.link(id);
    if (!link)
        return vss::make_error_code(vss::errc::not_found);
    const link_recipe before = recipe_of(backend.ports(), *link);
    link_recipe after = before;
    after.options = options;

    if (auto done = backend.disconnect(id); !done)
        return done.error();
    auto made = make_link(backend, after);
    if (!made)
    {
        static_cast<void>(make_link(backend, before));
        return made.error();
    }
    return made;
}

} // namespace vss::gui
