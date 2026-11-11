//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "bus_wire.hpp"
#include "delay_line.hpp"

#include <vss/core/bus_line.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/profile.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace vss {
namespace {

bool contains(const std::vector<endpoint_id>& values, endpoint_id value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

/// Whether `id` may own a link under `merge_policy::owner`: it is one of the
/// endpoints whose routes meet at one endpoint.  A one-way link merges its
/// sources into its target; a duplex link merges its targets' replies into
/// its one source.
bool may_own(link_mode mode, const std::vector<endpoint_id>& sources,
             const std::vector<endpoint_id>& targets, endpoint_id id)
{
    return contains(mode == link_mode::duplex ? targets : sources, id);
}

/// Marks a node of the cycle graph as a bus rather than an endpoint.  Endpoint
/// and link ids count up from one, so they never reach this bit.
constexpr endpoint_id bus_node_tag = endpoint_id{1} << 63;

/// What a half-duplex bus says about a chunk that was not a clean
/// transmission.  `other` is the member that was in the way, or empty.
std::string describe_fault(bus_fault fault, std::size_t bytes,
                           const std::string& other, bool garble)
{
    std::string sent =
        "sent " + std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
    switch (fault)
    {
    case bus_fault::overlap:
        return sent + " while " + other + " was sending" +
               (garble ? ": both were garbled" : "");
    case bus_fault::too_soon:
        return sent + " inside the turn-around time after " + other +
               (garble ? ": garbled" : "");
    case bus_fault::line_held:
        return sent + " while " + other + " held RTS high" +
               (garble ? ": garbled" : "");
    case bus_fault::transmitter_off:
        return sent + " with RTS low: the bus did not carry them";
    case bus_fault::overrun:
        return sent + " too far ahead of the line rate: dropped";
    case bus_fault::none:
        break;
    }
    return sent;
}

unsigned default_thread_count()
{
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 2u : std::max(2u, hardware);
}

} // namespace

hub::hub(hub_options options)
    : options_(std::move(options))
    , event_strand_(boost::asio::make_strand(io_))
    , traffic_(options_.traffic_capacity)
{
}

hub::~hub()
{
    stop();
    join();
}

void hub::start()
{
    if (running_.exchange(true))
        return;

    stopping_.store(false);
    io_.restart();
    work_ = std::make_unique<
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        io_.get_executor());

    const unsigned count =
        options_.io_threads == 0 ? default_thread_count() : options_.io_threads;
    threads_.reserve(count);
    VSS_PROFILE_MESSAGE("hub.started");
    for (unsigned index = 0; index < count; ++index)
    {
        threads_.emplace_back(
            [this]
            {
                VSS_PROFILE_THREAD("vss.hub.io");
                io_.run();
            });
    }
}

void hub::stop()
{
    if (!running_.exchange(false))
        return;

    VSS_PROFILE_ZONE("hub.stop");
    VSS_PROFILE_MESSAGE("hub.stopping");

    // The work guard stays until join(). Releasing it here would let the worker
    // threads leave an idle io_context before the shutdown handlers below are
    // posted, so those handlers would never run and an endpoint with a parked
    // read would keep itself alive through its transport.

    // Ask every endpoint to close itself on its own strand: transports are
    // single-threaded objects, so closing them from this thread while a read
    // is in flight would race the serial library.  The io_context is stopped
    // once every shutdown has finished, including the ones remove_endpoint()
    // started earlier, so a transport that never completes cannot hang
    // shutdown and a queued shutdown is never thrown away.
    std::vector<std::shared_ptr<endpoint>> endpoints;
    {
        std::shared_lock lock(mutex_);
        endpoints.reserve(endpoints_.size());
        for (const auto& entry : endpoints_)
            endpoints.push_back(entry.second);
    }

    closing_.fetch_add(endpoints.size());
    stopping_.store(true);
    if (closing_.load() == 0)
        io_.stop();

    for (const auto& entry : endpoints)
        entry->shutdown([this] { endpoint_closed(); });
}

time_point hub::now() const
{
    return options_.clock ? options_.clock() : clock_type::now();
}

void hub::endpoint_closed()
{
    if (closing_.fetch_sub(1) == 1 && stopping_.load())
        io_.stop();
}

