# Architecture

This note explains how the pieces fit and, more importantly, why the pieces
that look odd are the way they are. The threading rules and the shutdown order
are not incidental; both were written after a real failure.

## Layers

```
apps/vss_cli   apps/vss_gui
        \         /
         vss_studio        scenarios, persistence, advertised paths
        /     |      \
 vss_config  vss_ports  (both depend on) vss_core
```

`vss_core` knows nothing about serial hardware. It defines:

* `vss::transport`, the smallest useful abstraction: `async_read_some`,
  `async_write_some`, `close`, and optional control lines.
* `vss::endpoint`, one transport plus a strand, a write queue, counters and a
  control-line model.
* `vss::hub`, which owns the `io_context`, the endpoint registry, the routing
  table and the event dispatcher.

`vss_ports` implements transports and the operating-system side of a port:

* `serial_transport`, which carries a real serial port, an RFC 2217 client, a
  raw TCP or UDP connection and the scripted mock device through
  `boost::virtual_serial::any_serial_port`, chosen by the URI scheme;
* `listen_transport`, an endpoint that waits for others (`tcp-listen://`,
  `udp-listen://` and `rfc2217-listen://`), with the RFC 2217 protocol in
  `rfc2217.hpp`;
* `loopback_transport`, an in-memory device that echoes what it is sent;
* `create_virtual_port`, which makes a pseudo terminal on Linux and macOS and a
  driver port on Windows (`vcom_driver.hpp`), and `stable_link`, the symbolic
  links that give a pseudo terminal a fixed name;
* the native control-line calls, serial device enumeration, com0com detection,
  the permission and firewall checks, and the probe of the Windows driver setup.

`vss_config` is pure data: it reads, checks and writes the JSON topology file.
`vss_studio` composes all three into the operations a user actually asked for,
and holds the pieces that act on a running hub: capture and replay, scripted
devices and masters (`responder`), reconnecting a lost client, the line-format
check and the benchmark. `vss_testkit` is a small library for tests and depends
on `vss_core` and `vss_ports`.

The GUI reaches the studio through a backend interface in
`apps/vss_gui/src/backend`, which has a fake for tests.

The dependency direction matters: `vss_core` could be reused for a completely
different kind of byte transport without dragging serial code along.

## Threading

Each endpoint has its own `asio::strand`. Its transport is constructed on the
hub's executor, and two rules keep the transport single-threaded:

1. The endpoint binds every completion handler it passes down to its strand.
2. Transports complete handlers on the handler's *associated* executor, not on
   their own. `loopback_transport` uses
   `get_associated_executor(handler, fallback)` for exactly this reason, and
   `serial_transport` passes the handler through untouched so the serial
   library can honour it.

Without rule 2, a transport would complete on an arbitrary hub thread while
`close()` ran on the strand, which is a data race inside the serial library.
That failure showed up as an intermittent access violation in a repeated
high-speed test, and the fix is the reason the rule is written down.

The hub's own event dispatcher runs on a single dedicated strand. Sinks
therefore never run inline with the operation that published the event, and
never on a transport thread.

### Subscriber lifetime

`unsubscribe` stops future deliveries but cannot recall one that is already
running. A sink must capture state that outlives the hub: a `shared_ptr`, or a
variable declared before the hub in the same scope. Capturing a stack local of
the scope that created the subscription is a use-after-free waiting to happen,
and the system tests demonstrate the correct shape.

### Shutdown

`hub::stop()` does not close transports from the calling thread. It asks every
endpoint to shut down on its own strand and counts the completions; the last
one stops the `io_context`. Two properties follow:

* no `close()` can race a completion handler, and
* a transport that never completes its final operation cannot hang shutdown.

`hub::join()` only joins the worker threads. Calling `stop()` and `join()` twice
is safe, and a stopped hub can be started again (`io_context::restart`).

## Routing

The router looks at the routes originating from a source endpoint and copies
each chunk to every target's write queue. Routes are derived from links:

