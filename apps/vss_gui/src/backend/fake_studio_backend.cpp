//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/fake_studio_backend.hpp"

#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <thread>
#include <utility>

namespace vss::gui
{
namespace
{

QString path_for(vss::virtual_port_kind kind, const QString& name,
                 vss::endpoint_id id)
{
    switch (kind)
    {
    case vss::virtual_port_kind::loopback:
        return QStringLiteral("loopback:%1").arg(name);
    case vss::virtual_port_kind::pty:
        return QStringLiteral("/dev/pts/%1").arg(id);
    case vss::virtual_port_kind::named_pipe:
        return QStringLiteral("\\\\.\\pipe\\%1").arg(name);
    case vss::virtual_port_kind::windows_driver:
        return QStringLiteral("COM%1").arg(id);
    }
    return QStringLiteral("fake:%1").arg(name);
}

/// The shape the engine reports for a one-way link: several sources are a
/// join whether or not it carries modem lines, and one source is a one-way
/// link or, without modem lines, a monitor.
void set_one_way_shape(link_view& view)
{
    if (view.sources.size() > 1)
        view.shape = link_shape::join;
    else
        view.shape = view.options.emulate_modem_lines ? link_shape::one_way
                                                      : link_shape::monitor;
}

vss::endpoint_kind kind_for_uri(const QString& uri)
{
    const QString scheme = uri.section(QStringLiteral("://"), 0, 0);
    if (scheme == QLatin1String("tcp") || scheme == QLatin1String("tcp-listen"))
        return vss::endpoint_kind::tcp;
    if (scheme == QLatin1String("udp") || scheme == QLatin1String("udp-listen"))
        return vss::endpoint_kind::udp;
    if (scheme == QLatin1String("rfc2217"))
        return vss::endpoint_kind::rfc2217;
    return vss::endpoint_kind::physical;
}

} // namespace

FakeStudioBackend::FakeStudioBackend(QObject* parent) : StudioBackend(parent) {}

void FakeStudioBackend::set_lost_ports(std::vector<vss::endpoint_id> ids)
{
    lost_ = std::move(ids);
    Q_EMIT ports_changed();
}

std::vector<vss::endpoint_id> FakeStudioBackend::lost_ports() const
{
    return lost_;
}

vss::result<std::size_t> FakeStudioBackend::recreate_lost_ports()
{
    if (next_error_)
    {
        const vss::error_code error = *next_error_;
        next_error_.reset();
        return error;
    }
    const std::size_t made = lost_.size();
    lost_.clear();
    Q_EMIT ports_changed();
    return made;
}

void FakeStudioBackend::set_peer_state(vss::endpoint_id id, peer_state state)
{
    if (port_view* view = find_port(id))
    {
        view->peer = state;
        refresh();
    }
}

void FakeStudioBackend::set_devices(std::vector<device_view> devices)
{
    devices_ = std::move(devices);
}

std::vector<traffic_view> FakeStudioBackend::traffic(int limit) const
{
    if (limit <= 0 || static_cast<std::size_t>(limit) >= traffic_.size())
        return traffic_;
    std::vector<traffic_view> out;
    out.reserve(static_cast<std::size_t>(limit));
    const auto first = traffic_.end() - static_cast<std::ptrdiff_t>(limit);
    out.assign(first, traffic_.end());
    return out;
}

std::optional<port_view> FakeStudioBackend::port(vss::endpoint_id id) const
{
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    if (it == ports_.end())
        return std::nullopt;
    return *it;
}

std::optional<link_view> FakeStudioBackend::link(vss::link_id id) const
{
    const auto it =
        std::find_if(links_.begin(), links_.end(),
                     [id](const link_view& view) { return view.id == id; });
    if (it == links_.end())
        return std::nullopt;
    return *it;
}

QString FakeStudioBackend::unique_name(const QString& base) const
{
    QString stem = base.isEmpty() ? QStringLiteral("port") : base;
    const auto taken = [this](const QString& candidate)
    {
        return std::any_of(ports_.begin(), ports_.end(),
                           [&candidate](const port_view& view)
                           { return view.name == candidate; });
    };
    if (!taken(stem))
        return stem;
    for (int suffix = 2;; ++suffix)
    {
        const QString candidate = QStringLiteral("%1-%2").arg(stem).arg(suffix);
        if (!taken(candidate))
            return candidate;
    }
}

std::optional<vss::error_code> FakeStudioBackend::take_error()
{
    if (!next_error_)
        return std::nullopt;
    if (calls_before_error_ > 0)
    {
        --calls_before_error_;
        return std::nullopt;
    }
    const vss::error_code error = *next_error_;
    next_error_.reset();
    return error;
}

port_view* FakeStudioBackend::find_port(vss::endpoint_id id)
{
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    return it == ports_.end() ? nullptr : &*it;
}

link_view* FakeStudioBackend::find_link(vss::link_id id)
{
    const auto it =
        std::find_if(links_.begin(), links_.end(),
                     [id](const link_view& view) { return view.id == id; });
    return it == links_.end() ? nullptr : &*it;
}

vss::result<vss::endpoint_id>
FakeStudioBackend::create_virtual_port(const QString& name,
                                       vss::virtual_port_kind kind, bool)
{
    if (const auto error = take_error())
        return *error;

    port_view view;
    view.id = next_port_++;
    view.name = unique_name(name);
    view.kind = vss::endpoint_kind::virtual_port;
    view.state = vss::endpoint_state::open;
    view.open = true;
    view.supports_control_lines = true;
    view.stats.started = vss::clock_type::now();
    view.path = path_for(kind, view.name, view.id);
    view.description = QStringLiteral("Virtual port %1").arg(view.name);
    view.studio_only = kind == vss::virtual_port_kind::loopback;
    const vss::endpoint_id id = view.id;
    ports_.push_back(std::move(view));
    refresh();
    return id;
}

vss::result<vss::endpoint_id>
FakeStudioBackend::create_driver_port(const QString& name, bool capture,
                                      unsigned com_number)
{
    if (com_number > 255)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (com_number != 0 &&
        (taken_com_numbers_.contains(com_number) ||
         hidden_com_numbers_.contains(com_number) ||
         std::any_of(ports_.begin(), ports_.end(),
                     [com_number](const port_view& view)
                     { return view.com_number == com_number; })))
        return vss::make_error_code(vss::errc::already_exists);

    const auto created = create_virtual_port(
        name, vss::virtual_port_kind::windows_driver, capture);
    if (!created)
        return created;
    if (port_view* view = find_port(*created))
    {
        view->driver_port = true;
        view->com_number = com_number != 0
                               ? com_number
                               : static_cast<unsigned>(next_com_number_++);
        view->path = QStringLiteral("COM%1").arg(view->com_number);
        refresh();
    }
    return created;
}

vss::result<QString> FakeStudioBackend::set_com_number(vss::endpoint_id id,
                                                       unsigned com_number)
{
    if (const auto error = take_error())
        return *error;
    port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!view->driver_port)
        return vss::make_error_code(vss::errc::unsupported);
    if (com_number < 1 || com_number > 255)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (view->com_number == com_number)
        return view->path;
    if (taken_com_numbers_.contains(com_number) ||
        std::any_of(ports_.begin(), ports_.end(),
                    [com_number](const port_view& other)
                    { return other.com_number == com_number; }))
        return vss::make_error_code(vss::errc::already_exists);
    if (view->peer == peer_state::open)
        return vss::make_error_code(vss::errc::device_busy);
    view->com_number = com_number;
    view->path = QStringLiteral("COM%1").arg(com_number);
    return view->path;
}

