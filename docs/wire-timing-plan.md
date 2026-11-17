# Wire time and gap delivery: a plan

This page plans how Studio keeps the gaps between bytes, and between bytes and a break, from the program that wrote them
to the port that reads them. It follows the [break event plan](break-event-plan.md) and the
[audit](engine-serial-audit.md), which found that a plain link delivers as fast as the data is written, that the driver
stamps nothing per write, and that the receive side pays nothing out at the times the bytes were sent. DMX512 (the mark
after a break), LIN (the response space) and Modbus RTU (3.5 characters of silence ends a frame) all need this.

It changes no code. The driver interface and a link option are involved, so a phase waits for the decisions at the end.

## What is there now

* A program's writes go into one ring, `Tx`, and the helper reads what is there (`hub.c:693`, `:610`). Two writes made
  close together are one read, and a slow helper merges more. The chunk's time is the read time on the hub clock
  (`endpoint.cpp:305`), not the time of the write.
* A link delivers a chunk as soon as it arrives, after its own `delay` and `jitter`, through a delay line that reads the
  hub clock (`libs/vss_core/src/delay_line.hpp`). It does not space the bytes of a chunk, and it does not know how long the chunk takes on a
  wire. Only a half-duplex bus does: it holds a message for `10 × bytes ÷ bit_rate`, with its own rate, and ten bits a byte
  whatever the framing (`bus_line.cpp:21`).
* The driver's receive side is a ring, `Rx`. A program's read takes what is there. The driver already has the program's
  read timing (`vsscom_plan_read`, for the interval and total timeouts) and the true time of a character
  (`vsscom_tx_time`, used only for RTS toggling), but nothing pays bytes out over time.
* The engine does know the sender's format when the sender is a driver port: `transport::peer_format()` gives the rate and
  framing the program set.

The consequence is that bytes a program wrote 10 ms apart arrive at the other end together if the helper was slow, and
bytes written in one `WriteFile` arrive at once even though a wire would take a millisecond or more for them. A Modbus
RTU receiver that decides a frame by silence cannot be tested, and a DMX512 break is followed by data with no mark.

## The approach: pace in the engine, stamp in the driver

**Receive side needs no driver change.** A program that reads a port sees what the engine wrote and when. If the engine
writes each byte, or each short run of bytes, to the hub end of the port at the time a wire would have delivered it, the
program's own read timing (interval timeout, `WaitCommEvent`, a Modbus stack's silence timer) sees real gaps. A real
UART also buffers, so a program that reads late sees the merged bytes, as it would on hardware. The plan therefore does
the pacing in the engine and leaves the driver's `Rx` ring as it is.

**Send side needs a stamp.** The one thing only the driver knows is when the program called `WriteFile`. The break plan's
record ring (a record with a QPC count and an offset into `Tx`) takes a second kind of record: one for each write, with the
time of the call and the offset of its first byte. The helper splits what it reads at those offsets, so a write is one
chunk and its time is the program's, not the helper's. A burst of writes faster than the helper reads is kept in the ring,
so the gaps between them stay; if the ring is full the oldest is merged into the next and counted in `WritesMerged`.

This shares the interface, the helper loop and the guest test with the break records, so it goes into the same driver
version, 5, and is one version bump and one set of guest runs.

## The model