| Link | Routes |
|---|---|
| `connect(a, b)` | a→b and b→a |
| `split(a, [b, c])` | a→b, a→c, b→a, c→a |
| `join([a, b], c)` | a→c and b→c |
| `redirect(a, b)` | a→b |
| `monitor(a, b)` | a→b with modem emulation disabled |
| `bus([a, b, c])` | every ordered pair: a→b, a→c, b→a, b→c, c→a, c→b |

Adding a link is refused when it would close a cycle in the graph, because a
cycle in the routing table is an infinite loop. Cycles through *hardware* (the
two ends of a cable) cannot be detected and are documented as a user error.

### The owner policy

`interleave` links forward everything. A `merge_policy::owner` link keeps an
atomic owner and arbitrates the routes that *meet at one endpoint*, and no
others. Those are the routes where bytes from several publishers would mix:

| Link | Arbitrated routes | Never arbitrated | The owner is |
|---|---|---|---|
| `join([a, b], c)` | a→c, b→c | (none) | one of the sources |
| `redirect(a, b)` | a→b | (none) | the source, so nothing is ever dropped |
| `split(a, [b, c])` | b→a, c→a | a→b, a→c | one of the targets |
| `connect(a, b)` | b→a | a→b | the target, so nothing is ever dropped |
| `bus(...)` | refused: a bus takes `interleave` only | | |

On an arbitrated route a publisher that does not own the link has its bytes
counted in `link_stats.dropped` and not forwarded. On the other routes of the
link the policy does nothing: the single source of a split reaches every target
whoever owns the link, its bytes are never counted as dropped, and it never
becomes the owner. So a split with the owner policy is "several programs take
turns sending to one device, and all of them hear its replies".

While a link has no owner (it was just made, or `release_owner` was called) the
first publisher on an arbitrated route takes it with a compare-exchange.
`set_owner` accepts only an endpoint that publishes on an arbitrated route (the
fourth column) and fails with `invalid_argument` for any other, and with
`unsupported` for a bus. Whether a route is arbitrated is decided when the
route is made and stored in it, so the router does not work it out per chunk.

Before this rule the policy was checked on every route of a link. On a split
that dropped every reply of the targets once the source owned the link, and let
a target that sent first take the link and shut the source out of it.

### Changing a link in place

Three pairs of calls change a link that exists without making it again. The link
keeps its id, options, owner and counters, and its other routes are not touched:

* `hub::add_target(link, endpoint)` and `hub::remove_target(link, endpoint)` add or
  remove one target of a duplex link (`connect` or `split`).
* `hub::add_source(link, endpoint)` and `hub::remove_source(link, endpoint)` add or
  remove one source of a one-way link (`join`, `redirect` or `monitor`).
* `hub::add_member(link, endpoint)` and `hub::remove_member(link, endpoint)` add or
  remove one member of a bus.

The rules, for each kind of link:

| Link | `add_target` / `remove_target` | `add_source` / `remove_source` | `add_member` / `remove_member` |
|---|---|---|---|
| `connect`, `split` | adds source→target and target→source, or removes both | `unsupported` | `unsupported` |
| `join`, `redirect`, `monitor` | `unsupported` | adds source→target, or removes it | `unsupported` |
| `bus` | `unsupported` | `unsupported` | adds the `2 * n` routes between the member and the `n` others, or removes them |

A call is refused on a link of the wrong kind because each kind varies in one
list only. A duplex link has one source by construction (`connect` and `split`
make no other), so what varies is its targets. The saved topology has no shape
for one source feeding several targets one way (`redirect` and `monitor` are one
to one, `join` is many to one), so what varies on a one-way link is its sources.
A bus has no sources or targets: a member is both ends of `n - 1` routes, it is
recorded as the one bus the endpoint is on, and the cycle check treats the whole
bus as one node, so it has its own pair of calls.