std::optional<holder_view>
FakeStudioBackend::port_holder(vss::endpoint_id id) const
{
    const auto found = holders_.constFind(id);
    if (found == holders_.constEnd())
        return std::nullopt;
    return *found;
}

void FakeStudioBackend::set_holder(vss::endpoint_id id,
                                   std::optional<holder_view> holder)
{
    if (holder)
        holders_.insert(id, *holder);
    else
        holders_.remove(id);
}

vss::result<void> FakeStudioBackend::release_port(vss::endpoint_id id,
                                                  release_method method)
{
    if (const auto error = take_error())
        return *error;
    port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    const auto holder = holders_.constFind(id);
    if (holder == holders_.constEnd())
        return vss::make_error_code(vss::errc::not_found);
    if (method == release_method::end_program && !holder->can_end)
        return vss::make_error_code(vss::errc::permission_denied);
    releases_.emplace_back(id, method);
    holders_.remove(id);
    view->peer = peer_state::not_open;
    refresh();
    return {};
}

vss::result<vss::endpoint_id>
FakeStudioBackend::create_auto_port(const QString& name, bool capture)
{
    return create_virtual_port(name, vss::virtual_port_kind::pty, capture);
}

vss::result<vss::endpoint_id>
FakeStudioBackend::create_mock_port(const QString& name,
                                    const vss::serial_settings& settings,
                                    const QString& target)
{
    if (const auto error = take_error())
        return *error;

    port_view view;
    view.id = next_port_++;
    view.name = unique_name(name);
    view.kind = vss::endpoint_kind::mock;
    view.state = vss::endpoint_state::open;
    view.open = true;
    view.supports_control_lines = true;
    view.serial = settings;
    view.stats.started = vss::clock_type::now();
    view.path = target;
    view.description = QStringLiteral("Mock fixture %1").arg(target);
    view.studio_only = true;
    const vss::endpoint_id id = view.id;
    ports_.push_back(std::move(view));
    refresh();
    return id;
}