void hub::join()
{
    VSS_PROFILE_ZONE("hub.join");
    for (auto& thread : threads_)
    {
        if (thread.joinable())
            thread.join();
    }
    threads_.clear();
    work_.reset();
}

result<std::shared_ptr<endpoint>> hub::add_endpoint(
    const std::string& name, endpoint_kind kind,
    std::shared_ptr<transport> transport,
    const endpoint_options& options)
{
    VSS_PROFILE_ZONE_NAMED(zone, "hub.add_endpoint");
    if (zone.active())
        zone.text(name);
    if (name.empty())
        return make_error_code(errc::invalid_argument);
    if (!transport)
        return make_error_code(errc::invalid_argument);

    std::shared_ptr<endpoint> created;
    {
        std::unique_lock lock(mutex_);
        if (names_.find(name) != names_.end())
            return make_error_code(errc::already_exists);
        if (endpoints_.size() >= 4096)
            return make_error_code(errc::port_exhausted);

        const endpoint_id id = next_endpoint_id_++;
        created = std::shared_ptr<endpoint>(
            new endpoint(*this, id, name, kind, std::move(transport), options));
        endpoints_.emplace(id, created);
        names_.emplace(name, id);
        VSS_PROFILE_PLOT("hub.endpoints", endpoints_.size());
    }

    VSS_PROFILE_MESSAGE_TEXT(std::string("hub.endpoint_added ") + name);
    created->start();
    return created;
}

result<void> hub::remove_endpoint(endpoint_id id)
{
    VSS_PROFILE_ZONE("hub.remove_endpoint");
    std::shared_ptr<endpoint> victim;
    {
        std::unique_lock lock(mutex_);
        auto it = endpoints_.find(id);
        if (it == endpoints_.end())
            return make_error_code(errc::not_found);

        victim = it->second;

        for (auto link = links_.begin(); link != links_.end();)
        {
            auto& record = *link->second;
            if (record.mode == link_mode::bus)
            {
                // A bus loses the member and goes on; with fewer than two
                // left it is not a bus any more.
                if (!contains(record.sources, id))
                {
                    ++link;
                    continue;
                }
                record.sources.erase(std::remove(record.sources.begin(),
                                                 record.sources.end(), id),
                                     record.sources.end());
                bus_of_.erase(id);
                if (record.line)
                {
                    record.line->remove(id);
                    record.wire->forget(id);
                }
                if (record.sources.size() >= 2)
                {
                    ++link;
                    continue;
                }
                for (endpoint_id member : record.sources)
                    bus_of_.erase(member);
                const link_id removed = link->first;
                link = links_.erase(link);
                erase_routes(removed);
            }
            else if (contains(record.sources, id) ||
                     contains(record.targets, id))
            {
                const link_id removed = link->first;
                link = links_.erase(link);
                erase_routes(removed);
            }
            else
            {
                ++link;
            }
        }

        routes_.erase(id);
        for (auto& entry : routes_)
        {
            auto& routes = entry.second;
            routes.erase(std::remove_if(routes.begin(), routes.end(),
                                        [id](const route_entry& route) {
                                            return route.to == id;
                                        }),
                         routes.end());
        }

        names_.erase(victim->name());
        endpoints_.erase(it);
        VSS_PROFILE_PLOT("hub.endpoints", endpoints_.size());
        VSS_PROFILE_PLOT("hub.links", links_.size());
    }
    VSS_PROFILE_MESSAGE_TEXT(std::string("hub.endpoint_removed ") +
                             victim->name());

    closing_.fetch_add(1);
    victim->shutdown([this] { endpoint_closed(); });
    return {};
}

result<void> hub::rename_endpoint(endpoint_id id, const std::string& name)
{
    VSS_PROFILE_ZONE("hub.rename_endpoint");
    if (name.empty())
        return make_error_code(errc::invalid_argument);

    std::unique_lock lock(mutex_);
    auto it = endpoints_.find(id);
    if (it == endpoints_.end())
        return make_error_code(errc::not_found);
    if (names_.find(name) != names_.end())
        return make_error_code(errc::already_exists);

    names_.erase(it->second->name());
    names_.emplace(name, id);
    it->second->set_name(name);
    return {};
}

std::shared_ptr<endpoint> hub::find_endpoint(endpoint_id id) const
{
    std::shared_lock lock(mutex_);
    auto it = endpoints_.find(id);
    return it == endpoints_.end() ? nullptr : it->second;
}