A `monitor` takes a source. The saved topology writes a one-way link with
several sources as a `join` whether or not it carries modem lines (a tap on a
bus is such a join), so a monitor with two sources is a valid `join` with
`emulate_modem_lines` false, and with one source again it is saved as a
`monitor` (or as a `redirect` if it carries lines). A `redirect` is a `join` from
the second source on and a `redirect` again at the first. The file format and the
JSON schema are unchanged.

The refusals, and nothing is changed by any of them:

* `not_found` for an unknown link or endpoint, and for removing an endpoint that
  is not a target, source or member of the link (the target of a one-way link is
  not one of its sources).
* `unsupported` for a link of the wrong kind, as the table shows.
* `invalid_argument` for adding the link's own source as a target (a self-link)
  or its target as a source, for removing the last target or the last source (a
  link has at least one of each, so `disconnect` it), for adding a member to a
  bus of 64 (`max_bus_members`), and for removing a member of a bus of two (a bus
  has at least two, so `disconnect` it; removing an *endpoint* from the hub is
  different, and takes the bus down under two members).
* `already_exists` for an endpoint that is a target or source of the link
  already, and for a member of this bus or of another (an endpoint is on one bus
  at most).
* `cycle_detected` when a new route would close a loop through other links or
  through a bus. For a bus member the rule is that the endpoint becomes part of
  the bus node, which closes a loop exactly when a route leaves the endpoint and
  reaches the bus, or leaves the bus and reaches the endpoint, directly or
  through other links or buses. Either would deliver the same bytes twice or
  send them round. The check is `reaches(endpoint, {bus})` or
  `reaches(bus, {endpoint})` on the table as it is before the change; the graph
  has no cycle now, so no other cycle can pass through the merged node. Two
  routes that reach the same listen-only tap, one from the endpoint and one from
  the members, are not a path between them and are not a loop.

Every check is made before the first change, and the space for every route is
reserved, and every delay line made, before any route is stored, so a refused or
failed call leaves no half of a link behind.

What a change does to traffic and state:

* **Bytes on the way.** `route_data` collects its deliveries under the shared
  lock, as shared pointers to the target endpoints and delay lines, and writes
  after releasing it. The six calls change the table under the exclusive lock.
  So a chunk that is being routed when an end is removed either reaches it or
  does not, and the endpoint it is written to is alive because the delivery
  holds it. A chunk routed when an end is added sees either all of the new routes
  or none: for a member, the `2 * n` routes are stored, `sources` is extended and
  `bus_of_` is updated under one exclusive lock, so no reader sees a member that
  can hear and not speak, or the reverse. A surviving route's entry is never
  rewritten (its delay line is the same object), and one publisher's chunks are
  routed one at a time on its strand, so a surviving end sees them in order and
  once.
* **Delayed bytes.** The hub closes the delay lines of the removed routes before
  the call returns, after releasing the lock. Closing discards what the line
  holds, makes it refuse a chunk still on its way in, and waits for a write to
  the target that is under way. The line's timer and strand go when the last
  reference does; its handlers hold it weakly, so nothing runs on a freed line.
  An end that is added again gets new, empty lines, and cannot be overtaken by
  the old ones. A new route has the link's delay and jitter.
* **Counters.** They belong to the link, not to a route. `bytes` and `messages`
  count deliveries handed to a route when they were routed and `dropped` counts
  bytes the owner policy refused, and all three keep what a removed route added.
  Bytes discarded from a delay line were counted in `bytes` when they went in,
  as they are when a delayed link is removed.
* **The owner.** Removing the target (of a duplex link) or the source (of a
  one-way link) that owns the link releases it, so the next one to send takes it.
  A new end owns nothing until it is given the link or sends first on a link with
  no owner. Whether a route is arbitrated is decided when the route is made, from
  its sender being one of the link's owner candidates (the targets of a duplex
  link, the sources of a one-way link), so a new end is put in the list *before*
  its routes are made. A new source's route is arbitrated: its bytes are dropped
  and counted until it owns the link. A new target's reply route is, and its
  forward route is not. A bus has no owner.