vss::result<vss::endpoint_id>
FakeStudioBackend::attach_serial(const QString& name, const QString& uri,
                                 const vss::serial_settings& settings, bool)
{
    if (const auto error = take_error())
        return *error;

    port_view view;
    view.id = next_port_++;
    view.name = unique_name(name);
    view.kind = kind_for_uri(uri);
    view.state = vss::endpoint_state::open;
    view.open = true;
    view.supports_control_lines = view.kind == vss::endpoint_kind::physical;
    view.serial = settings;
    view.stats.started = vss::clock_type::now();
    view.path = uri;
    view.description = uri;
    view.studio_only = false;
    const vss::endpoint_id id = view.id;
    ports_.push_back(std::move(view));
    refresh();
    return id;
}

vss::result<void> FakeStudioBackend::remove_port(vss::endpoint_id id)
{
    if (const auto error = take_error())
        return *error;

    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    if (it == ports_.end())
        return vss::make_error_code(vss::errc::not_found);
    ports_.erase(it);
    std::erase(echoing_, id);

    std::vector<vss::link_id> removed_links;
    for (const auto& view : links_)
    {
        if (view.sources.contains(id) || view.targets.contains(id))
            removed_links.push_back(view.id);
    }
    for (const vss::link_id link_id : removed_links)
        wiring_.remove(link_id);
    std::erase_if(
        links_, [id](const link_view& view)
        { return view.sources.contains(id) || view.targets.contains(id); });
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::rename_port(vss::endpoint_id id,
                                                 const QString& name)
{
    if (const auto error = take_error())
        return *error;
    if (name.isEmpty())
        return vss::make_error_code(vss::errc::invalid_argument);
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    if (it == ports_.end())
        return vss::make_error_code(vss::errc::not_found);
    if (it->name == name)
        return {};
    const bool taken =
        std::any_of(ports_.begin(), ports_.end(), [&name](const port_view& view)
                    { return view.name == name; });
    if (taken)
        return vss::make_error_code(vss::errc::already_exists);
    it->name = name;
    refresh();
    return {};
}

vss::result<void>
FakeStudioBackend::set_port_settings(vss::endpoint_id id,
                                     const vss::serial_settings& settings)
{
    if (const auto error = take_error())
        return *error;
    if (const auto invalid = vss::validate(settings))
        return invalid;
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    if (it == ports_.end())
        return vss::make_error_code(vss::errc::not_found);
    if (it->kind == vss::endpoint_kind::tcp ||
        it->kind == vss::endpoint_kind::udp)
        return vss::make_error_code(vss::errc::unsupported);
    // Like the engine, no refresh: the caller may be a worker thread.
    it->serial = settings;
    return {};
}

vss::result<vss::link_id>
FakeStudioBackend::connect_ports(vss::endpoint_id first,
                                 vss::endpoint_id second,
                                 const vss::link_options& options)
{
    if (const auto error = take_error())
        return *error;
    if (first == second)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!find_port(first) || !find_port(second))
        return vss::make_error_code(vss::errc::not_found);

    return add_link(link_shape::two_way, vss::link_mode::duplex, {first},
                    {second}, options);
}

vss::result<vss::link_id>
FakeStudioBackend::split(vss::endpoint_id source,
                         const std::vector<vss::endpoint_id>& targets,
                         const vss::link_options& options)
{
    if (const auto error = take_error())
        return *error;
    if (targets.empty())
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!find_port(source))
        return vss::make_error_code(vss::errc::not_found);
    for (const vss::endpoint_id target : targets)
    {
        if (!find_port(target) || target == source)
            return vss::make_error_code(vss::errc::invalid_argument);
    }

    return add_link(link_shape::split, vss::link_mode::duplex, {source},
                    targets, options);
}

vss::result<vss::link_id>
FakeStudioBackend::join(const std::vector<vss::endpoint_id>& sources,
                        vss::endpoint_id target,
                        const vss::link_options& options)
{
    if (const auto error = take_error())
        return *error;
    if (sources.empty())
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!find_port(target))
        return vss::make_error_code(vss::errc::not_found);
    for (const vss::endpoint_id source : sources)
    {
        if (!find_port(source) || source == target)
            return vss::make_error_code(vss::errc::invalid_argument);
    }

    return add_link(link_shape::join, vss::link_mode::one_way, sources,
                    {target}, options);
}

