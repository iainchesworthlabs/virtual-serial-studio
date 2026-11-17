# Break as a timestamped event: a plan

This page plans how Studio carries a serial BREAK as an event with a start time and a length, in order with the data
around it, from a program or a device to every place a byte can go. It is a plan for release candidate 7 and after. It
changes no code. The driver interface, the capture format and the plugin API are all touched by it, so each phase waits
for the owner's decision on the points in the last section.

It starts from the [baud rate, framing and break audit](engine-serial-audit.md). The findings of that page about break
were read again in the source before this plan was written. They hold, with the additions in the next section.

## What is there now

| Place | What it does with a break | Source |
|---|---|---|
| Driver, program side | `IOCTL_SERIAL_SET_BREAK_ON` and `_OFF` set one flag, `Break`, and raise the state generation if the flag changed. No time is kept, no length, and no count of the bytes written before the call | `drivers/windows/vsscom/io.c:1152`, `hub.c:183`, `hub.c:203` |
| Driver, hub side | `VSSCOM_HUB_STATE.Break` is a level. The helper waits on `IOCTL_VSSCOM_HUB_WAIT_STATE` and reads the whole state when the generation changes, so a break that goes on and off between two reads leaves the flag as it was and is lost | `public.h:142`, `vcom_driver_windows.cpp:692`, `:1007` |
| Driver, to the program | `IOCTL_VSSCOM_HUB_LINE_ERROR` with `VSSCOM_LINE_ERROR_BREAK` raises `EV_BREAK` and the break character in its place in the received stream. It has no length, and it is the only part of the path that already keeps order with data | `public.h:50` to `:100` |
| Engine | A break is the bit `control_line::break_signal`. `endpoint::emit_lines` publishes a `lines` event stamped when the state was read. `hub::route_lines` passes DTR and RTS only through `modem_wiring::apply` and drops the rest. A link with `emulate_modem_lines` off, and any bus, carries no lines at all | `control_lines.hpp:31`, `:126`, `endpoint.cpp:868`, `hub.cpp:1644` |
| Real serial port | A break is the read error `break_detected`. The endpoint counts it and writes at most one log line a second. `platform_set_break` can send a break, with no timer | `serial_transport.cpp:326`, `endpoint.cpp:329`, `native_lines_*.cpp` |
| RFC 2217 server | Takes SET_CONTROL 5 and 6 (break on and off) as the listener's BREAK output. Sends no NOTIFY_LINESTATE. The code has no line-state notice at all, only the modem-state one | `rfc2217.cpp:330`, `:37` |
| Capture | `.cap`, `.csv` and `.jsonl` hold data records with `offset_ms`, whole milliseconds. The reader skips a line it does not know and counts it | `capture.cpp:352`, `:543`, `:706` |
| Pro capture point | The plugin recording drops the break bit on purpose ("The break signal has no place in a recording") and keeps `lines` changes for CTS, DSR, DCD and RI only | `capture_point.cpp:373` |
| Plugin API 1.1 | `decoder.data` has a `line_error` item with kind `break` and a time but no length, and a `line` item that has no break. `fixture.fault` can raise a break at a port. `timeline.load` plays data and the four input lines, and counts faults as not played | `plugin-api-reference.md` |

Two further facts shape the plan. First, the driver and the program's writes meet at a ring: the program's bytes go into
`Tx` and the helper reads them as they are. A break flag read from the state says nothing about where among those bytes
the program called `SetCommBreak`. Second, `SetCommBreak` has no length. A program holds the break by calling it, waiting
(`Sleep`) and calling `ClearCommBreak`, so the length is the gap between two calls, and only the driver sees both at the
right time.

## 1. The event model

One new event kind, `event_kind::line_break`, with two members added to `endpoint_event`: `duration` (nanoseconds) and a
`break_phase` that is `begin`, `end` or `complete`.

