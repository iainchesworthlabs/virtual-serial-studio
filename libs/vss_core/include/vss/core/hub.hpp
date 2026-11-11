//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_HUB_HPP
#define VSS_CORE_HUB_HPP

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/error.hpp>
#include <vss/core/event.hpp>
#include <vss/core/link.hpp>
#include <vss/core/profile.hpp>
#include <vss/core/traffic_log.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vss {

class delay_line;

struct hub_options
{
    /// Worker threads; 0 selects `std::thread::hardware_concurrency()`.
    unsigned io_threads = 0;
    /// Events kept in the traffic ring buffer.
    std::size_t traffic_capacity = 8192;
};

/// The topology engine.
///
/// A hub owns an `io_context`, the endpoints attached to it, the routing
/// table between them and the event dispatcher used by the GUI.  It contains
/// no serial knowledge: endpoints are given a #transport, so the same hub
/// drives real ports, virtual ports, network bridges and in-memory fixtures.
class hub : public std::enable_shared_from_this<hub>
{
public:
    explicit hub(hub_options options = {});
    ~hub();

    hub(const hub&) = delete;
    hub& operator=(const hub&) = delete;

    boost::asio::io_context& io() noexcept { return io_; }
    boost::asio::any_io_executor executor() noexcept
    {
        return io_.get_executor();
    }

    void start();
    void stop();
    void join();
    bool running() const noexcept { return running_.load(); }

    // -- endpoints ---------------------------------------------------------

    result<std::shared_ptr<endpoint>> add_endpoint(
        const std::string& name, endpoint_kind kind,
        std::shared_ptr<transport> transport, const endpoint_options& options = {});

    result<void> remove_endpoint(endpoint_id id);
    result<void> rename_endpoint(endpoint_id id, const std::string& name);

    std::shared_ptr<endpoint> find_endpoint(endpoint_id id) const;
    std::shared_ptr<endpoint> find_endpoint(std::string_view name) const;
    std::vector<std::shared_ptr<endpoint>> endpoints() const;

    // -- topology ----------------------------------------------------------
    //
    // Every call that takes `link_options` refuses a delay over
    // #max_link_delay_ms, or a jitter over the delay, with
    // `errc::invalid_argument`.

    /// Bidirectional link between two endpoints.
    result<link_id> connect(endpoint_id a, endpoint_id b,
                            link_options options = {});

    /// One endpoint feed many listeners; replies are merged back.
    result<link_id> split(endpoint_id source,
                          const std::vector<endpoint_id>& targets,
                          link_options options = {});

    /// Many publishers merged into one endpoint.
    result<link_id> join(const std::vector<endpoint_id>& sources,
                         endpoint_id target, link_options options = {});

    /// One way traffic from `from` to `to`.
    result<link_id> redirect(endpoint_id from, endpoint_id to,
                             link_options options = {});

    /// Passive one-way copy, for a monitor port.
    result<link_id> monitor(endpoint_id from, endpoint_id to);

    /// A shared multi-drop bus: every byte a member publishes goes to every
    /// other member and never back to its sender, as on an RS-485 pair.
    ///
    /// The members form one broadcast domain, so the cycle check treats the
    /// whole bus as a single node: the routes between its members are not
    /// loops, but a path that leaves the bus and comes back to it still is.
    /// An endpoint is a member of at most one bus (`errc::already_exists`).
    /// There must be 2 to #max_bus_members distinct members.  A route that
    /// already joins two of them is refused with `errc::cycle_detected`.
    ///
    /// A listen-only tap is not a member: give it a #join from the members
    /// (or a #monitor from one), which the bus does not feed back.
    ///
    /// A bus carries no modem lines, so `emulate_modem_lines` is stored as
    /// `false`, and it supports `merge_policy::interleave` only
    /// (`errc::unsupported` otherwise).  `delay_ms` and `jitter_ms` apply to
    /// every route between members.  The link counts a delivery to each
    /// receiving member, as a split does.  Removing a member takes it out of
    /// the bus; the bus itself goes when fewer than two members remain.
    result<link_id> bus(const std::vector<endpoint_id>& members,
                        link_options options = {});