std::shared_ptr<endpoint> hub::find_endpoint(std::string_view name) const
{
    std::shared_lock lock(mutex_);
    auto it = names_.find(std::string(name));
    if (it == names_.end())
        return nullptr;
    auto endpoint_it = endpoints_.find(it->second);
    return endpoint_it == endpoints_.end() ? nullptr : endpoint_it->second;
}

std::vector<std::shared_ptr<endpoint>> hub::endpoints() const
{
    std::shared_lock lock(mutex_);
    std::vector<std::shared_ptr<endpoint>> out;
    out.reserve(endpoints_.size());
    for (const auto& entry : endpoints_)
        out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const auto& lhs, const auto& rhs) { return lhs->id() < rhs->id(); });
    return out;
}

result<link_id> hub::connect(endpoint_id a, endpoint_id b, link_options options)
{
    return add_link(link_mode::duplex, {a}, {b}, options);
}

result<link_id> hub::split(endpoint_id source,
                           const std::vector<endpoint_id>& targets,
                           link_options options)
{
    return add_link(link_mode::duplex, {source}, targets, options);
}

result<link_id> hub::join(const std::vector<endpoint_id>& sources,
                          endpoint_id target, link_options options)
{
    return add_link(link_mode::one_way, sources, {target}, options);
}

result<link_id> hub::redirect(endpoint_id from, endpoint_id to,
                              link_options options)
{
    return add_link(link_mode::one_way, {from}, {to}, options);
}

result<link_id> hub::monitor(endpoint_id from, endpoint_id to)
{
    link_options options;
    options.emulate_modem_lines = false;
    return add_link(link_mode::one_way, {from}, {to}, options);
}

result<link_id> hub::bus(const std::vector<endpoint_id>& members,
                         link_options options)
{
    VSS_PROFILE_ZONE("hub.add_bus");
    if (members.size() < 2 || members.size() > max_bus_members)
        return make_error_code(errc::invalid_argument);
    if (options.policy != merge_policy::interleave)
        return make_error_code(errc::unsupported);
    if (!valid_latency(options) || !valid_half_duplex(options.half_duplex))
        return make_error_code(errc::invalid_argument);

    const std::unordered_set<endpoint_id> unique(members.begin(),
                                                 members.end());
    if (unique.size() != members.size())
        return make_error_code(errc::invalid_argument);

    // A multi-drop wire pair has no modem lines to cross.
    options.emulate_modem_lines = false;

    std::unique_lock lock(mutex_);

    for (endpoint_id id : members)
    {
        if (endpoints_.find(id) == endpoints_.end())
            return make_error_code(errc::not_found);
        if (bus_of_.find(id) != bus_of_.end())
            return make_error_code(errc::already_exists);
    }
    if (bus_would_create_cycle(members))
        return make_error_code(errc::cycle_detected);

    auto record = std::make_shared<link_record>();
    record->id = next_link_id_++;
    record->mode = link_mode::bus;
    record->options = options;
    record->sources = members;
    if (options.half_duplex.enabled)
    {
        record->line = std::make_shared<bus_line>(options.half_duplex);
        record->wire = std::make_shared<bus_wire>(
            io_.get_executor(), options_.clock, record->line->garbled());
        for (endpoint_id id : members)
            record->line->set_rts(id, endpoints_.at(id)->peer_rts());
    }

    links_.emplace(record->id, record);
    VSS_PROFILE_PLOT("hub.links", links_.size());
    VSS_PROFILE_MESSAGE("hub.bus_added");

    for (endpoint_id from : members)
    {
        bus_of_.emplace(from, record->id);
        for (endpoint_id to : members)
        {
            if (from != to)
                add_route(*record, from, to);
        }
    }

    return record->id;
}