* **`begin`** says the line went to break at `when`. It has no length yet.
* **`end`** says the line came back, and carries `start` (the begin time), `when` (the end time) and `duration`
  (`when - start`). A consumer that wants one record per break reads only `end`.
* **`complete`** is a begin and an end that the source knows together, as the driver does for a short break, and as a
  real port does when it reports a break it has already seen. It carries `start` and `duration` and is routed and
  recorded as one item. A consumer that treats `begin` and `end` as the pair treats `complete` as both.
* **Times** are nanoseconds on the hub clock (`hub::now()`), as every other event time is. A source that has a time of its
  own converts it before the event is made (section 2).
* **Measured or not.** A real device gives no length. Its break is `complete` with `duration` set to one character time
  of the port's current format (a break is at least that long, or the receiver would not call it one) and `measured`
  false. The log and the recording say "at least". DMX512 (88 µs) and LIN (13 bit times) decoders must not trust an
  unmeasured length, and the plugin item (section 6) says so.
* **Direction.** `direction` is `rx` for a break the endpoint received (a device, or a client's break) and `tx` for one a
  program or the engine sent out through it, as for data.

**Order with data.** A byte stream with a break between two bytes is the main requirement, and it fixes where the break
lives. A break is not a `lines` event. It is an item in the same ordered flow as a data chunk, so a source emits the bytes
before it, then the break, then the bytes after, and every stage that keeps the order of chunks keeps the order of
breaks: the read stage, `route_chunk`, the delay line of a link, the half-duplex bus wire, and the traffic ring that feeds
the monitor and the plugins. `route_chunk` takes a `bytes_view` today. It becomes a function over an item that is bytes or
a break, so the loop checks (the visited set, the links a chunk has crossed, the monitor direction test) run once for both
and cannot drift apart.

**A short break is not lost.** The source gives a `complete` item or a `begin` and `end` pair with its own stamps. Nothing
is read off a level. The `lines` event still shows `BREAK` while it is held, for the Event log and the line matrix, and
keeps its current meaning; it is a view of the level and not the record.

**Long breaks.** A program may hold a break for seconds. `begin` goes on at once and `end` when it ends, so a link and a
recording show a break that is still running. Only the driver port and the RFC 2217 server act on a break at its end (a
receiving UART reports a break once, after a full character of low line), so a port that is not told about the beginning
loses nothing.

## 2. The driver change

The aim is that the driver stamps both calls with a monotonic time and the byte count, and the helper reads them in order
and loses none.

* **A break record.** In `VSSCOM_DEVICE_CONTEXT`, a small ring (64 entries) of records:
  `{ OnTime, OffTime, TxOffsetOn, TxOffsetOff, Flags }`. `OnTime` and `OffTime` are raw `KeQueryPerformanceCounter`
  counts. `TxOffset*` is a 64-bit count of bytes the program has ever put into `Tx`, read under the same lock as the
  flag, so the helper knows exactly how many bytes were written before each call. `SET_BREAK_ON` opens a record (if none
  is open), `SET_BREAK_OFF` closes it. A second `ON` with a break held changes nothing, as now. A close with nothing
  open changes nothing. A full ring drops the oldest closed record and counts it in `BreaksDropped`.
* **A new request** `IOCTL_VSSCOM_HUB_READ_BREAKS` (METHOD_BUFFERED, output only): returns the pending records in order
  and takes them from the ring, with `QpcFrequency`, the count dropped since the last call, and a flag that a record is
  still open (a break being held). The state structure and `IOCTL_VSSCOM_HUB_WAIT_STATE` stay as they are. The generation
  already rises on every change of `Break`, so the helper learns there is something to read and calls the new request. A
  new field `BreakCount` in the state (records added so far) lets the helper skip the call when nothing is new.
* **The helper** (`vcom_driver_windows.cpp`) turns each record into hub items. It keeps the count of bytes it has read from
  `Tx`. After every read of `Tx` it fetches records whose `TxOffsetOn` is within what it has read (a record is stored
  before any later byte enters the ring, so a fetch made after the read cannot miss one). It splits the chunk it just read
  at the offsets, so the order is bytes, `begin`, `end`, more bytes. Bytes the program wrote while a break was held come
  after the `end`: a real UART holds its output while the line is in break, and the offsets say which they are. The helper
  converts the counts to the hub clock using the QPC frequency. `std::chrono::steady_clock` is QPC on MSVC; the test in
  phase 2 checks the two agree to within the read delay.
* **Version.** `VSSCOM_VERSION` goes from 4 to 5. The helper already opens a port only when the version is exactly the one
  it was built for (`vcom_driver_windows.cpp:1122`), and the notes of this project record that the driver and Studio have
  not been deployed beyond the owner's own machines and guests. So there is **no compatibility code**: no old-helper path,
  no old-driver path, no fallback to the level. Installing Studio installs the matching driver, and a mismatch is the
  existing "driver and Studio do not match" refusal. The file `docs/windows-com-driver.md` gets a line under Versions.
* **Towards the program.** The existing `IOCTL_VSSCOM_HUB_LINE_ERROR` already puts a break in order with the bytes around
  it, so a break that arrives from a link, a device or a client is delivered with it at its `end` (or `complete`). A
  Windows program cannot see a break's length, so none is passed. Holding the bytes after it back for the length of the
  break, and delivering chunks on a gap, is the separate gap-accurate work in the audit and is not part of this plan.
* **Guest test.** The change is in a kernel driver, so the check runs in a guest, one guest at a time (see the project's
  notes on guest driver runs and `docs/vm-testing.md`). A small test program opens the port, writes `AB`, calls
  `SetCommBreak`, sleeps 5 ms, calls `ClearCommBreak`, writes `CD`, repeats this with sleeps of 0, 1 and 100 µs worth of
  busy wait, and closes. The helper side test reads the engine events and checks: order `A B break C D`; length within 1 ms
  of 5 ms for the long one and non-zero for the short ones; no break lost when 1000 are made in a tight loop (counted
  against `BreaksDropped`); a break held when the program closes the handle ends at close; a break held while the helper
  restarts is reported as open and then ended. Driver Verifier is on for the run, as in the other guest runs. Unit tests of
  the ring and the offset arithmetic go into `drivers/windows/vsscom/core` where the other host-testable code is.

## 3. The engine

* **Source side.** `transport` gains an optional way to report a break, a callback or an item in the read completion with a
  start, a duration and `measured`. A transport that cannot say anything keeps today's behaviour. The endpoint stamps
  nothing itself for a driver break (the driver's stamps are used) and stamps a real device's break with the read time.
* **Routing.** `route_lines` is not the carrier. A break goes where data goes: over every link and bus route of the
  endpoint, in the order of the data, through the delay line of the link, so a link with a delay or jitter delays a break
  the same as the bytes before it and never overtakes them.
    * **Links.** A break written by a program on one port appears on the other ports of the link as a break, after the
      bytes the program wrote before it. A split gives it to every listener, a join to the target, as with data.
    * **Direction and pass-through.** The rules of `docs/architecture.md` apply as written: a route is taken only in its own
      direction, an owner link forwards only for its owner, and a link does not return an item to where it came from or send
      it over a link it arrived by. A `monitor` copies a break as it copies the bytes: its `direction` of `transmit` shows
      the breaks the monitored port's program sends, `receive` the breaks that links bring into it, `both` both. A monitor
      that has modem lines off still gets the break, because a break is data-like (it takes place in the stream), not a
      modem line. `emulate_modem_lines` has no say over it.
    * **Loop safety.** The visited set and the crossed-links list that keep a chunk from going round a loop are the ones
      the item uses, since it is the same function. A test builds a ring of three links and checks one break is seen once
      by each port.
    * **Policy and options.** The link's `delay`, `jitter` and `policy` apply to the item. A `drop` policy that discards a
      chunk discards the break at that place too. No new link option is added (decision 4 asks whether to add one that
      turns break carrying off).
* **Half-duplex bus.** A break is a transmission with a length. The line takes it as a `transmit` of zero bytes lasting
  `duration`, so another member's bytes during it are an `overlap`, a break begun inside another member's chunk is the same,
  and the bus holds the line for the break and then for the turn-around. The bus counts ten bits a byte from its own bit
  rate today; a break counts for its measured length, not for a number of bits. A bus fault event names the break as the
  cause. A break that is not measured holds the line for one character time at the bus rate.
* **Counters.** `endpoint_stats` gets `breaks_rx` and `breaks_tx`, `link_stats` gets `breaks`, and the read-error count
  for a real port's break stays in `errors` as now (the break is both an event and a counted line fault, so nothing that
  reads `errors` changes). The agent and the control protocol add the counters to the stats they give; a field the old
  agent does not send is absent and reads as zero.
* **Fixtures and the test kit.** `vss_testkit` fixtures gain a way to make a break with a length, and the fault injection
  `break` becomes a `complete` item of one character time, so the existing rule-set action `line_error: break` keeps
  working and shows in the same places.
* **Capability.** A transport that cannot carry a break to its program (a POSIX pseudo terminal, which has no way to
  raise one at the slave that the project has found) says so through a capability flag, the engine logs "break not
  delivered: this port cannot show one to the program" once for the port, and counts it as dropped. This is the rule in
  `docs/gui-design/docs/08-engine-gaps.md`: degrade visibly. Phase 1 checks Linux and macOS before the flag is set.

## 4. RFC 2217

RFC 2217 limits what is possible, so the plan says what each direction can carry.

* **A client sets a break (SET_CONTROL 5, then 6).** This is the one place a length can be made from a network client: the
  listener stamps the receipt of 5 as `begin` and of 6 as `end` with the hub clock and sends the `end` item with the
  length. The length includes network delay and jitter, so the log says it was made from two commands. If the client
  drops while the break is on, the listener ends the break at the disconnect.
* **A program or a device sends a break towards a client.** RFC 2217 has no command from a server to a client that sets a
  control state: SET_CONTROL is a client request and the server only answers it. The valid way to tell a client about a
  break is `NOTIFY_LINESTATE` (command 6, answered as 106), line-state bit `0x10` (break detect), and only after the client
  has asked for it with `SET_LINESTATE_MASK` (command 10), which the server already accepts. The plan sends one
  `NOTIFY_LINESTATE` with `0x10` for each `end` or `complete` item that reaches the listener's port, in order with the
  data written before it.
* **The length.** RFC 2217 has no field for it. The plan does not invent one. Two choices, for the owner:
    1. send the notice once, at the end, with no length. A client sees a break and nothing more. This is plain RFC 2217 and
       interoperates with every client.
    2. send `NOTIFY_LINESTATE` at the begin and again at the end, with the bit set the second time only. A client that
       reads both can time the gap, but the time includes network jitter and some clients may treat the first as a
       break. Not recommended.

  The recommended choice is 1. The length is kept in Studio's own log and recording, where it is exact, and the notice
  carries the fact. The listener's state shows the break length in the inspector as "length at the source".
* **What Studio receives from a real device.** A real adapter reports a break to the host in one of two ways. Windows
  reports `EV_BREAK` with `WaitCommEvent` and a `CE_BREAK` error bit from `ClearCommError`, usually with a `0x00` byte;
  POSIX reads a `0x00` byte (with `BRKINT` off and `IGNBRK` off) or, with `PARMRK`, `0xFF 0x00 0x00`. Neither gives a
  length or a time better than the time the read returned. The serial transport is changed to turn this into a
  `complete` item with `measured` false, in order with the bytes read before it, and to drop the stray `0x00` that goes
  with it (decision 5). A device that does not report a break gives nothing, and the plan cannot add it.
* **Studio has no RFC 2217 client.** Sending a break to a remote device through RFC 2217 (SET_CONTROL 5, a timer, then 6) is
  possible only when Studio has a client transport. It is not planned here. The engine's item already carries what such a
  client needs, so adding it later changes the transport only.
* **Sending to a real port.** A `begin` item arriving at a real serial port calls `platform_set_break(true)`, and the
  `end` calls it with false. A `complete` item is made into the same two calls with a timer of `duration`, which is
  accurate to the system's timer (about a millisecond on Windows). The log says the length on the wire may differ.

## 5. Recording and replay

* **The capture formats** (`vss::studio` capture, community) get a new version. Today the `.jsonl` header says
  `"version": 1`, a data record has `offset_ms`, and the reader counts and skips a line it does not know.
    * `.jsonl` version 2: a data record gets `offset_ns` (an integer) in addition to `offset_ms`, which is still written so
      a version 1 reader keeps working. A break record is `{"type":"break","offset_ns":…,"offset_ms":…,"port":…,
      "direction":"R","phase":"complete","duration_ns":…,"measured":true}`. A version 1 reader sees no `hex` member, counts
      the line as skipped and carries on, so old readers lose the break and nothing else.
    * `.csv`: the columns are fixed in the header line, so two columns are added at the end, `offset_ns` and
      `duration_ns`, and a break row has direction `B` and empty bytes and hex. A spreadsheet that opens the old columns
      still works.
    * `.cap`: this text format is meant for other tools to read, so it changes least. A break is one comment line,
      `# break 12.345678 R 88000 ns`, which every reader ignores and Studio's reader parses. Times in `.cap` stay as they
      are (milliseconds), and the break line holds the finer time.
    * The writer keeps the file open and writes a begin as nothing and an end or complete as one record, so a break that
      is still running when the capture stops is written with the length so far and `"open": true`.
* **Timestamps.** The record's time is the item's own `when` (nanoseconds from the capture origin), not the time the
  capture saw it, so a chunk and a break keep the order and the gaps the hub saw. The data times are the hub's read times,
  as now; this plan does not make them the program's write times (the audit item on per-write stamps is separate, and the
  record ring of section 2 could carry them later).