* **Control lines.** On a link with `emulate_modem_lines` a new target is given
  the source's output lines at once, through the link's wiring. The source's
  inputs follow the new target from that target's next line change, as on a link
  that was just made. A new *source* changes no input of the target: the target
  follows it from its next line change, as it follows any source of a join. A
  removed target loses the inputs the link drove, as when a cable is pulled; if
  another link still drives that endpoint its inputs show that link's sender
  instead. The source's inputs are then set from the targets that remain, in list
  order, so they never show a port that is no longer on the link. After a removed
  *source*, the target's inputs are set from the sources that remain, in list
  order (with several, the last of them shows), so they never show a port that is
  gone. A bus carries no lines.
* **The saved topology.** `link_info`, `studio::snapshot()` and the JSON follow
  the lists. A duplex link with one target is saved as `duplex` and with several
  as `split`; a one-way link with several sources is saved as `join`, and with one
  as `redirect` (`monitor` without modem lines); a bus lists its members in order.
  The file format is unchanged, and a snapshot after any sequence of the calls is
  valid and reads back as the same links.

### Changing the options of a link

`hub::set_link_options(link, options)` changes the delay, jitter, merge policy,
`emulate_modem_lines`, wiring and half-duplex line of a link that exists, in place. The link keeps
its id, its sources, its targets and its counters, and the other links are not
touched. Before it, the GUI made the link again with a new id for any of these.

| Option | What a change does |
|---|---|
| `delay_ms`, `jitter_ms` | Every route gets a new delay line (none when both are now zero) and the old lines are closed, so the bytes they still hold are **discarded**, as when a target is removed. If neither changed, the lines are the same objects and keep their bytes. |
| `policy` | Each route's `arbitrated` flag is worked out again. Into `owner` the link has no owner until a publisher on an arbitrated route sends first or `set_owner` names one. Out of `owner` the stored owner is cleared. `owner` to `owner` leaves the owner as it is. |
| `emulate_modem_lines` on, or `wiring` while on | The current outputs of every sender are pushed along the link through the new wiring, as `set_link_wiring` does, so the receivers' inputs follow at once. |
| `emulate_modem_lines` off | Each endpoint the link drove loses the inputs it drove, as when a cable is pulled, unless another link still drives it, and then it shows that link's sender, as after `remove_target`. |
| `half_duplex` (a bus only) | When any of its values changes while it is on, or it is turned on or off, the bus gets a new `bus_line` and a new `bus_wire` (none when it is now off) and the old wire is closed, so what it still carries is **discarded**. The new line starts free, each member's RTS is read again, and `link_stats::collisions` and `garbled` start again from zero, because they belong to the line. If none of its values changed, the line and the wire are the same objects. |

The rules for each kind of link:

| Link | Rule |
|---|---|
| `connect`, `split`, `join`, `redirect` | Every option may change. |
| `monitor` | Every option may change. A `monitor` is a one-way link with `emulate_modem_lines` false, and the saved topology tells it from a `redirect` by that flag, so turning the lines on saves it as a `redirect` and off again as a `monitor`. The file takes a delay and a policy on every link, so no combination is refused for being unsaveable, and the options read back from the saved file are the ones set. The wiring is the one option the file does not store. |
| `bus` | Delay, jitter and the half-duplex line may change. `emulate_modem_lines` is stored as false whatever is passed, as on creation. Any policy but `interleave` is `unsupported`. The wiring is ignored and the bus keeps the wiring it has: it has no lines, so it means nothing, and only `set_link_wiring`, which is asked for nothing else, refuses it with `unsupported`. |

The refusals, and nothing is changed by any of them: `invalid_argument` for a
delay over one minute, a jitter over the delay or half-duplex values outside
their limits (checked first, as on creation), `not_found` for an unknown link,
`unsupported` for a bus and the owner policy and for `half_duplex.enabled` on a
link that is not a bus. The new delay lines, and the new line and wire of a
half-duplex bus, are made before the first stored change, so a failed call
leaves the link as it was. While the old wire is closed the link's `retiming`
count is up, as it is while old delay lines are closed, so no chunk takes the
new wire and passes one the old wire is still delivering.

