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
#include <functional>
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

class bus_line;
class bus_wire;
class delay_line;

struct hub_options
{
    /// Worker threads; 0 selects `std::thread::hardware_concurrency()`.
    unsigned io_threads = 0;
    /// Events kept in the traffic ring buffer.
    std::size_t traffic_capacity = 8192;
    /// Where the hub reads the time (#hub::now).  Empty (the default) is the
    /// steady clock.  A test gives its own and moves it by hand, so that
    /// what overlaps and what does not is written in the test and does not
    /// depend on how fast the machine ran it.  A timer cannot wait for such
    /// a clock, so a link that holds bytes for a delay looks at it every
    /// millisecond.  Called from any thread.
    std::function<time_point()> clock;
    /// Ask the system to time this process's timers to about a millisecond
    /// while the hub runs.  It matters on Windows, where a timer otherwise
    /// fires on a 15.6 ms tick and a link's `delay_ms` of 1 takes about 15;
    /// elsewhere timers are already that fine and nothing is asked.  The
    /// request is made by start() and given back by join().  It costs some
    /// power, so a host that only watches traffic may turn it off.
    bool fine_timers = true;
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

    /// The time by the hub's clock: the steady clock unless
    /// `hub_options::clock` replaces it.  Every time the hub reports (an
    /// event's `when`) and every time it acts on (a link's delay, a
    /// half-duplex bus) is read here.  Safe from any thread.
    time_point now() const;

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
    ///
    /// `options.half_duplex` makes the bus a half-duplex line: one member
    /// sends at a time, at a line rate, and a chunk that runs into another
    /// member's is counted and published (see #half_duplex_options).  Values
    /// outside its limits are `errc::invalid_argument`.
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

    /// Adds one more source to a one-way link that exists (a #join, a
    /// #redirect or a #monitor), without touching its other routes: the link
    /// keeps its id, its options, its owner and its counters, and bytes on the
    /// way from the other sources are not disturbed.  A #redirect with a
    /// second source is a join from then on, and so is a #monitor (the saved
    /// topology has one shape for a one-way link with several sources, a
    /// join, which carries the link's `emulate_modem_lines` as it is).
    ///
    /// The call adds one route from the new source to each target of the
    /// link, all or none.  It fails, and changes nothing, with
    /// - `errc::not_found` for an unknown link or endpoint,
    /// - `errc::unsupported` for a duplex link (a #connect or a #split has
    ///   exactly one source, and what varies in it is its targets) and for a
    ///   #bus (membership is a different operation: see #add_member),
    /// - `errc::invalid_argument` when the endpoint is a target of the link,
    /// - `errc::already_exists` when it is a source of the link already,
    /// - `errc::cycle_detected` when a new route would close a loop, through
    ///   other links or through a bus.
    ///
    /// Under `merge_policy::owner` the new source's route is arbitrated like
    /// the others': it is dropped and counted until the source owns the link,
    /// which it does only if the link has no owner and it sends first or it is
    /// given the link with #set_owner.  The owner does not change.
    ///
    /// With `emulate_modem_lines` the target's input lines are not changed:
    /// they follow the new source from its next line change, as they follow
    /// any source of a join.
    result<void> add_source(link_id id, endpoint_id source);

    /// Takes one source out of a one-way link and leaves the others linked.
    /// The endpoint itself stays in the hub.
    ///
    /// A chunk the hub is routing when the call is made either reaches the
    /// target or does not; nothing reaches it from the removed source through
    /// this link after the call returns except such a chunk.  Bytes the link's
    /// delay still holds for the removed route are discarded.  The link's
    /// counters keep what the route carried.  If the source owned the link the
    /// owner is released.  With `emulate_modem_lines` the target's input lines
    /// are set from the sources that remain, in list order (so with several
    /// the last of them shows), and never show a port that is no longer on the
    /// link.
    ///
    /// It fails, and changes nothing, with `errc::not_found` for an unknown
    /// link and for an endpoint that is not a source of it (a target of the
    /// link included), `errc::unsupported` for a duplex link or a bus, and
    /// `errc::invalid_argument` for the last source: a link has at least one,
    /// so remove the link with #disconnect instead.
    result<void> remove_source(link_id id, endpoint_id source);