* **The recording of the Pro Diagnostics plugin** (`recording_format` 1, nanosecond times already) gets a line type
  `break` with `t_ns`, `dir`, `duration_ns` and `measured`. The plan checks `recording.cpp` for how the reader treats a
  line type it does not know. If it refuses, `recording_format` becomes 2 and the reader accepts 1 and 2; if it skips,
  the format stays 1 and a break is the new optional line type. The host's capture point stops dropping the break bit and
  writes the item instead of a `lines` change.
* **Timeline and replay.** `timeline.load` plays a break line by raising a `begin` item on the recorded place's port at
  the line's time and an `end` at `t_ns + duration_ns`, both at `time_scale`. It is never early, as for data. A break
  with `measured` false plays as one character time of the port's format. The replay counts `breaks_not_played` for
  places that cannot deliver one (the capability flag of section 3) in place of the generic fault count. The capture
  `replay` of Studio's own reader (used by `vss_cli`) gets the same treatment. A recording that has no break lines plays
  exactly as now.
* **Fuzzing.** `fuzz_plugin_message` and the capture reader need seed files with break lines, and the corpus replays in CI.

## 6. The plugin API

No new message is needed for a decoder, and one new message for a fixture.

* **Decoders** receive a new `item` of `decoder.data`: `break`, with `place`, `direction`, `offset` (the bytes at that
  place and direction before it, so a decoder puts it in the byte stream), `t_ns` (the start), `end_ns` (the end, absent
  for a `begin` that has not ended), `duration_ns` and `measured`. A decoder takes one item for a complete break and does
  not need to pair two. It sits in order with the `data` items, as the format says items do. Decoders that came with API
  1.1 do not know it, and the host refuses unknown items, so the host sends `break` only to a decoder whose manifest
  requires the new capability `events_break`, and sends the old `line_error` item of kind `break` (with its time) to the
  others. The API version becomes 1.2, and Studio offers `events_break`.