**Wire time of a chunk** is `bits_per_char × bytes ÷ rate`, where `bits_per_char` is 1 start, 5 to 8 data, 0 or 1 parity,
and 1, 1.5 or 2 stop (8E1 and 8N2 are 11 bits; the bus's flat ten is wrong for them). A break is on the wire for its own
length, and the mark after it is the idle time the sender chose.

**The release time** of an item at the far port is
`release = max(source_time + link delay + jitter, previous_release + previous_wire_time)`.
The second term is what makes a break hold the bytes behind it, and a long chunk hold the next one, as a serial line does.
Items keep their order, so the delay line stays a queue.

**Whose rate and format.** The sender's, in this order: the format the program set on the sender port (`peer_format`), the
format set on the endpoint in the topology, and a `bit_rate` and framing given with the option itself (a plain link has none today; only a half-duplex bus does). If none is known the item is not
paced, and the log says "no rate for this link" once. The far port's own rate is not used, because a link does not
convert a rate: it carries bits at the speed they were sent. A rate mismatch between the ends stays a warning, as now.

**Granularity.** Chunks are released whole when their wire time is shorter than the timer's accuracy, and in pieces when
it is longer: a chunk is split at character boundaries into runs that each take at most the larger of the timer accuracy
and one character time, so a 512-slot DMX frame at 250000 baud (about 22 ms) is a handful of releases and not 512 timer
waits. A Modbus frame stays one write if its wire time is below the accuracy, and its silence is then the gap before the
next item.

**Timer accuracy** is the limit. A normal Windows timer is about 1 ms (15 ms on an old setup), and 3.5 characters at 115200
baud is about 330 µs. The engine uses a high resolution waitable timer where the system has one (about 0.5 ms) and, for a
release less than 1 ms away, spins on the clock for the remainder. The spin is bounded (decision 3) and the lateness of
every release is counted, as it is for response rules, so the engine never claims a time it did not keep.

**Where it lives.** The delay line is where chunks wait for a link's delay, and it already reads the hub clock, so pacing
is a new stage in that line and a per-route "previous release" value. A route with pacing off is untouched and pays one
test. The half-duplex bus keeps its own wire, but takes its message length from the same function, with the real framing,
so a bus and a link agree and the flat ten is retired.

**Break.** An item of the break plan is paced like a chunk with the wire time of its length (an unmeasured one counts one
character time). The next byte is released no earlier than the break end plus the mark after it that the sender made, so a
DMX512 receiver sees the break, the mark, then the start code. This is the "hold the bytes back after a break" work.

## The option and the schema

Pacing is a property of a link, so it needs a link option, `wire_timing`: `off` (default) or `on`, with an optional `bit_rate` and `framing` for a link whose sender reports none. It is written in the
saved file only when `on`, as `direction` is written only when it is not `both`, so older files read as `off` and a
file written with `on` opens in an older Studio as a link without pacing, with no error. This is the one place either
plan touches the topology schema, and AGENTS.md says to ask first (decision 1). The option applies to every link kind,
including a monitor. A bus keeps `half_duplex` and does not take the option, because it already has a wire.

Pacing is off by default because most traffic is not timing-sensitive, and because spinning for a sub-millisecond gap costs
CPU. A scenario or a plugin that needs it turns it on for the link it uses; the DMX512, LIN and Modbus RTU fixtures of the
Pro plugins ask for it in their link settings.

## Platforms

* **Windows driver port to a driver port:** the full model, with the program's own write times from phase 1.
* **A real serial port as a source:** read times are the host's, so a device's gaps are as accurate as the read loop. The
  plan claims no more.
* **A POSIX pseudo terminal:** the engine paces its writes to the master, so the program on the slave sees the gaps within
  the system's timer. The pty keeps its framing at 8N whatever the program set, so wire time is counted from the format the
  program set and not what the pty uses, and a "not the same" note is logged for a 2 stop bit program.
* **RFC 2217:** pacing applies to what the listener sends to the socket, but TCP merges and splits as it likes. The gaps
  a client sees are not reliable and the page says so.

## Phases

Effort is working days for one person including tests and docs. G1 is part of phase 2 of the break plan and is not counted
twice.

| Phase | Work | Effort | Risk | Edition | Test plan |
|---|---|---|---|---|---|
| G1 | Driver write records in the same ring and version 5 as breaks; helper splits `Tx` reads at the offsets; `WritesMerged` | 3 on top of break phase 2 | High. Kernel and the same guest runs | Community | Ring tests in `core/`; guest: 100 writes 1 ms apart arrive as 100 chunks with the program's gaps within 0.5 ms; a burst of 10000 merges and counts |
| G2 | Wire time function with real framing; pacing stage in the delay line; `wire_timing` link option and its file, control and GUI reading; bus uses the same function | 8 | Medium. Delay line is hot and the option is a schema addition | Community | Unit tests on a manual clock for release times, break hold, split into runs; schema round trip with an old reader; TSan job; perf report |
| G3 | Timer accuracy: high resolution timer, bounded spin, lateness counters, platform notes | 4 | Medium. CPU use and platform differences | Community | Lateness histogram on Windows, Linux, macOS; CPU at 1000 releases a second; a stress run with 64 paced links |
| G4 | End-to-end: DMX512, LIN and Modbus RTU fixtures that pass their timing checks through a paced link and the driver | 5 | Medium | Fixtures and decoders Pro; engine tests Community | A Modbus slave stack in a guest answers only when the frame gap is 3.5 characters; a DMX512 receiver sees the mark after a break; a LIN slave answers inside the response space |
| G5 | RFC 2217 client transport (SET_CONTROL break with a timer, line-state in) | 8 | Medium. New transport | Community | Loopback with Studio's own listener; a real device on the bench |

G5 is a separate subsystem that shares nothing with G1 to G4 except the break item, and can run on its own after phase 1 of
the break plan.

## Decisions for the owner

1. **The `wire_timing` link option**, default `off`, written only when `on`. A schema addition, so it needs your yes.
2. **Where the rate comes from:** the sender's program-set format, then the endpoint's setting, then a rate given with the option
   (the plan's order), and no pacing, with a logged note, when none is known.
3. **The spin limit:** the most a paced release may spin on a core for the last part of a gap. The plan suggests 1 ms a
   release and a cap of one core's 10 percent over a second, after which it falls back to the timer and counts the
   lateness.
4. **Fold G1 into the break plan's driver version 5**, one bump and one guest run. The plan recommends yes.
5. **The bus:** replace the flat ten bits a byte with the real framing now (changes the numbers a bus reports and the
   collision tests), or leave it until the owner's next bus change. The plan recommends now, in G2.
6. **G5**, an RFC 2217 client: part of this work, or its own plan later.