vss::result<vss::link_id>
FakeStudioBackend::redirect(vss::endpoint_id from, vss::endpoint_id to,
                            const vss::link_options& options)
{
    if (const auto error = take_error())
        return *error;
    if (from == to)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!find_port(from) || !find_port(to))
        return vss::make_error_code(vss::errc::not_found);

    return add_link(link_shape::one_way, vss::link_mode::one_way, {from}, {to},
                    options);
}

vss::result<vss::link_id> FakeStudioBackend::monitor(vss::endpoint_id from,
                                                     vss::endpoint_id to)
{
    if (const auto error = take_error())
        return *error;
    if (from == to)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (!find_port(from) || !find_port(to))
        return vss::make_error_code(vss::errc::not_found);

    vss::link_options options;
    options.emulate_modem_lines = false;
    return add_link(link_shape::monitor, vss::link_mode::one_way, {from}, {to},
                    options);
}

vss::result<vss::link_id>
FakeStudioBackend::bus(const std::vector<vss::endpoint_id>& members)
{
    if (const auto error = take_error())
        return *error;
    if (!caps_.shared_bus)
        return vss::make_error_code(vss::errc::unsupported);
    if (members.size() < 2 || members.size() > vss::max_bus_members)
        return vss::make_error_code(vss::errc::invalid_argument);
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        if (!find_port(members[index]))
            return vss::make_error_code(vss::errc::not_found);
        for (std::size_t other = 0; other < index; ++other)
        {
            if (members[other] == members[index])
                return vss::make_error_code(vss::errc::invalid_argument);
        }
        for (const link_view& link : links_)
        {
            if (link.shape == link_shape::bus &&
                link.sources.contains(members[index]))
                return vss::make_error_code(vss::errc::already_exists);
        }
    }

    vss::link_options options;
    options.emulate_modem_lines = false;
    return add_link(link_shape::bus, vss::link_mode::bus, members, {}, options);
}

vss::link_id
FakeStudioBackend::add_link(link_shape shape, vss::link_mode mode,
                            const std::vector<vss::endpoint_id>& sources,
                            const std::vector<vss::endpoint_id>& targets,
                            const vss::link_options& options)
{
    link_view view;
    view.id = next_link_++;
    const vss::link_id link_id = view.id;
    view.shape = shape;
    view.mode = mode;
    view.options = options;
    view.sources.reserve(static_cast<qsizetype>(sources.size()));
    for (const vss::endpoint_id id : sources)
        view.sources.push_back(id);
    view.targets.reserve(static_cast<qsizetype>(targets.size()));
    for (const vss::endpoint_id id : targets)
        view.targets.push_back(id);
    links_.push_back(std::move(view));
    refresh();
    return link_id;
}