result<link_id> hub::add_link(link_mode mode,
                              const std::vector<endpoint_id>& sources,
                              const std::vector<endpoint_id>& targets,
                              link_options options)
{
    VSS_PROFILE_ZONE("hub.add_link");
    if (sources.empty() || targets.empty())
        return make_error_code(errc::invalid_argument);
    if (!valid_latency(options))
        return make_error_code(errc::invalid_argument);
    // Only a bus is one wire that its members take turns on.
    if (options.half_duplex.enabled)
        return make_error_code(errc::unsupported);

    std::unique_lock lock(mutex_);

    for (endpoint_id id : sources)
    {
        if (endpoints_.find(id) == endpoints_.end())
            return make_error_code(errc::not_found);
        if (contains(targets, id))
            return make_error_code(errc::invalid_argument);
    }
    for (endpoint_id id : targets)
    {
        if (endpoints_.find(id) == endpoints_.end())
            return make_error_code(errc::not_found);
    }

    const bool duplex = mode == link_mode::duplex;
    for (endpoint_id source : sources)
    {
        for (endpoint_id target : targets)
        {
            if (would_create_cycle(source, target)
                || (duplex && would_create_cycle(target, source)))
                return make_error_code(errc::cycle_detected);
        }
    }

    auto record = std::make_shared<link_record>();
    record->id = next_link_id_++;
    record->mode = mode;
    record->options = options;
    record->sources = sources;
    record->targets = targets;

    links_.emplace(record->id, record);
    VSS_PROFILE_PLOT("hub.links", links_.size());
    VSS_PROFILE_MESSAGE("hub.link_added");

    for (endpoint_id source : sources)
    {
        for (endpoint_id target : targets)
        {
            add_route(*record, source, target);
            if (duplex)
                add_route(*record, target, source);
        }
    }

    return record->id;
}

endpoint_id hub::cycle_node(endpoint_id id) const
{
    const auto it = bus_of_.find(id);
    return it == bus_of_.end() ? id : (bus_node_tag | it->second);
}

bool hub::reaches(endpoint_id start,
                  const std::unordered_set<endpoint_id>& goals) const
{
    VSS_PROFILE_ZONE("hub.cycle_check");
    std::unordered_set<endpoint_id> visited;
    std::vector<endpoint_id> pending{start};

    while (!pending.empty())
    {
        const endpoint_id current = pending.back();
        pending.pop_back();
        if (goals.count(current) != 0)
            return true;
        if (!visited.insert(current).second)
            continue;

        // A bus node stands for all of its members; any of them may leave it.
        std::vector<endpoint_id> members{current};
        if ((current & bus_node_tag) != 0)
        {
            const auto bus = links_.find(current & ~bus_node_tag);
            if (bus == links_.end())
                continue;
            members = bus->second->sources;
        }

        for (endpoint_id member : members)
        {
            const auto it = routes_.find(member);
            if (it == routes_.end())
                continue;
            for (const auto& route : it->second)
                pending.push_back(cycle_node(route.to));
        }
    }
    return false;
}

bool hub::would_create_cycle(endpoint_id from, endpoint_id to) const
{
    // Endpoints of one bus are one node, so a route between two of them
    // (which would deliver twice) is refused like a route to itself.
    return reaches(cycle_node(to), {cycle_node(from)});
}

bool hub::bus_would_create_cycle(const std::vector<endpoint_id>& members) const
{
    // The members become one node.  Any existing route that leaves one of
    // them and arrives at one of them again, directly or through other links,
    // would be a loop through the bus.  None of the members is in a bus yet,
    // so each stands for itself.
    const std::unordered_set<endpoint_id> goals(members.begin(), members.end());
    for (endpoint_id member : members)
    {
        const auto it = routes_.find(member);
        if (it == routes_.end())
            continue;
        for (const auto& route : it->second)
        {
            if (reaches(cycle_node(route.to), goals))
                return true;
        }
    }
    return false;
}

hub::route_entry hub::make_route(const link_record& link, endpoint_id from,
                                 endpoint_id to)
{
    std::shared_ptr<delay_line> delay;
    if (link.options.has_latency())
    {
        // The caller holds the lock and has checked that `to` exists.
        const auto target = endpoints_.find(to);
        if (target != endpoints_.end())
            delay = std::make_shared<delay_line>(
                io_.get_executor(), target->second, link.options.delay_ms,
                link.options.jitter_ms);
    }
    // The owner policy arbitrates the routes that meet at one endpoint: all
    // of a one-way link's, and on a duplex link the ones from its targets.
    // What the single source of a duplex link sends is never arbitrated.
    const bool arbitrated =
        link.options.policy == merge_policy::owner &&
        may_own(link.mode, link.sources, link.targets, from);
    return route_entry{link.id, to, std::move(delay), arbitrated};
}