    result<void> disconnect(link_id id);

    /// Adds one more target to a duplex link that exists (a #connect or a
    /// #split), without touching its other routes: the link keeps its id,
    /// its options, its owner and its counters, and bytes on the way to the
    /// other targets are not disturbed.  A #connect with a second target is
    /// a split from then on.
    ///
    /// The call adds the routes source→target and target→source, both or
    /// neither.  It fails, and changes nothing, with
    /// - `errc::not_found` for an unknown link or endpoint,
    /// - `errc::unsupported` for a one-way link (#join, #redirect, #monitor:
    ///   the saved topology has no shape for one source feeding several
    ///   targets one way, and what varies in a join is its sources) and for
    ///   a #bus (membership is a different operation: every member is both
    ///   ends of `n - 1` routes and one node of the cycle graph),
    /// - `errc::invalid_argument` when the endpoint is the link's source,
    /// - `errc::already_exists` when it is a target of the link already,
    /// - `errc::cycle_detected` when either new route would close a loop,
    ///   through other links or through a bus.
    ///
    /// With `emulate_modem_lines` the new target sees the source's output
    /// lines at once.  The source's inputs follow the new target from that
    /// target's next line change, as they do on a link just made.
    result<void> add_target(link_id id, endpoint_id target);

    /// Takes one target out of a duplex link and leaves the others linked.
    /// The endpoint itself stays in the hub.
    ///
    /// A chunk the hub is routing when the call is made either reaches the
    /// target or does not; nothing reaches it from this link after the call
    /// returns except such a chunk.  Bytes the link's delay still holds for
    /// the two removed routes are discarded.  The link's counters keep what
    /// the removed routes carried.  If the target owned the link the owner
    /// is released.  With `emulate_modem_lines` the target's input lines
    /// drop, as when a cable is pulled, unless another link still drives
    /// them, and the source's inputs are set from the targets that remain.
    ///
    /// It fails, and changes nothing, with `errc::not_found` for an unknown
    /// link and for an endpoint that is not a target of it,
    /// `errc::unsupported` for a one-way link or a bus,
    /// `errc::invalid_argument` for the link's source, and
    /// `errc::invalid_argument` for the last target: a link has at least
    /// one, so remove the link with #disconnect instead.
    result<void> remove_target(link_id id, endpoint_id target);

    std::vector<link_info> links() const;
    std::optional<link_info> find_link(link_id id) const;

    /// Assigns the single publisher allowed by #merge_policy::owner.
    ///
    /// The policy arbitrates the routes of a link that meet at one
    /// endpoint, and only those.  On a one-way link (#join, #redirect) they
    /// are the routes from its sources, so the owner is one of the sources.
    /// On a duplex link (#connect, #split) they are the routes from its
    /// targets back to its source, so the owner is one of the targets; what
    /// the source sends fans out to every target whoever owns the link, is
    /// never dropped by the policy and never takes ownership.  While a link
    /// has no owner, the first publisher on an arbitrated route takes it.
    /// `errc::invalid_argument` for an endpoint that cannot own the link and
    /// `errc::unsupported` for a bus.
    result<void> set_owner(link_id id, endpoint_id owner);
    result<void> release_owner(link_id id);

    /// Changes which control line drives which across an existing link, and
    /// brings the peers' input lines in step with the lines now high.
    result<void> set_link_wiring(link_id id, const modem_wiring& wiring);

    // -- events ------------------------------------------------------------

    /// Registers a sink.
    ///
    /// Sinks run on the hub's event strand, never inline with the publish.
    /// `unsubscribe` stops future deliveries but cannot recall one that is
    /// already running, so a sink must capture state that outlives the hub
    /// (a `shared_ptr`, or locals declared before the hub is destroyed) --
    /// never a stack local of the scope that created it.
    subscriber_id subscribe(event_filter filter, event_sink sink);