* **What the three decoders do with it.** DMX512 starts a frame at `break`, checks `duration_ns` against 88 µs when
  `measured`, and reads the mark after break and the start code from the bytes that follow. LIN checks the length against
  13 bit times of the rate the user set for the place, and reads the sync byte next. Modbus RTU needs no break; it needs
  the finer times that the data items already have in `t_ns` and `end_ns` (nanoseconds), and the delivery on a gap that
  is not part of this plan.
* **Fixtures** gain `fixture.break` (plugin to host, notification): `instance`, `duration_us` (1 to 10000000), `after_us`
  (optional, 0 to 60000000). It is ordered with `fixture.reply` of the same instance, so a reply, a break and a reply go
  out in that order. This is how a DMX512 or LIN simulator sends its frame. `fixture.fault` with `break` stays and means
  a break of one character time.
* **Triggers.** `trigger.publish` kind `line_error` with code `break` keeps working for an unmeasured break. A
  measured one adds the length to the trigger's context, with no new kind.
* **Capability and design sections.** `docs/plugin-api-reference.md` and section 20 of `docs/plugins.md` gain the item,
  the message and the capability, and the test that checks that every message of the reference is in the code covers
  them.

## 7. The user interface

* **Monitor and Log.** A break is a row of its own, not a data row and not a `lines` row: Event "Break", Detail "88 µs" or
  "at least 8.7 µs" for an unmeasured one, "running" for a begin that has not ended. In the byte view it is a marker
  between the bytes, with the length in its tooltip, so the order with data is visible. The `lines` row that says BREAK
  while it is held stays in the Event log and the line matrix. The strings are written with `tr()`, in the project's
  plain style, and the new rows have an accessible name.