void hub::add_route(const link_record& link, endpoint_id from, endpoint_id to)
{
    routes_[from].push_back(make_route(link, from, to));
}

result<void> hub::add_target(link_id id, endpoint_id target)
{
    VSS_PROFILE_ZONE("hub.add_target");
    std::shared_ptr<endpoint> source_end;
    std::shared_ptr<endpoint> target_end;
    modem_wiring wiring;
    {
        std::unique_lock lock(mutex_);
        const auto found = links_.find(id);
        if (found == links_.end())
            return make_error_code(errc::not_found);
        auto& record = *found->second;
        if (record.mode != link_mode::duplex)
            return make_error_code(errc::unsupported);
        const auto added = endpoints_.find(target);
        if (added == endpoints_.end())
            return make_error_code(errc::not_found);

        // A duplex link has one source: connect() and split() make no other.
        const endpoint_id source = record.sources.front();
        if (target == source)
            return make_error_code(errc::invalid_argument);
        if (contains(record.targets, target))
            return make_error_code(errc::already_exists);
        if (would_create_cycle(source, target) ||
            would_create_cycle(target, source))
            return make_error_code(errc::cycle_detected);

        // Everything that can fail happens before the first change, so a
        // refusal (or an allocation that fails) leaves no half of a route.
        // The target joins the list first: its reply route is arbitrated by
        // the owner policy only once it is one of the targets.
        record.targets.reserve(record.targets.size() + 1);
        auto& forward_routes = routes_[source];
        forward_routes.reserve(forward_routes.size() + 1);
        auto& reply_routes = routes_[target];
        reply_routes.reserve(reply_routes.size() + 1);
        record.targets.push_back(target);
        route_entry forward;
        route_entry reply;
        try
        {
            forward = make_route(record, source, target);
            reply = make_route(record, target, source);
        }
        catch (...)
        {
            record.targets.pop_back();
            throw;
        }
        // Room was reserved above, so neither of these can fail.  (A rehash
        // of the map moves no element, so both references are still good.)
        forward_routes.push_back(std::move(forward));
        reply_routes.push_back(std::move(reply));
        VSS_PROFILE_MESSAGE("hub.target_added");

        if (record.options.emulate_modem_lines)
        {
            const auto origin = endpoints_.find(source);
            if (origin != endpoints_.end())
                source_end = origin->second;
            target_end = added->second;
            wiring = record.options.wiring;
        }
    }

    // The new target must see what the source drives now, not at the
    // source's next change.
    if (source_end && target_end)
        target_end->set_peer_inputs(wiring.apply(source_end->lines()));
    return {};
}