vss::result<void> FakeStudioBackend::disconnect(vss::link_id id)
{
    if (const auto error = take_error())
        return *error;
    const auto it =
        std::find_if(links_.begin(), links_.end(),
                     [id](const link_view& view) { return view.id == id; });
    if (it == links_.end())
        return vss::make_error_code(vss::errc::not_found);
    wiring_.remove(id);
    links_.erase(it);
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::add_target(vss::link_id link,
                                                vss::endpoint_id target)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::duplex)
        return vss::make_error_code(vss::errc::unsupported);
    if (find_port(target) == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->sources.isEmpty() || target == view->sources.first())
        return vss::make_error_code(vss::errc::invalid_argument);
    if (view->targets.contains(target))
        return vss::make_error_code(vss::errc::already_exists);
    view->targets.push_back(target);
    view->shape = link_shape::split;
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::remove_target(vss::link_id link,
                                                   vss::endpoint_id target)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::duplex)
        return vss::make_error_code(vss::errc::unsupported);
    if (find_port(target) == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->sources.isEmpty() || target == view->sources.first())
        return vss::make_error_code(vss::errc::invalid_argument);
    const qsizetype at = view->targets.indexOf(target);
    if (at < 0)
        return vss::make_error_code(vss::errc::not_found);
    if (view->targets.size() == 1)
        return vss::make_error_code(vss::errc::invalid_argument);
    view->targets.remove(at);
    if (view->owner == target)
        view->owner = 0;
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::add_source(vss::link_id link,
                                                vss::endpoint_id source)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::one_way)
        return vss::make_error_code(vss::errc::unsupported);
    if (find_port(source) == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->targets.contains(source))
        return vss::make_error_code(vss::errc::invalid_argument);
    if (view->sources.contains(source))
        return vss::make_error_code(vss::errc::already_exists);
    view->sources.push_back(source);
    set_one_way_shape(*view);
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::remove_source(vss::link_id link,
                                                   vss::endpoint_id source)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::one_way)
        return vss::make_error_code(vss::errc::unsupported);
    const qsizetype at = view->sources.indexOf(source);
    if (at < 0)
        return vss::make_error_code(vss::errc::not_found);
    if (view->sources.size() == 1)
        return vss::make_error_code(vss::errc::invalid_argument);
    view->sources.remove(at);
    if (view->owner == source)
        view->owner = 0;
    set_one_way_shape(*view);
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::add_member(vss::link_id link,
                                                vss::endpoint_id member)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::bus)
        return vss::make_error_code(vss::errc::unsupported);
    if (find_port(member) == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    for (const link_view& other : links_)
    {
        if (other.shape == link_shape::bus && other.sources.contains(member))
            return vss::make_error_code(vss::errc::already_exists);
    }
    if (static_cast<std::size_t>(view->sources.size()) >= vss::max_bus_members)
        return vss::make_error_code(vss::errc::invalid_argument);
    view->sources.push_back(member);
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::remove_member(vss::link_id link,
                                                   vss::endpoint_id member)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(link);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode != vss::link_mode::bus)
        return vss::make_error_code(vss::errc::unsupported);
    const qsizetype at = view->sources.indexOf(member);
    if (at < 0)
        return vss::make_error_code(vss::errc::not_found);
    if (view->sources.size() <= 2)
        return vss::make_error_code(vss::errc::invalid_argument);
    view->sources.remove(at);
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::set_owner(vss::link_id id,
                                               vss::endpoint_id owner)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (view->mode == vss::link_mode::bus)
        return vss::make_error_code(vss::errc::unsupported);
    // Like the engine: on a duplex link the owner arbitrates the routes from
    // the targets back to the source, so it is a target; on a one-way link it
    // is one of the sources.
    const bool allowed = view->mode == vss::link_mode::duplex
                             ? view->targets.contains(owner)
                             : view->sources.contains(owner);
    if (!allowed)
        return vss::make_error_code(vss::errc::invalid_argument);
    view->owner = owner;
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::release_owner(vss::link_id id)
{
    if (const auto error = take_error())
        return *error;
    link_view* view = find_link(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    view->owner = 0;
    refresh();
    return {};
}

std::optional<line_wiring> FakeStudioBackend::link_wiring(vss::link_id id) const
{
    const auto it = wiring_.constFind(id);
    if (it == wiring_.constEnd())
        return std::nullopt;
    return it.value();
}

vss::result<void> FakeStudioBackend::set_link_wiring(vss::link_id id,
                                                     const line_wiring& wiring)
{
    if (const auto error = take_error())
        return *error;
    if (find_link(id) == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!caps_.line_matrix)
        return vss::make_error_code(vss::errc::unsupported);
    wiring_.insert(id, wiring);
    refresh();
    return {};
}

vss::result<vss::benchmark_result> FakeStudioBackend::run_benchmark(
    vss::endpoint_id /*from*/, vss::endpoint_id /*to*/,
    const vss::benchmark_options& options,
    const vss::benchmark_progress_handler& progress,
    const std::atomic<bool>* cancel)
{
    // Runs on a worker thread, so it reads no port or link: the outcome is
    // whatever `set_benchmark_outcome` said.
    if (!caps_.benchmark)
        return vss::make_error_code(vss::errc::unsupported);
    benchmark_seen_ = options;

    vss::benchmark_result result;
    result.expected = options.bytes;
    const std::uint64_t arriving =
        options.bytes > benchmark_lost_ ? options.bytes - benchmark_lost_ : 0;
    constexpr int kSteps = 10;
    for (int step = 1; step <= kSteps; ++step)
    {
        if (cancel != nullptr && cancel->load())
        {
            result.cancelled = true;
            return result;
        }
        std::this_thread::sleep_for(benchmark_duration_ / kSteps);
        result.sent = options.bytes * static_cast<std::uint64_t>(step) / kSteps;
        result.received = arriving * static_cast<std::uint64_t>(step) / kSteps;
        if (progress)
            progress({result.sent, result.received, options.bytes});
    }
    result.elapsed = benchmark_duration_ > std::chrono::milliseconds(0)
                         ? benchmark_duration_
                         : std::chrono::milliseconds(1000);
    result.complete = result.received >= result.sent;
    return result;
}

vss::result<void> FakeStudioBackend::set_echo(vss::endpoint_id id, bool on)
{
    if (const auto error = take_error())
        return *error;
    const port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!caps_.fixture_echo || view->kind != vss::endpoint_kind::mock)
        return vss::make_error_code(vss::errc::unsupported);
    std::erase(echoing_, id);
    if (on)
        echoing_.push_back(id);
    return {};
}

