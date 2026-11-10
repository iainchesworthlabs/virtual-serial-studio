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

`merge_policy::owner` links keep an atomic owner; a publisher that does not own
the link has its bytes counted as dropped instead of forwarded. `interleave`
links forward everything.

### Link delay

`connect`, `split`, `join`, `redirect` and `bus` take `link_options` with
`delay_ms` and `jitter_ms`, to rehearse a slow wire or a network hop. Each route
of such a link has its own `delay_line`: a first-in, first-out queue with one
timer on a strand of its own. A chunk is written to the target when its delay
has passed, drawn uniformly within the jitter either side of `delay_ms`. Due
times never go backwards, so jitter cannot reorder the bytes of a route. The
delay is at most one minute (`max_link_delay_ms`) and the jitter at most the
delay, or the call fails with `invalid_argument`. `monitor` takes no options and
has no delay. Removing a route drops whatever its line still holds.

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
  in the order the hub sees them, and nothing arbitrates a collision; chunks
  from one member keep their order.
* **Counting.** The link counts each delivery, as a split does, so a chunk of
  `n` bytes from one member of a bus of five adds `4n` bytes and four messages
  to the link. The traffic ring shows it once as received on the sender and
  once as sent on each receiver.
* **Removal.** Removing the link frees its members. Removing a member takes it
  out of the bus and leaves the others on it; the bus goes when fewer than two
  remain.

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
| `rfc2217-listen://` | The client's, but several changes that arrive together are seen as one | No |
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
and what the others had on the wire is no longer delivered to it. The members of
a bus are fixed when it is made, so none joins a transmission half way.

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