result<void> hub::remove_target(link_id id, endpoint_id target)
{
    VSS_PROFILE_ZONE("hub.remove_target");

    struct line_driver
    {
        std::shared_ptr<endpoint> from;
        modem_wiring wiring;
    };

    std::vector<std::shared_ptr<delay_line>> held;
    std::shared_ptr<endpoint> source_end;
    std::shared_ptr<endpoint> target_end;
    // What still drives the removed target's inputs, and the source's.
    std::vector<line_driver> target_drivers;
    std::vector<line_driver> source_drivers;
    {
        std::unique_lock lock(mutex_);
        const auto found = links_.find(id);
        if (found == links_.end())
            return make_error_code(errc::not_found);
        auto& record = *found->second;
        if (record.mode != link_mode::duplex)
            return make_error_code(errc::unsupported);
        const endpoint_id source = record.sources.front();
        if (target == source)
            return make_error_code(errc::invalid_argument);
        if (!contains(record.targets, target))
            return make_error_code(errc::not_found);
        if (record.targets.size() == 1)
            return make_error_code(errc::invalid_argument);

        record.targets.erase(
            std::remove(record.targets.begin(), record.targets.end(), target),
            record.targets.end());

        const auto take = [&](endpoint_id from, endpoint_id to)
        {
            const auto it = routes_.find(from);
            if (it == routes_.end())
                return;
            auto& routes = it->second;
            const auto leaves = [&](const route_entry& route)
            { return route.link == id && route.to == to; };
            // The delay lines are taken before the entries are removed:
            // what remove_if leaves behind it has been moved from.
            for (auto& route : routes)
            {
                if (leaves(route) && route.delay)
                    held.push_back(std::move(route.delay));
            }
            routes.erase(std::remove_if(routes.begin(), routes.end(), leaves),
                         routes.end());
            if (routes.empty())
                routes_.erase(it);
        };
        take(source, target);
        take(target, source);

        // An owner that is no longer on the link would silence it for good.
        endpoint_id owner = target;
        record.owner.compare_exchange_strong(owner, 0);
        VSS_PROFILE_MESSAGE("hub.target_removed");

        if (record.options.emulate_modem_lines)
        {
            const auto end_of = [&](endpoint_id which)
            {
                const auto it = endpoints_.find(which);
                return it == endpoints_.end() ? nullptr : it->second;
            };
            source_end = end_of(source);
            target_end = end_of(target);
            for (const endpoint_id other : record.targets)
            {
                if (auto from = end_of(other))
                    source_drivers.push_back(
                        {std::move(from), record.options.wiring});
            }
            for (const auto& [from, routes] : routes_)
            {
                for (const auto& route : routes)
                {
                    if (route.to != target)
                        continue;
                    const auto link = links_.find(route.link);
                    if (link == links_.end() ||
                        !link->second->options.emulate_modem_lines)
                        continue;
                    if (auto driver = end_of(from))
                        target_drivers.push_back(
                            {std::move(driver), link->second->options.wiring});
                }
            }
        }
    }

    // After this nothing the removed routes still held can reach an
    // endpoint, so a target that is added again starts from an empty line.
    for (const auto& line : held)
        line->close();

    if (target_end)
    {
        // The cable is out: the inputs it drove drop, unless another link
        // drives them, and then they show that link's sender.
        if (target_drivers.empty())
            target_end->set_peer_inputs(control_lines{});
        for (const auto& driver : target_drivers)
            target_end->set_peer_inputs(
                driver.wiring.apply(driver.from->lines()));
    }
    if (source_end)
    {
        // The source may have been showing the removed target's lines.
        for (const auto& driver : source_drivers)
            source_end->set_peer_inputs(
                driver.wiring.apply(driver.from->lines()));
    }
    return {};
}

void hub::erase_routes(link_id link)
{
    for (auto& entry : routes_)
    {
        auto& routes = entry.second;
        routes.erase(std::remove_if(routes.begin(), routes.end(),
                                    [link](const route_entry& route) {
                                        return route.link == link;
                                    }),
                     routes.end());
    }
}

result<void> hub::disconnect(link_id id)
{
    VSS_PROFILE_ZONE("hub.disconnect");
    std::unique_lock lock(mutex_);
    const auto found = links_.find(id);
    if (found == links_.end())
        return make_error_code(errc::not_found);
    if (found->second->mode == link_mode::bus)
    {
        for (endpoint_id member : found->second->sources)
            bus_of_.erase(member);
    }
    links_.erase(found);
    erase_routes(id);
    VSS_PROFILE_PLOT("hub.links", links_.size());
    VSS_PROFILE_MESSAGE("hub.link_removed");
    return {};
}

link_info hub::describe(const link_record& record) const
{
    link_info info;
    info.id = record.id;
    info.mode = record.mode;
    info.options = record.options;
    info.sources = record.sources;
    info.targets = record.targets;
    info.owner = record.owner.load();
    info.stats.bytes = record.bytes.load();
    info.stats.messages = record.messages.load();
    info.stats.dropped = record.dropped.load();
    if (record.line)
    {
        info.stats.collisions = record.line->collisions();
        info.stats.garbled = record.line->garbled()->load();
    }
    return info;
}

std::vector<link_info> hub::links() const
{
    std::shared_lock lock(mutex_);
    std::vector<link_info> out;
    out.reserve(links_.size());
    for (const auto& entry : links_)
        out.push_back(describe(*entry.second));
    std::sort(out.begin(), out.end(),
              [](const link_info& lhs, const link_info& rhs) { return lhs.id < rhs.id; });
    return out;
}

std::optional<link_info> hub::find_link(link_id id) const
{
    std::shared_lock lock(mutex_);
    auto it = links_.find(id);
    if (it == links_.end())
        return std::nullopt;
    return describe(*it->second);
}