bool FakeStudioBackend::echo(vss::endpoint_id id) const
{
    return std::find(echoing_.begin(), echoing_.end(), id) != echoing_.end();
}

vss::result<void> FakeStudioBackend::inject_fault(vss::endpoint_id id,
                                                  vss::fault_kind fault)
{
    if (const auto error = take_error())
        return *error;
    port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!caps_.faults || view->kind != vss::endpoint_kind::mock)
        return vss::make_error_code(vss::errc::unsupported);
    // The engine stops the endpoint on the next read; the fake does so at once.
    injected_.push_back(fault);
    view->state = vss::endpoint_state::failed;
    view->open = false;
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::send(vss::endpoint_id id,
                                          const QByteArray& data)
{
    if (const auto error = take_error())
        return *error;
    port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!view->open)
        return vss::make_error_code(vss::errc::not_open);
    sent_.push_back(data);
    sent_to_.push_back(id);
    view->stats.bytes_tx += static_cast<std::uint64_t>(data.size());
    view->stats.last_activity = vss::clock_type::now();
    traffic_view record;
    record.endpoint = id;
    record.name = view->name;
    record.direction = vss::data_direction::tx;
    record.data = data;
    record.when = view->stats.last_activity;
    traffic_.push_back(std::move(record));
    refresh();
    return {};
}

vss::result<void> FakeStudioBackend::set_line(vss::endpoint_id id,
                                              vss::control_line line,
                                              bool state)
{
    if (const auto error = take_error())
        return *error;
    port_view* view = find_port(id);
    if (view == nullptr)
        return vss::make_error_code(vss::errc::not_found);
    if (!view->supports_control_lines)
        return vss::make_error_code(vss::errc::unsupported);
    view->lines.set(line, state);
    Q_EMIT lines_changed(id);
    refresh();
    return {};
}

void FakeStudioBackend::refresh_lines(vss::endpoint_id id)
{
    if (find_port(id) != nullptr)
        Q_EMIT lines_changed(id);
}

vss::config::document FakeStudioBackend::snapshot() const
{
    vss::config::document document;
    document.name = "fake";

    for (const auto& view : ports_)
    {
        vss::config::port_entry entry;
        entry.name = view.name.toStdString();
        entry.serial = view.serial;
        switch (view.kind)
        {
        case vss::endpoint_kind::virtual_port:
            entry.kind = "virtual";
            entry.virtual_kind =
                view.path.startsWith(QStringLiteral("loopback:")) ? "loopback"
                                                                  : "auto";
            break;
        case vss::endpoint_kind::mock:
            entry.kind = "mock";
            entry.mock_target = view.path.toStdString();
            break;
        case vss::endpoint_kind::physical:
            entry.kind = "physical";
            entry.uri = view.path.toStdString();
            break;
        case vss::endpoint_kind::tcp:
            entry.kind = "tcp";
            entry.uri = view.path.toStdString();
            break;
        case vss::endpoint_kind::udp:
            entry.kind = "udp";
            entry.uri = view.path.toStdString();
            break;
        case vss::endpoint_kind::rfc2217:
            entry.kind = "rfc2217";
            entry.uri = view.path.toStdString();
            break;
        }
        document.ports.push_back(std::move(entry));
    }

    for (const auto& view : links_)
    {
        vss::config::link_entry entry;
        switch (view.shape)
        {
        case link_shape::two_way:
            entry.mode = "duplex";
            break;
        case link_shape::split:
            entry.mode = "split";
            break;
        case link_shape::join:
            entry.mode = "join";
            break;
        case link_shape::one_way:
            entry.mode = "redirect";
            break;
        case link_shape::monitor:
            entry.mode = "monitor";
            break;
        case link_shape::bus:
            entry.mode = "bus";
            break;
        }
        for (const vss::endpoint_id id : view.sources)
        {
            for (const auto& candidate : ports_)
            {
                if (candidate.id == id)
                {
                    (view.shape == link_shape::bus ? entry.members
                                                   : entry.sources)
                        .push_back(candidate.name.toStdString());
                }
            }
        }
        for (const vss::endpoint_id id : view.targets)
        {
            for (const auto& candidate : ports_)
            {
                if (candidate.id == id)
                    entry.targets.push_back(candidate.name.toStdString());
            }
        }
        entry.emulate_modem_lines = view.options.emulate_modem_lines;
        entry.policy = view.options.policy;
        document.links.push_back(std::move(entry));
    }
    return document;
}