* **Send panel.** The user can send a break of a chosen length: a "Break" button with a length field in microseconds
  (default 1000, 1 to 10 000 000). The engine makes a `begin`, waits with a timer on the endpoint's strand and makes the
  `end`. On a driver port this is raised at the program as one break; on a real port it is `SetCommBreak` for the length;
  on an RFC 2217 listener it is a `NOTIFY_LINESTATE`. The panel says "The length on the wire is accurate to about a
  millisecond" when the port is a real one. The button is off, with a reason in its tooltip, for a port whose capability
  flag says it cannot send one.
* **Counters.** The inspector shows breaks received and sent beside the byte counts.

## 8. The JSON topology schema

No phase needs a change to the schema. A break is traffic, not configuration. It uses no new link option (decision 4 is
asking whether to add `carry_break` to links). If the owner says yes, that is an optional boolean written only when false,
as `direction` is written only when it is not `both`, so old files keep their meaning and the rule in `AGENTS.md`
about the schema is met by asking first. Capture and recording formats are separate from the topology and are covered in
section 5.

## 9. Phases

Effort is working days for one person including tests and docs. Pro here means in the Pro repository, outside this one.

| Phase | Work | Effort | Risk | Edition | Test plan |
|---|---|---|---|---|---|
| 1 | Engine item: `event_kind::line_break`, `route_chunk` over an item, delay line and bus, counters, fixtures, Event log and Monitor row, loop and monitor tests. RFC 2217 listener turns SET_CONTROL 5 and 6 into begin and end | 8 | Medium. `route_chunk` is the hot path | Community | Unit and system tests: order with data across a link with delay; ring of three links; monitor direction; bus overlap; TSan job; read-stage cost measured with the perf report |
| 2 | Driver version 5: break record ring, `IOCTL_VSSCOM_HUB_READ_BREAKS`, helper reads and splits chunks, QPC conversion, version doc | 7 | High. Kernel code and a version bump | Community | Ring and offset tests in `core/`; one guest at a time with Driver Verifier; 1000 breaks in a loop; hold and close; helper restart |
| 3 | RFC 2217 `NOTIFY_LINESTATE` and the line-state mask; real port break in and out (`measured` false, stray `0x00` dropped); POSIX capability flags | 5 | Medium. Real adapters differ | Community | Loopback with a client written in the test; a USB adapter with a break source on the bench for the owner; Linux and macOS pty checks |
| 4 | Capture version 2 in `.jsonl`, `.csv`, `.cap`; Studio reader and `vss_cli` replay; changelog fragment | 4 | Low | Community | Round trip; version 1 file read by the new reader; version 2 file read by the version 1 reader; fuzz corpus |
| 5 | Plugin API 1.2: `break` item, `fixture.break`, `events_break` capability, `timeline.load` break lines, recording line type, host capture point; docs of the reference | 7 | Medium. Public API | Host in Community. Diagnostics and replay plugin, and DMX512, LIN and Modbus RTU decoders, in Pro | Message round trip and fuzz; a test plugin that decodes a DMX frame from a driver port and a link; timeline with a break; old decoder still gets `line_error` |
| 6 | Send panel break and length; inspector counters; copy and translations | 3 | Low | Community | GUI tests (`vss_gui --selftest`); keyboard and 200 percent text; both themes |