    /// Accepts any callable invocable with an #endpoint_event, so callers do
    /// not have to spell `std::function` at the call site.  The `EventSink`
    /// concept is what turns a mistyped handler into a readable diagnostic.
    template <EventSink Sink>
    subscriber_id subscribe(event_filter filter, Sink&& sink)
    {
        return subscribe(filter, event_sink(std::forward<Sink>(sink)));
    }
    void unsubscribe(subscriber_id id);

    std::vector<traffic_record> traffic(std::size_t limit = 0) const;
    void clear_traffic();

    // -- used by endpoint --------------------------------------------------

    void route_data(endpoint_id from, bytes_view data);
    void route_lines(endpoint_id from, control_lines lines);
    void publish(endpoint_event event);

private:
    struct route_entry
    {
        link_id link = 0;
        endpoint_id to = 0;
        /// Holds the bytes for the link's delay; null for a link without one.
        std::shared_ptr<delay_line> delay;
        /// The link's owner policy decides whether this route forwards.
        bool arbitrated = false;
    };

    struct link_record
    {
        link_id id = 0;
        link_mode mode = link_mode::duplex;
        link_options options;
        std::vector<endpoint_id> sources;
        std::vector<endpoint_id> targets;
        std::atomic<endpoint_id> owner{0};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::uint64_t> messages{0};
        std::atomic<std::uint64_t> dropped{0};
    };

    struct subscription
    {
        subscriber_id id = 0;
        event_filter filter;
        event_sink sink;
    };

    result<link_id> add_link(link_mode mode,
                             const std::vector<endpoint_id>& sources,
                             const std::vector<endpoint_id>& targets,
                             link_options options);
    bool would_create_cycle(endpoint_id from, endpoint_id to) const;
    bool bus_would_create_cycle(const std::vector<endpoint_id>& members) const;
    /// The node of the cycle graph an endpoint stands for: itself, or the
    /// bus it belongs to.
    endpoint_id cycle_node(endpoint_id id) const;
    bool reaches(endpoint_id start,
                 const std::unordered_set<endpoint_id>& goals) const;
    route_entry make_route(const link_record& link, endpoint_id from,
                           endpoint_id to);
    void add_route(const link_record& link, endpoint_id from, endpoint_id to);
    void erase_routes(link_id link);
    link_info describe(const link_record& record) const;
    void dispatch(const endpoint_event& event);
    void endpoint_closed();

    hub_options options_;
    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        work_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    // Shutdowns in flight, from remove_endpoint() and stop(). The io_context is
    // stopped only once stop() has been called and this reaches zero.
    std::atomic<std::size_t> closing_{0};
    /// Events posted to the dispatcher strand and not yet delivered; feeds the
    /// `hub.dispatch_queue` plot and costs nothing without instrumentation.
    profiling::depth_counter pending_events_;
    std::atomic<bool> stopping_{false};

    mutable std::shared_mutex mutex_;
    endpoint_id next_endpoint_id_ = 1;
    link_id next_link_id_ = 1;
    std::unordered_map<endpoint_id, std::shared_ptr<endpoint>> endpoints_;
    std::unordered_map<std::string, endpoint_id> names_;
    std::unordered_map<link_id, std::shared_ptr<link_record>> links_;
    std::unordered_map<endpoint_id, std::vector<route_entry>> routes_;
    /// The bus each member belongs to.
    std::unordered_map<endpoint_id, link_id> bus_of_;

    mutable std::mutex subscriber_mutex_;
    subscriber_id next_subscriber_id_ = 1;
    std::vector<subscription> subscribers_;

    boost::asio::strand<boost::asio::any_io_executor> event_strand_;
    traffic_log traffic_;
};

} // namespace vss

#endif // VSS_CORE_HUB_HPP