What a change does to traffic:

* **Which fields are read.** `route_data` reads, under the shared lock, the
  route's `link`, `to`, `delay` and `arbitrated`, and the link's `owner`,
  counters and `retiming` count. `route_lines` reads the route's `link` and `to`
  and the link's `emulate_modem_lines` and `wiring`. `set_link_options` writes
  every one of them under the exclusive lock, so no reader sees a link half
  changed: it sees the options, the lines and the flags from before, or all of
  them from after.
* **Bytes on the way.** A chunk routed before the call, on a delayed link, is
  either delivered or discarded (the old line is closed after the lock is
  released, and a chunk that reaches a closed line is dropped). On an undelayed
  link it is delivered. A chunk routed after the call returns is delivered, in
  order, through the new lines. Because one publisher's chunks are routed one at a
  time on its strand, and the old lines are closed before the call returns, bytes
  can be lost at a change but are never reordered or repeated.
* **The gate.** Closing a line waits for a write under way, which happens after
  the lock is released. A chunk routed in between would take the new route and could
  pass a chunk that an old line is still writing (a delayed link made undelayed).
  So when lines are replaced the link's `retiming` count is raised under the
  exclusive lock and lowered once the old lines are closed. `route_data` skips a
  link while the count is above zero, so a chunk routed in that short time is
  discarded and counts nothing. The count rather than a flag lets two overlapping
  calls on one link keep the gate shut until both have closed their lines.
* **Counters and owner.** The counters belong to the link and keep counting. A
  discarded chunk was counted in `bytes` when it went into its line. The owner is
  kept unless the policy leaves `owner`.
* **A line change being routed** when the lines are switched off may still reach
  its receiver just after the call, as with `remove_target` and
  `set_link_wiring`; the next change corrects it.

### The hub's clock, the read time and the read stage

The hub has one clock, `hub::now()`: the steady clock unless
`hub_options::clock` replaces it. Every time the hub reports and every time it
acts on is read there: an event's `when`, a link's delay, a half-duplex bus.
(An endpoint's `stats().last_activity` is the steady clock's, as before.)

An endpoint reads the clock once for each chunk it reads, before the chunk is
routed. That **read time** is the chunk's time for the half-duplex line, for a
`bus_fault` event and for the data event's `when`, so the three agree and none
includes the time routing took.

`endpoint::set_read_stage` puts a `read_stage` on an endpoint. It is called on
the endpoint's strand with each chunk and its read time, before the chunk is
routed and before its event is published. It is the place where response rules
will watch a port. The stage is a plain member
used only on the strand, so an endpoint with no stage pays one test of a null
pointer for each read. A stage is replaced on the strand too, which is between
two chunks: the old stage has seen every chunk before the change and the new one
sees every chunk after, with none lost and none shown twice. A stage must not
block, because it holds up every byte of the port for as long as it takes.

A delay line reads the hub's clock as well. On the steady clock it waits for the
exact time with a timer. On a clock a test moves by hand a timer cannot wait, so
a line that holds something looks at the clock every millisecond, as the wire of
a half-duplex bus does.

### Link delay

`connect`, `split`, `join`, `redirect` and `bus` take `link_options` with
`delay_ms` and `jitter_ms`, to rehearse a slow wire or a network hop. Each route
of such a link has its own `delay_line`: a first-in, first-out queue with one
timer on a strand of its own. A chunk is written to the target when its delay
has passed, drawn uniformly within the jitter either side of `delay_ms`. Due
times never go backwards, so jitter cannot reorder the bytes of a route. The
delay is at most one minute (`max_link_delay_ms`) and the jitter at most the
delay, or the call fails with `invalid_argument`. `monitor` takes no options and
has no delay. Removing a route drops whatever its line still holds, and so does
changing the delay of the link (see *Changing the options of a link*).