vss::result<void>
FakeStudioBackend::apply(const vss::config::document& document, bool)
{
    // Fail first if a test queued an error, then reject an invalid document.
    // Nothing is changed until the whole document has been built.
    if (const auto error = take_error())
        return *error;
    if (const vss::error_code invalid = vss::config::validate(document))
        return invalid;

    // Build the new ports and links on the side.  names maps each port name
    // in the document to the id it was given, for resolving links.
    std::vector<port_view> next_ports;
    std::vector<link_view> next_links;
    std::vector<std::pair<std::string, vss::endpoint_id>> names;
    vss::endpoint_id next_port = 1;

    // Ports: every port starts open, numbered from 1.  A mock port has no
    // control lines.
    for (const auto& entry : document.ports)
    {
        port_view view;
        view.id = next_port++;
        view.name = QString::fromStdString(entry.name);
        view.serial = entry.serial;
        view.state = vss::endpoint_state::open;
        view.open = true;
        view.supports_control_lines = entry.kind != "mock";
        // The path shown depends on the kind: a loopback exists only inside
        // Studio, and a mock port is shown by its target.
        if (entry.kind == "virtual")
        {
            view.kind = vss::endpoint_kind::virtual_port;
            view.studio_only = entry.virtual_kind == "loopback";
            view.path = view.studio_only
                            ? QStringLiteral("loopback:%1").arg(view.name)
                            : QStringLiteral("auto:%1").arg(view.name);
        }
        else if (entry.kind == "mock")
        {
            view.kind = vss::endpoint_kind::mock;
            view.path = QString::fromStdString(entry.mock_target);
            view.studio_only = true;
        }
        else
        {
            // Physical and network ports use their uri as the path; the
            // kind comes from the document's kind name.
            view.path = QString::fromStdString(entry.uri);
            const auto kind = vss::config::to_endpoint_kind(entry.kind);
            if (!kind)
                return kind.error();
            view.kind = *kind;
        }
        names.emplace_back(entry.name, view.id);
        next_ports.push_back(std::move(view));
    }

    // Links: numbered from 1, with ports looked up by name.  A name that no
    // port has is a config error.
    vss::link_id next_link = 1;
    for (const auto& entry : document.links)
    {
        const auto resolve =
            [&names](const std::string& name) -> std::optional<vss::endpoint_id>
        {
            for (const auto& [candidate, id] : names)
            {
                if (candidate == name)
                    return id;
            }
            return std::nullopt;
        };

        link_view view;
        view.id = next_link++;
        view.options.emulate_modem_lines = entry.emulate_modem_lines;
        view.options.policy = entry.policy;
        // Map the document's mode name to a shape.  Any other mode is
        // one-way.  A bus needs a backend that supports shared buses.
        if (entry.mode == "duplex")
            view.shape = link_shape::two_way;
        else if (entry.mode == "split")
            view.shape = link_shape::split;
        else if (entry.mode == "join")
            view.shape = link_shape::join;
        else if (entry.mode == "monitor")
            view.shape = link_shape::monitor;
        else if (entry.mode == "bus")
        {
            if (!caps_.shared_bus)
                return vss::make_error_code(vss::errc::unsupported);
            view.shape = link_shape::bus;
        }
        else
            view.shape = link_shape::one_way;
        // Derive the direction of data flow from the shape: join, one-way
        // and monitor links run one way and the rest are duplex.
        view.mode = view.shape == link_shape::bus ? vss::link_mode::bus
                    : view.shape == link_shape::join ||
                            view.shape == link_shape::one_way ||
                            view.shape == link_shape::monitor
                        ? vss::link_mode::one_way
                        : vss::link_mode::duplex;

        // The members of a bus are stored as sources, since a bus has no
        // separate targets.
        for (const auto& name : entry.members)
        {
            const auto id = resolve(name);
            if (!id)
                return vss::make_error_code(vss::errc::config_error);
            view.sources.push_back(*id);
        }

        for (const auto& name : entry.sources)
        {
            const auto id = resolve(name);
            if (!id)
                return vss::make_error_code(vss::errc::config_error);
            view.sources.push_back(*id);
        }
        for (const auto& name : entry.targets)
        {
            const auto id = resolve(name);
            if (!id)
                return vss::make_error_code(vss::errc::config_error);
            view.targets.push_back(*id);
        }
        next_links.push_back(std::move(view));
    }

    // Success: replace the old state in one step, so a failure above leaves
    // it untouched.  The old wiring belongs to the old links, so drop it.
    ports_ = std::move(next_ports);
    links_ = std::move(next_links);
    wiring_.clear();
    next_port_ = next_port;
    next_link_ = next_link;
    refresh();
    return {};
}