    /// Adds one endpoint to a bus that exists, without touching the routes
    /// between its other members: the bus keeps its id, its options and its
    /// counters, and bytes on the way between the others are not disturbed.
    ///
    /// The call adds the `2 * n` routes between the new member and each of the
    /// `n` members the bus has, all or none, and puts the endpoint at the end
    /// of the member list.  It fails, and changes nothing, with
    /// - `errc::not_found` for an unknown link or endpoint,
    /// - `errc::unsupported` for a link that is not a bus (use #add_source or
    ///   #add_target),
    /// - `errc::already_exists` when the endpoint is a member of this bus or
    ///   of any other (an endpoint is on one bus at most),
    /// - `errc::invalid_argument` when the bus would have more than
    ///   #max_bus_members members,
    /// - `errc::cycle_detected` when the endpoint becoming part of the bus
    ///   would close a loop: a route runs from it to the bus, or from the bus
    ///   to it, directly or through other links or buses, and the bytes would
    ///   arrive twice or go round.
    ///
    /// A bus carries no modem lines and no owner, so nothing else changes.
    /// On a half-duplex bus the new member takes its turn on the line like
    /// the others, and the line is told at once whether its transmitter is
    /// on (its RTS, where its transport reports it).
    result<void> add_member(link_id id, endpoint_id member);

    /// Takes one member out of a bus and leaves the others on it, with the
    /// routes between them untouched.  The endpoint itself stays in the hub.
    ///
    /// A chunk the hub is routing when the call is made either reaches the
    /// removed member or does not; nothing reaches it, and nothing it sends
    /// reaches the bus, after the call returns except such a chunk.  Bytes the
    /// bus's delay still holds for the removed routes are discarded.  The
    /// bus's counters keep what the routes carried.  On a half-duplex bus
    /// what the member still has on the wire is discarded too and stops
    /// counting against the others, as when the endpoint is removed.
    ///
    /// It fails, and changes nothing, with `errc::not_found` for an unknown
    /// link and for an endpoint that is not a member of it,
    /// `errc::unsupported` for a link that is not a bus and
    /// `errc::invalid_argument` for a bus of two members: a bus has at least
    /// two, so remove it with #disconnect instead.  (Removing an endpoint from
    /// the hub is different: it takes the endpoint out of its bus and removes
    /// the bus under two members.)
    result<void> remove_member(link_id id, endpoint_id member);