result<void> hub::set_owner(link_id id, endpoint_id owner)
{
    std::unique_lock lock(mutex_);
    auto it = links_.find(id);
    if (it == links_.end())
        return make_error_code(errc::not_found);
    auto& record = *it->second;
    if (record.mode == link_mode::bus)
        return make_error_code(errc::unsupported);
    if (!may_own(record.mode, record.sources, record.targets, owner))
        return make_error_code(errc::invalid_argument);
    record.owner.store(owner);
    return {};
}

result<void> hub::release_owner(link_id id)
{
    std::unique_lock lock(mutex_);
    auto it = links_.find(id);
    if (it == links_.end())
        return make_error_code(errc::not_found);
    it->second->owner.store(0);
    return {};
}

result<void> hub::set_link_wiring(link_id id, const modem_wiring& wiring)
{
    VSS_PROFILE_ZONE("hub.set_link_wiring");
    std::vector<std::pair<endpoint_id, control_lines>> senders;
    {
        std::unique_lock lock(mutex_);
        auto it = links_.find(id);
        if (it == links_.end())
            return make_error_code(errc::not_found);
        if (it->second->mode == link_mode::bus)
            return make_error_code(errc::unsupported);
        it->second->options.wiring = wiring;

        for (const auto& [from, routes] : routes_)
        {
            const bool uses = std::any_of(
                routes.begin(), routes.end(),
                [id](const route_entry& route) { return route.link == id; });
            if (!uses)
                continue;
            const auto sender = endpoints_.find(from);
            if (sender != endpoints_.end())
                senders.emplace_back(from, sender->second->lines());
        }
    }

    // The peers must see what the new wires carry now, not at the next change.
    for (const auto& [from, lines] : senders)
        route_lines(from, lines);
    return {};
}

void hub::route_data(endpoint_id from, bytes_view data)
{
    VSS_PROFILE_ZONE("hub.route");

    if (data.empty())
        return;
    VSS_PROFILE_PLOT("hub.route_bytes", data.size());

    struct delivery
    {
        std::shared_ptr<endpoint> target;
        std::shared_ptr<delay_line> delay;
    };

    std::vector<delivery> deliveries;

    // For a sender on a half-duplex bus: what the line said of the chunk,
    // and where the wire takes it.  An endpoint is on one bus at most.
    std::shared_ptr<bus_wire> wire;
    std::vector<bus_wire::delivery> on_wire;
    bus_verdict verdict;
    std::optional<endpoint_event> fault;
    {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(from);
        if (it == routes_.end())
            return;

        for (const auto& route : it->second)
        {
            auto link_it = links_.find(route.link);
            if (link_it == links_.end())
                continue;
            auto& record = *link_it->second;

            if (route.arbitrated)
            {
                // A link with no owner goes to the first publisher on one
                // of its arbitrated routes.  A failed exchange leaves the
                // winner in `owner`.
                endpoint_id owner = record.owner.load();
                if (owner == 0 &&
                    record.owner.compare_exchange_strong(owner, from))
                    owner = from;
                if (owner != from)
                {
                    record.dropped.fetch_add(data.size());
                    continue;
                }
            }

            if (record.line)
            {
                // One verdict for the chunk, however many members hear it.
                if (!wire)
                {
                    wire = record.wire;
                    verdict = record.line->transmit(from, data.size(), now());
                    if (verdict.fault != bus_fault::none)
                    {
                        const auto name_of = [this](endpoint_id id)
                        {
                            const auto found = endpoints_.find(id);
                            return found == endpoints_.end()
                                       ? std::string()
                                       : found->second->name();
                        };
                        fault.emplace();
                        fault->kind = event_kind::bus_fault;
                        fault->endpoint = from;
                        fault->name = name_of(from);
                        fault->fault = verdict.fault;
                        fault->link = record.id;
                        fault->message = describe_fault(
                            verdict.fault, data.size(), name_of(verdict.other),
                            record.options.half_duplex.on_collision ==
                                collision_action::garble);
                    }
                }
                if (!verdict.carried)
                {
                    record.dropped.fetch_add(data.size());
                    continue;
                }
                record.bytes.fetch_add(data.size());
                record.messages.fetch_add(1);

                auto target = endpoints_.find(route.to);
                if (target != endpoints_.end())
                    on_wire.push_back({target->second, route.delay});
                continue;
            }

            record.bytes.fetch_add(data.size());
            record.messages.fetch_add(1);

            auto target = endpoints_.find(route.to);
            if (target != endpoints_.end())
                deliveries.push_back({target->second, route.delay});
        }
    }

    for (const auto& entry : deliveries)
    {
        if (entry.delay)
            entry.delay->push(data);
        else
            entry.target->write(data);
    }

    if (wire && !on_wire.empty())
        wire->push(from, verdict.ends, data, std::move(verdict.transmission),
                   std::move(on_wire));
    if (fault)
    {
        fault->when = clock_type::now();
        publish(std::move(*fault));
    }
}