### The shared bus

A bus is the one link that is a broadcast domain rather than a set of
directions. `hub::bus(members)` puts two to 64 endpoints on one multi-drop
wire, as an RS-485 pair does: a chunk read from any member is copied to every
other member and never back to the member it came from. The routes are the
`n * (n - 1)` ordered pairs, so routing needs nothing new, and the limit of 64
keeps a saved file from asking for millions of them. The link is stored with
`link_mode::bus`, its members in `sources` and no `targets`. Because the
routes run both ways between every pair, the plain cycle check would refuse
them; the rules below are what keep it sound.

* **A bus is one node of the cycle graph.** The check treats all members as a
  single node. The routes between members are not loops, but a path that leaves
  the bus and comes back to it is: with a bus of `a`, `b` and `c`,
  `redirect(a, x)` is allowed and `redirect(x, b)` is then refused, however long
  the way round. Creating a bus is refused (`cycle_detected`) when an existing
  route already runs from one member to another, directly or through other
  ports, since the bytes would arrive twice. A route between two members of the
  same bus is refused for the same reason.
* **An endpoint is on at most one bus** (`already_exists`). Two buses that share
  a member would be one bus, so the hub does not pretend otherwise. A member may
  still be in a split, a join or a redirect; those links see it as the one
  endpoint it is, and the cycle rule above covers the combination.
* **A listen-only tap is not a member.** Nothing was added for it. A `join` from
  every member to the tap (or a `monitor` from one member) copies each
  member's bytes to it, and the tap sends nothing back because a join is one
  way and the tap is not on the bus. A member that "must not transmit" is a
  program that does not write, which the hub cannot enforce and a tap does not
  need.
* **No modem lines.** A multi-drop wire pair carries none, and with several
  members "the peer's inputs" has no meaning, so `emulate_modem_lines` is stored
  as `false` whatever the caller passed, and the wiring cannot be edited
  (`unsupported`). A program that toggles RTS to switch an RS-485 transceiver
  still works; the line goes nowhere.
* **Only `interleave`.** `merge_policy::owner` names one publisher per link, which
  on a bus would silence the replies a polled protocol needs, so it is refused
  (`unsupported`) and so is `set_owner`. Bytes from different members are mixed
  in the order the hub sees them, and nothing arbitrates a collision unless the
  bus is a half-duplex line (below); chunks from one member keep their order.
* **Counting.** The link counts each delivery, as a split does, so a chunk of
  `n` bytes from one member of a bus of five adds `4n` bytes and four messages
  to the link. The traffic ring shows it once as received on the sender and
  once as sent on each receiver.
* **Removal and membership.** Removing the link frees its members. Removing an
  endpoint from the hub takes it out of the bus and leaves the others on it; the
  bus goes when fewer than two remain. `add_member` and `remove_member` change
  the membership of a bus that stays (see *Changing a link in place*); the
  second refuses to go below two members, because that is what `disconnect` is
  for.

Any endpoint kind may be a member: a driver port, a pseudo terminal, a loopback
port, a real serial port, and TCP, UDP and RFC 2217 ports, so a port on another
computer can sit on the same bus.

### The half-duplex line

A bus made as above gives the hearing of a two-wire RS-485 pair and none of its
discipline. `link_options::half_duplex` (off by default, `unsupported` on any
link but a bus) adds the discipline: one member sends at a time, and the bus says
so when two do not. A bus without the option takes none of the paths below, and
its routing, its counters, its events and its saved file are what they were.

**What "the line is busy" means.** The bus has a line rate, `bit_rate`, in bits
per second (50 to 4,000,000; a byte is ten bits, as 8N1). A chunk of `n` bytes
that the hub reads from a member at time `t` occupies the wire from `t` until
`t + 10n / bit_rate`. A second chunk from the same member waits for the first, as
bytes leave a UART one after another. The other members hear a chunk when its
last byte has left the wire, and not when the hub read it.