    /// Changes the delay, jitter, merge policy, `emulate_modem_lines`,
    /// wiring and half-duplex line of a link that exists, in place: the link
    /// keeps its id, its sources, its targets and its counters, and the other
    /// links are not touched.  It works on every kind of link, a #monitor and
    /// a #bus included.
    ///
    /// It fails, and changes nothing, with
    /// - `errc::invalid_argument` for a delay over #max_link_delay_ms, a
    ///   jitter over the delay or half-duplex values outside their limits, as
    ///   on creation (checked first),
    /// - `errc::not_found` for an unknown link,
    /// - `errc::unsupported` for a #bus and any policy but `interleave`, as
    ///   #bus is, and for `half_duplex.enabled` on a link that is not a bus.
    ///
    /// A #bus stores `emulate_modem_lines = false` whatever is passed, as
    /// #bus does, and keeps the wiring it has: a bus has no modem lines, so
    /// the wiring of one has no meaning and is ignored here, whereas
    /// #set_link_wiring, which is asked for nothing else, refuses it.  Its
    /// delay and jitter change like any other link's.  A #monitor is a
    /// one-way link with `emulate_modem_lines` false; every option may change.
    /// Turning the lines on makes it a redirect in the saved topology
    /// (`studio::snapshot()` tells the two apart by that flag), and every
    /// combination of options is a valid saved link, so none is refused for
    /// that reason.  The wiring is not saved at all.
    ///
    /// - **Merge policy.**  Whether each route of the link is arbitrated is
    ///   worked out again for the new policy.  Into `owner`, the link is owned
    ///   by nobody until a publisher on an arbitrated route sends first or
    ///   #set_owner names one (an owner named while the link interleaved is
    ///   kept).  Out of `owner` the owner is cleared, so the link does not
    ///   name an owner it no longer has.
    /// - **Delay and jitter.**  When either changes, every route of the link
    ///   gets a new delay line (none when both are now zero) and the old ones
    ///   are closed before the call returns: the bytes they still hold are
    ///   DISCARDED, as when a target is removed.  When neither changes the
    ///   lines are the same objects and keep their bytes, so a change of
    ///   policy or wiring loses nothing.
    /// - **Half-duplex line** (a #bus only).  When any of its values changes
    ///   while it is on, or it is turned on or off, the bus gets a new line
    ///   and a new wire (none when it is now off).  What the old wire still
    ///   carries is DISCARDED, the new line starts free, and
    ///   `link_stats::collisions` and `garbled` start again from zero: they
    ///   belong to the line, not to the bus.  When none of its values changes
    ///   the line is the same object and nothing on it is lost.
    /// - **Order.**  A chunk routed before the call may be discarded (a
    ///   delayed link or a half-duplex bus) or delivered; a chunk routed while
    ///   the call runs may be discarded; chunks routed after it returns are
    ///   delivered in order, and never ahead of one that came before them.
    ///   Bytes can be lost at a change but are not reordered or repeated.
    /// - **Modem lines.**  Turned on, or rewired while on, the link pushes the
    ///   current outputs of every sender along it, as #set_link_wiring does,
    ///   so the receivers' inputs follow at once.  Turned off, each endpoint
    ///   the link drove loses the inputs it drove, as when a cable is pulled,
    ///   unless another link still drives it, and then it shows that link's
    ///   sender, as after #remove_target.
    ///
    /// The options are validated and every delay line is made before the
    /// first stored change, so a failed call changes nothing.
    result<void> set_link_options(link_id id, const link_options& options);

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

    /// Routes a chunk that `from` read at `when`, a time of the hub's clock.
    /// It is the chunk's time for a half-duplex bus and for a fault event.
    void route_data(endpoint_id from, bytes_view data, time_point when);
    /// The same, for a chunk read now.
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
        /// Above zero while #set_link_options closes the delay lines it has
        /// replaced.  A chunk routed in that time is discarded, so none can
        /// take the new route and pass one that is still in an old line.
        std::atomic<unsigned> retiming{0};
        /// The line and the wire of a half-duplex bus; null for every other
        /// link, which is how the router tells them apart.
        std::shared_ptr<bus_line> line;
        std::shared_ptr<bus_wire> wire;
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
    /// The delay line for a route to `to` under `options`; null when they
    /// have no delay.  The caller holds the lock.
    std::shared_ptr<delay_line> make_delay(endpoint_id to,
                                           const link_options& options);
    void add_route(const link_record& link, endpoint_id from, endpoint_id to);
    /// Removes the routes of `link` that leave `from` for `to` (for any
    /// endpoint when `to` is 0) and hands their delay lines to `held`, to be
    /// closed once the lock is released.  The caller holds the lock.
    void take_routes(endpoint_id from, link_id link, endpoint_id to,
                     std::vector<std::shared_ptr<delay_line>>& held);
    void erase_routes(link_id link);
    link_info describe(const link_record& record) const;
    void dispatch(const endpoint_event& event);
    void endpoint_closed();

    hub_options options_;
    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        work_;
    std::vector<std::thread> threads_;
    /// The request for fine timers (`hub_options::fine_timers`), held from
    /// start() to join().  Its type is the platform layer's.
    std::shared_ptr<void> timer_period_;
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