vss::result<share_view> FakeStudioBackend::share_serial(const QString& uri,
                                                        int listeners)
{
    if (listeners < 1 || listeners > 64)
        return vss::make_error_code(vss::errc::invalid_argument);
    if (const auto error = take_error())
        return *error;

    const auto source = attach_serial(QStringLiteral("shared"), uri,
                                      vss::serial_settings{}, true);
    if (!source)
        return source.error();

    std::vector<vss::endpoint_id> created;
    for (int index = 0; index < listeners; ++index)
    {
        if (share_failure_ && share_failure_->first == index)
        {
            const vss::error_code error = share_failure_->second;
            share_failure_.reset();
            for (const vss::endpoint_id id : created)
                static_cast<void>(remove_port(id));
            static_cast<void>(remove_port(*source));
            return error;
        }
        const QString name = QStringLiteral("listener-%1").arg(index + 1);
        std::optional<vss::endpoint_id> listener;
        if (free_pairs_ < 0 || index < free_pairs_)
        {
            // Mirror a Windows auto port: open one end of a free driver pair
            // and advertise the other.
            port_view view;
            view.id = next_port_++;
            view.name = unique_name(name);
            view.kind = vss::endpoint_kind::virtual_port;
            view.state = vss::endpoint_state::open;
            view.open = true;
            view.supports_control_lines = true;
            view.path = QStringLiteral("COM%1").arg(next_com_number_++);
            view.description = QStringLiteral("Virtual port %1").arg(view.name);
            listener = view.id;
            ports_.push_back(std::move(view));
        }
        else
        {
            const auto made = create_virtual_port(
                name, vss::virtual_port_kind::loopback, true);
            if (!made)
            {
                for (const vss::endpoint_id id : created)
                    static_cast<void>(remove_port(id));
                static_cast<void>(remove_port(*source));
                return made.error();
            }
            listener = *made;
        }
        created.push_back(*listener);
    }

    const auto link = split(*source, created, vss::link_options{});
    if (!link)
    {
        for (const vss::endpoint_id id : created)
            static_cast<void>(remove_port(id));
        static_cast<void>(remove_port(*source));
        return link.error();
    }

    share_view view;
    view.source = *source;
    view.link = *link;
    view.listeners.reserve(static_cast<qsizetype>(created.size()));
    for (const vss::endpoint_id id : created)
        view.listeners.push_back(id);
    return view;
}

vss::result<vss::link_id> FakeStudioBackend::bridge(const QString& from_uri,
                                                    const QString& to_uri)
{
    if (const auto error = take_error())
        return *error;

    const auto from = attach_serial(QStringLiteral("bridge-a"), from_uri,
                                    vss::serial_settings{}, true);
    if (!from)
        return from.error();
    const auto to = attach_serial(QStringLiteral("bridge-b"), to_uri,
                                  vss::serial_settings{}, true);
    if (!to)
    {
        static_cast<void>(remove_port(*from));
        return to.error();
    }
    const auto link = connect_ports(*from, *to, vss::link_options{});
    if (!link)
    {
        static_cast<void>(remove_port(*from));
        static_cast<void>(remove_port(*to));
        return link.error();
    }
    return *link;
}

vss::result<void> FakeStudioBackend::create_com0com_pair()
{
    if (const auto error = take_error())
        return *error;

    int next_pair = 0;
    int highest_com = 0;
    for (const auto& device : devices_)
    {
        if (device.com0com && device.com0com_pair >= 0)
            next_pair = std::max(next_pair, device.com0com_pair + 1);
        const QString path = device.path;
        if (path.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive))
        {
            bool ok = false;
            const int number = path.mid(3).toInt(&ok);
            if (ok)
                highest_com = std::max(highest_com, number);
        }
    }
    next_com_number_ = std::max(next_com_number_, highest_com + 1);

    device_view first;
    first.path = QStringLiteral("COM%1").arg(next_com_number_++);
    first.description = QStringLiteral("com0com pair end");
    first.virtual_device = true;
    first.com0com = true;
    first.com0com_pair = next_pair;
    first.com0com_side = QStringLiteral("A");
    devices_.push_back(first);

    device_view second;
    second.path = QStringLiteral("COM%1").arg(next_com_number_++);
    second.description = QStringLiteral("com0com pair end");
    second.virtual_device = true;
    second.com0com = true;
    second.com0com_pair = next_pair;
    second.com0com_side = QStringLiteral("B");
    devices_.push_back(second);

    facts_.com0com_found = true;
    facts_.com0com_pairs += 1;
    return {};
}

void FakeStudioBackend::refresh()
{
    Q_EMIT ports_changed();
    Q_EMIT links_changed();
}

void FakeStudioBackend::set_refresh_interval(int milliseconds)
{
    refresh_interval_ms_ = milliseconds;
}

} // namespace vss::gui