That last rule is not a choice. A port made by Studio hands over a whole frame in
an instant, and a device answers within microseconds of hearing it. If the poll
were delivered at once while the model still held the wire for its eight
milliseconds, every fast answer would be reported as a collision that no real
line would have had. Holding the bytes for their time on the wire is what keeps
the report true.

The alternatives were weighed and left:

* *A fixed hold for each chunk* has no meaning a user can check against their
  equipment, and a chunk is not a frame, so the hold would change with how a
  program happens to split its writes.
* *Busy while a member's RTS is high* works only for members with an RTS the
  hub can follow, and most have none (see below).
* *No duration, overlap by arrival order* cannot tell an answer that came after
  the poll from one that came during it.

**What overlap does.** A chunk whose time on the wire meets another member's has
collided with it (`bus_fault::overlap`). It is always counted
(`link_stats::collisions`) and always published. What the receivers get is
`on_collision`: `report` delivers the bytes as they were sent, and `garble`
delivers every byte of each chunk that collided inverted (`~byte`), counted in
`link_stats::garbled` once for each receiving member, as `bytes` is. Inverting is
deterministic, changes every byte, and a capture shows what the bytes were. Both
chunks are spoilt, the one that was on the wire and the one that ran into it:
because a chunk is held until its end, the first is still in hand when the second
arrives. The whole chunk is spoilt, not the part that overlapped; Studio does not
know where a frame ends. Nothing is dropped by a collision.

**Turn-around.** `turnaround_ms` is how long the line stays taken after a
member's last byte, while its transmitter lets go. A chunk from *another* member
that starts inside it has come too soon (`bus_fault::too_soon`): it is counted
and published like an overlap, and with `garble` it alone is delivered inverted,
because the bytes before it had already left the wire whole. The turn-around
delays nothing. `delay_ms` and `jitter_ms` are still the only delay: they are
applied to each route after the chunk has left the wire, so a member hears a
chunk `10n / bit_rate + delay_ms` after it was sent. One thing holds the bytes for
the time of a slow wire (the link delay), one for the time of the bits (the line
rate), and the turn-around only judges.

**RTS.** With `rts_gating`, a member sends only while its RTS is high, and holds
the line for as long as it is: bytes it sends with RTS low never reach the wire
(`bus_fault::transmitter_off`, counted in `dropped` and published), and a chunk
from another member while it holds RTS high has collided with it
(`bus_fault::line_held`). This is only true to a real line when the hub knows the
RTS at the moment it reads the bytes, so it applies to a member only when its
transport says so (`transport::reports_peer_rts`). A member whose transport does
not is taken to switch its transmitter by itself, as an adapter with automatic
direction control does: it always sends and never holds the line. That is every
port a program opens today:

| Member | The hub's RTS for it | Gated |
|---|---|---|
| Loopback port | Set through Studio (`endpoint::set_line`), in step with the bytes | Yes |
| Windows driver port | The program's, but read only when asked; a change is not seen in step with the bytes | No |
| Pseudo terminal | None: a pty has no modem lines, the model holds RTS high | No |
| Real serial port | Studio's own output to the device, not the device's transmitter | No |
| `rfc2217-listen://` | The client's, but a change that arrives with bytes is seen after them, and several together as one | No |
| TCP, UDP, RFC 2217 client, mock | None | No |

A program that raises RTS, writes and drops RTS does all three within
microseconds on a virtual port. A hub that samples RTS beside the bytes would see
it low for most well-behaved programs and mute them, which is the opposite of the
truth. Gating a driver port needs the driver to hand the hub the RTS in line with
the bytes, and until it does the member is not gated. For the same reason RTS
dropped before the last byte has left the wire, which cuts a frame short on real
hardware, is not modelled.

**Too far ahead.** A program can write faster than the line for ever. A chunk
that could not start within a minute of being read (`max_bus_backlog_ms`) is
dropped, counted in `dropped` and published (`bus_fault::overrun`).