void hub::route_lines(endpoint_id from, control_lines lines)
{
    VSS_PROFILE_ZONE("hub.route_lines");
    struct update
    {
        std::shared_ptr<endpoint> target;
        control_lines inputs;
    };

    std::vector<update> updates;
    {
        std::shared_lock lock(mutex_);

        // A half-duplex bus carries no lines either, but its line is told
        // whether this member's transmitter is on.
        if (const auto bus = bus_of_.find(from); bus != bus_of_.end())
        {
            const auto record = links_.find(bus->second);
            const auto sender = endpoints_.find(from);
            if (record != links_.end() && record->second->line &&
                sender != endpoints_.end())
            {
                record->second->line->set_rts(
                    from, sender->second->peer_rts().has_value()
                              ? std::optional<bool>(lines.rts())
                              : std::nullopt);
            }
        }

        auto it = routes_.find(from);
        if (it == routes_.end())
            return;

        for (const auto& route : it->second)
        {
            auto link_it = links_.find(route.link);
            if (link_it == links_.end())
                continue;
            if (!link_it->second->options.emulate_modem_lines)
                continue;
            auto target = endpoints_.find(route.to);
            if (target != endpoints_.end())
                updates.push_back(
                    {target->second,
                     link_it->second->options.wiring.apply(lines)});
        }
    }

    for (const auto& update : updates)
        update.target->set_peer_inputs(update.inputs);
}

void hub::publish(endpoint_event event)
{
    if (event.kind == event_kind::data && event.data)
    {
        traffic_.push(traffic_record{event.endpoint, event.name,
                                     event.direction, event.data, event.when});
    }
    // The counter moves whether or not anyone is recording, so it stays
    // balanced when recording starts between a post and its delivery.
    [[maybe_unused]] const auto queued = pending_events_.up();
    VSS_PROFILE_PLOT("hub.dispatch_queue", queued);
    boost::asio::post(event_strand_,
                      [this, event = std::move(event)]() mutable
                      {
                          [[maybe_unused]] const auto left =
                              pending_events_.down();
                          VSS_PROFILE_PLOT("hub.dispatch_queue", left);
                          dispatch(event);
                      });
}

void hub::dispatch(const endpoint_event& event)
{
    VSS_PROFILE_ZONE("hub.dispatch");

    // Once stop() has been called the subscribers may already be tearing down
    // their own state; delivering a late completion would run a sink over
    // dangling captures.  Shutdown traffic is dropped deliberately.
    if (!running_.load())
        return;

    std::vector<subscription> snapshot;
    {
        std::lock_guard lock(subscriber_mutex_);
        snapshot = subscribers_;
    }

    for (const auto& entry : snapshot)
    {
        if (!entry.filter.matches(event))
            continue;
        try
        {
            entry.sink(event);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // A subscriber must never take the hub down.
        }
    }
}

subscriber_id hub::subscribe(event_filter filter, event_sink sink)
{
    VSS_PROFILE_ZONE("hub.subscribe");
    std::lock_guard lock(subscriber_mutex_);
    const subscriber_id id = next_subscriber_id_++;
    subscribers_.push_back(subscription{id, filter, std::move(sink)});
    return id;
}

void hub::unsubscribe(subscriber_id id)
{
    std::lock_guard lock(subscriber_mutex_);
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                      [id](const subscription& entry) {
                                          return entry.id == id;
                                      }),
                       subscribers_.end());
}

std::vector<traffic_record> hub::traffic(std::size_t limit) const
{
    VSS_PROFILE_ZONE("hub.traffic_snapshot");
    return traffic_.snapshot(limit);
}

void hub::clear_traffic()
{
    traffic_.clear();
}

} // namespace vss