Phase 1 can start now and does not depend on the driver. Phase 2 can run in parallel with 1 once the item type is merged.
Phases 3 and 4 follow 1. Phase 5 follows 1 and 4. Phase 6 follows 1. All six are about 34 days. Phases 1, 2 and 4 are what
DMX512 needs to see a break at all; LIN needs phase 3 for a real device; Modbus RTU gains from phase 4 only (nanosecond
times).

## Decisions for the owner

1. **Go for phase 2, a driver version bump to 5**, with no compatibility code, on the strength of the notes that no
   driver or Studio has shipped beyond the owner's machines and guests. Confirm this is still true before the first
   release candidate that carries it.
2. **Where a break falls among the program's bytes:** the plan puts it where the program called `SetCommBreak`, in program
   order, and delivers bytes written during the break after it ends. Real hardware does the same in principle. Say if the
   other reading (the break goes out at once, ahead of queued bytes) is wanted.
3. **`begin`, `end` and `complete`, or one item only?** The plan has all three so a long break shows while running. One
   item at the end is simpler and delays what links and the log show.
4. **A link option `carry_break`** (a schema addition, default on), or none. The plan recommends none and a break
   that always goes where data goes.
5. **The stray `0x00` from a real device's break**: drop it (the plan's choice, so the byte stream is clean and the
   break is an item), or keep it and also send the item.
6. **RFC 2217 length:** option 1 (one notice at the end, no length) or option 2. The plan recommends option 1.
7. **A real device's unmeasured break** is given one character time as its length with `measured` false, or a length of
   zero with `measured` false. The plan recommends one character time because a receiver would not call anything shorter a
   break.
8. **Capture format:** version 2 as above (`offset_ns` added, `offset_ms` kept, `.cap` gets a comment line), or a break
   in `.jsonl` and `.csv` only and nothing in `.cap`.
9. **Plugin API 1.2 and `recording_format`:** accept the API version bump and the `events_break` capability, and say
   whether a recording with a break line may stay `recording_format` 1 if the reader skips unknown lines (decided from
   `recording.cpp` in phase 5) or must be 2.
10. **Pro boundary:** the host side of phase 5 is in this repository and the plugins are Pro. Confirm the DMX512, LIN and
    Modbus RTU decoders are Pro work and not community.
11. **Out of scope, to be confirmed:** delivering bytes to a driver port on a gap, holding bytes back for the length of a
    break at the receiving program, per-write stamps from the driver, and an RFC 2217 client in Studio. Each needs its
    own plan.