**Where the state is.** Each half-duplex bus has a `bus_line` (the model: who
holds the wire until when, and each member's RTS) and a `bus_wire` (the chunks on
the wire, one timer on a strand of its own). `route_data` asks the line for a
verdict once for each chunk, under the hub's shared lock and the line's own
mutex, which guards a few comparisons and is held for nothing else: no write, no
event and no other lock is taken under it. The hub mutex is released before the
chunk is pushed to the wire, and the wire writes to the members from its own
strand, outside its mutex. A link that is not a half-duplex bus pays one test of
a null pointer for each route. The line is per bus, so two buses, or a bus and
any other link, never wait for each other. One wire for the whole bus, and not a
queue for each route, is what makes every member hear the chunks in the order
they left the wire.

**Time.** `bus_line` has no clock: every call is given the time, so the model is
tested with times written in the test. The hub reads the time from
`hub_options::clock` (the steady clock when empty), so a test of the hub moves
the time by hand. A timer cannot wait for such a clock, so a wire on one looks at
it every millisecond while it holds something; a wire on the steady clock waits
for the exact time. What still depends on the wall clock is when a chunk is
delivered: never before its time on the wire is over, and as late after it as the
machine is busy.

**Reporting.** A chunk that was not a clean transmission publishes one
`event_kind::bus_fault` for its sender, with `fault`, the `link` and a sentence
in `message`, through the dispatcher strand like every other event. It is posted
after the hub lock is released. `link_stats` gains `collisions` and `garbled`.

**Removal.** A member that is removed stops holding the line at once, what it
still had on the wire is discarded with its routes (as a link's delay line is),
and what the others had on the wire is no longer delivered to it. `remove_member`
does the same for a member that stays in the hub.

**Joining.** `add_member` puts an endpoint on a half-duplex bus under the
exclusive lock: the line is told its RTS and its routes are added in one step. It
hears the chunks routed after that, so a chunk already on the wire when it joined
does not reach it, and it never hears part of one.

**The saved file.** A half-duplex bus that came back as a plain bus after save
and open would silently change what a test rig does, so the option is saved: one
optional object, `half_duplex`, on a bus link, left out when the option is off.
The line wiring stays out of the file as before.

## Control lines

`control_lines` is a bit set split into outputs (DTR, RTS, BREAK) and inputs
(CTS, DSR, DCD, RI). When an endpoint's outputs change, the hub computes the
peer's inputs from the link's `modem_wiring` and pushes them to every linked
endpoint. The default wiring is a straight-through null-modem cable: DTR drives
DSR and DCD, and RTS drives CTS. A link can be rewired, and a link made with
`emulate_modem_lines` off (a `monitor`, a bus) carries no lines at all.

```cpp
control_lines peer = link.options.wiring.apply(local);
target->set_peer_inputs(peer);   // replaces the whole input set
```

The inputs are replaced rather than merged so the result does not depend on
which line moved last. Transports that can drive the device apply the change;
the rest keep the model and report it through events.

## Why loopback devices echo

A loopback transport is a loopback *plug*: what you write comes back on the
receive side. That makes it a useful demo device and a good test double, and it
means two loopback devices wired duplex are an echo chamber. This is a property
of the device, not a routing bug. Use a one-way link (`redirect` or `monitor`)
when the point is observation rather than full duplex.

## Events and capture

`endpoint::on_read` routes data on the I/O thread and then publishes an event.
Routing does not wait for the dispatcher, so the fast path stays on one thread
and copies once. Events carry a `shared_ptr<const byte_buffer>`, so fan-out to
many monitors is cheap, and the hub keeps a bounded ring buffer for late
subscribers. Endpoints can opt out of data events entirely for throughput.

## Extending it

* A new transport: implement `vss::transport`, observe the associated-executor
  and `close()` contracts, and add it to `vss_ports`.
* A new backend: the serial library already selects by URI; `serial_transport`
  classifies the scheme for labelling.
* A new scenario: compose `studio` primitives, then save the result through
  `studio::snapshot()` so it can be replayed and tested.
