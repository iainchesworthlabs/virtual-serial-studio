# 9-bit serial and multi-drop addressing (design)

Status: **proposal**, not built, and not decided. This page works out how Studio could carry and show 9-bit serial
traffic, and what part of that is a Pro feature. Nothing here changes `libs/vss_core` behaviour or the JSON topology
schema today. Where the page says "schema", it means the saved setup file, and it says which option would change it.

Break, baud and parity pass-through in the engine and the driver is being audited separately. This page does not repeat
that work. Its findings are not in the repository yet, so the points below that depend on them are marked *(audit)*.

## What 9-bit serial is

A character on a UART is a start bit, 5 to 8 data bits, an optional parity bit and one or more stop bits. In a 9-bit
scheme there are nine data bits, or eight data bits and a ninth bit that carries meaning instead of parity. On a bus
with one master and many slaves the ninth bit says what the other eight are:

* **Ninth bit set: an address.** Every receiver looks at it. A receiver whose address does not match ignores the bytes
  that follow.
* **Ninth bit clear: data.** Only the receiver that matched the last address keeps it.

This is where it is found:

| Where | How the ninth bit is used |
|---|---|
| MDB (the vending machine bus) | 9600 baud, 9 data bits. The master sets the ninth bit on the first byte of a message (the address and command). The slave sets it on the last byte of its answer |
| Intel 8051 style multiprocessor mode (and the many microcontrollers that copied it) | The ninth bit marks an address byte. A slave in address mode ignores bytes with the bit clear |
| Some RS-485 buses and PLC or building-control protocols | The same address-or-data idea. Which ones is a matter for the person who has the device, and Studio does not assume any |
| Modbus RTU | Not 9-bit. It uses 8 data bits and a silent gap of 3.5 characters. It is in the same family only because it is a multi-drop master and slave bus, and the gap is already a match in [response timing](response-timing.md) (`quiet_us`) |

The bit is not parity. Many UARTs that can do "9 bits" do it by using the parity bit position, and send it as mark or
space (always 1 or always 0) so that the value is the one the program chose.

## How Windows and real hardware handle it

The Windows serial API describes a port with `DCB.ByteSize` of 5 to 8, a parity of none, odd, even, mark or space, and
stop bits. There is no 9. A program that needs a ninth bit does one of these:

1. **Mark and space parity, changed byte by byte.** Set mark parity (the ninth bit is 1) and write the address byte.
   Wait for the transmitter to empty. Set space parity (the ninth bit is 0) and write the data. The program has to wait
   each time, because the parity setting applies to what is in the UART when it changes. Receiving works the other way:
   the program sets the parity it expects for data, and a byte whose ninth bit is the other value arrives as a parity
   error. The program learns it was an address from the error flag, `fErrorChar`, or `EV_ERR`. How well the error
   arrives in step with the data depends on the driver and the chip.
2. **A USB UART or adapter with a 9-bit mode and its own driver.** Some chips and multi-port boards have one, with a
   control call that sets the ninth bit for a write or reports it with each received byte. This is the vendor's own
   API and nothing in the standard serial interface carries it. It is not checked here which parts have it.

On POSIX the equivalent is `CMSPAR` (stick parity) with `PARMRK`, changed in the same way. A Linux pseudo terminal keeps
no parity at all: its slave is 8N1 whatever the program asks (`line_format::framing_known` records this), so a POSIX
virtual port cannot carry a ninth bit by that route.

### What the Windows driver sees

`drivers/windows/vsscom` is a KMDF function driver that implements the serial contract. The facts that matter:

* `IOCTL_SERIAL_SET_LINE_CONTROL` is accepted with the five parities (`SERIAL_PARITY_MARK` and `SERIAL_PARITY_SPACE`
  among them) and a word length of 5 to 8. It stores them in the port and adds one to `Generation` in
  `VSSCOM_HUB_STATE`. The studio sees the state when it next reads it.
* A write request is a run of bytes. Nothing in it says what the parity was when the program issued it.
* The change of parity and the write are not tied together. The studio knows that the parity is now space, and it knows
  the bytes it read, but not which bytes were written under mark. That is the whole problem for a decoder that only
  looks at state (option 2 below).
* On the receive side the driver already has `IOCTL_VSSCOM_HUB_LINE_ERROR` (`Errors`, `HasByte`, `Byte`). It is served
  in order with the bytes the hub wrote, and it raises the parity error to the program with the byte the receiver
  made of it. So **receiving a ninth bit already works with the driver as it is**: for a byte whose ninth bit does not
  match the parity the program set, the studio sends the byte followed by a parity line error. It costs one request for
  each address byte, and address bytes are few.

What is possible for a virtual port, then:

| | Windows virtual port | POSIX virtual port | Real port | RFC 2217 |
|---|---|---|---|---|
| Program to Studio, ninth bit | Needs a driver change: the driver notes the parity in force with each write request | Not possible | Possible by the mark and space route, slowly | No concept of it |
| Studio to program, ninth bit | Possible now, as a parity error | Not possible | Possible by the mark and space route | No concept of it |

## What the engine does today

The engine's data is bytes and only bytes.

* `endpoint_event::data` is a `shared_ptr<const byte_buffer>`, and `traffic_record` is the same. `read_stage::on_read`
  takes a `bytes_view`. Routing, the delay line, the bus and the response rules all pass or look at a run of bytes.
* `serial_settings` has `data_bits` (5 to 8, as `validate` enforces), `parity_mode` with `mark` and `space`, and stop
  bits. `parity_mode` is a **setting**, kept so that two ends can agree and so that a port can be opened with it. The
  engine does not model parity as a property of a byte, and it does not check or compute it. `line_format` and
  `same_format` compare the four fields so that a link between mismatched ports can be flagged.
* A half-duplex bus counts the bits of a character from the sender's framing (`vss::wire_time`: start, 5 to 8 data bits,
  parity, 1, 1.5 or 2 stop bits), or 10 bits when the framing is not known. It has no ninth data bit, so a 9-bit frame
  would be 11 bits and the line would run about 10 percent fast for 9-bit traffic.
* RFC 2217 (`libs/vss_ports/src/rfc2217.cpp`) sets baud, data size, parity and stop size, and carries a stream of bytes.
  It has nothing for a per-byte bit.
* The saved setup holds `data_bits` and `parity` for a port as numbers and words (`libs/vss_config/src/document.cpp`).
  The reader accepts 0 to 255 for `data_bits` and `validate` then refuses a value outside 5 to 8.

## The options

### 1. A ninth bit carried with the data

Every chunk may carry the ninth bit of each of its bytes, beside the bytes. A port that is not in 9-bit mode never
has one, so its chunks are exactly what they are now.

How it could be carried:

* **A sparse list of positions** where the bit is 1: a sorted array of offsets into the chunk, in a `shared_ptr` that is
  null for an ordinary chunk. Address bytes are a few in a long run, and this costs nothing when there are none.
* **A parallel bit mask**, one bit per byte, with the same null-when-empty rule. Simpler to index, and it costs a
  byte of mask for each eight bytes of a chunk that has any address byte.
* **A 16-bit symbol type** for all traffic. Rejected: it doubles every copy on the hot path and changes every
  consumer, to help a feature most users never turn on.

The sparse list is the better first choice. In every case the hot path takes **no per-byte allocation**: the chunk
allocates one small block when it holds at least one marked byte, and nothing otherwise.

### 2. Decoder only, using mark and space events as address markers

The engine stays bytes-only. A decoder watches the port's parity changes (the state the driver already reports) and
treats a change to mark as "the next bytes start with an address".

It cannot be made exact. The driver does not tie a parity change to the writes around it (see above), so the decoder
guesses by time. It also cannot work on a port that has no parity setting to watch, such as a POSIX virtual port, a
file or a network port. It needs no engine change, which is why it is the cheapest, and it is the right tool for
**reading** a capture of a program that already toggles parity, where a guess flagged as a guess is useful.

### 3. A separate endpoint type

A "9-bit port" endpoint, with its own transport that speaks 16-bit symbols, linked to others through an adapter.
It leaves the data path alone and moves the cost to the adapter. The price is that a 9-bit port cannot be linked
straight to an ordinary port, a bus, or a monitor; every existing feature has to be re-done for the new type or left
out, and the topology gains a new endpoint kind.

### Comparison

| | 1. Carried with the data | 2. Decoder only | 3. Separate endpoint type |
|---|---|---|---|
| Exact ninth bit | Yes | No, a guess by time | Yes, inside the type |
| Data path change | Chunk gains an optional field; links, delay line, bus, rules and events pass it on | None | None in the core; a new type and an adapter |
| Links and buses | Work as they are | Work, ninth bit is not there | Only through the adapter |
| Half-duplex timing | Bus charges 11 bits for a 9-bit member | Wrong by 10 percent | Own model |
| RFC 2217 | Ninth bit dropped, counted and reported | Not applicable | Ninth bit dropped at the adapter |
| Recording and monitor | A field in the record, a marker in the view | Notes a guess in a decoder's output | New record kind |
| Saved setup | A port setting (see below) | None | A new endpoint kind |
| Driver | Windows: a per-write tag | None | Windows: a per-write tag |
| Hot-path cost | One pointer test per chunk; one allocation per chunk that has an address byte | None | None |
| Plugin or engine | Engine carries it; a plugin reads and acts on it | Plugin only | Engine type; plugin on top |

## Option 1 in detail

### Engine

* **Chunk.** Add an optional `shared_ptr<const ninth_marks>` beside the bytes (events, `traffic_record`, the delay
  line's held chunk, and the argument of `read_stage::on_read`). A merge that concatenates chunks has to move the
  offsets. A split has to cut them.
* **Port setting.** A port is 9-bit when its `data_bits` is 9. That needs `validate` to accept 9, and it needs no new
  key in the setup file, because the key is already there and the reader already accepts it. A separate `nine_bit`
  flag is the other way, and it would add a key. The first is simpler and `same_format` then flags an 8-bit port
  linked to a 9-bit one for free. The Windows port still reports 8 data bits to the program, and the studio reads
  mark and space as the ninth bit when the port is 9-bit. *(audit: depends on what the audit finds about how parity
  reaches the hub.)*
* **Links.** An ordinary link passes the field on. A link to a port that is not 9-bit drops it and counts the drop
  (a counter in `link_stats`, so it is visible, as AGENTS.md asks for a missing capability).
* **Bus.** The half-duplex line charges `1 + 9 + 1` bits for a 9-bit member. `half_duplex_options` has no per-byte bit
  count now, so either the bus reads it from its members' formats or the option gains a field (a schema change, of an
  optional key with a default of 10).
* **Garble.** A collision on a bus inverts the bytes. The ninth bit inverts too, so a collision can make a data byte
  look like an address, as it can on the wire.
* **Response rules.** A rule could match on "address byte" as it matches on bytes now. That is an addition to the rule
  model in `libs/vss_rules`, and it is the only way an address filter can run at wire speed without a round trip to a
  plugin.
* **RFC 2217.** There is no way to carry the bit. The endpoint drops it, counts it and publishes a log event, and a
  capability flag says so (`docs/gui-design/docs/08-engine-gaps.md`). Studio could define a private framing
  between two Studios (two bytes a symbol over a raw TCP port), but that is a decision of its own and is not proposed
  here.
* **Recording.** The record format gains an optional field with the positions. Older readers ignore it. The format is
  covered in [the plugin framework](plugins.md); it needs a minor version bump.
* **Monitor.** A marked byte is shown with a marker, such as an `A` badge or a ninth column, in hex and text views.
* **Plugin protocol.** `decoder.data` items gain an optional field for the positions. It is additive.
* **Saved setup.** None if `data_bits` of 9 is used. One optional key if a flag or a bus bit count is added.

### Driver

* **Transmit.** The driver tags each write request with the parity in force when it was queued, so the hub learns
  that the bytes of one write have a ninth bit of 1 (mark), 0 (space), or the computed parity (odd and even, which
  the studio can compute). The hub handle needs a way to read it, such as a new read that returns runs, with a
  version bump of `public.h`. Nothing is deployed beyond the owner's machines, so no compatibility code is needed.
* **Receive.** No change. The existing line error does it (see above), with one request for each address byte.
* **Mark and space is not 9-bit.** A program that really wants mark parity and does not think of it as a ninth bit
  gets the same bytes it always did. The 9-bit mode is a port setting, so it is a choice and not a guess.

### Hot path

The check is a pointer test for each chunk, and a copy of the pointer where a chunk is passed on. A chunk that has
address bytes allocates once, when it is read, not for each byte. Fan-out to monitors shares the same pointer as it
shares the bytes. The checks that matter are the throughput tests with 9-bit traffic off (no change expected) and a
benchmark with a worst case of every byte marked.

## Multi-drop addressing at the protocol level

A 9-bit bus has a master and slaves, and a slave answers only when its address matches. At the engine level this is
three things:

* **Address filtering.** A slave ignores a message not for it. A simulated slave (a fixture, or a response rule) needs
  to match the address byte and then take the bytes up to the next address byte.
* **Master and slave roles.** A master sends the address and waits. A slave answers. A fixture that plays a slave has
  a role, and the engine already has roles in the rule model (`peer`, `station`, `inline`).
* **Turnaround and collision on half-duplex RS-485.** Only one end drives the wire at a time. The existing half-duplex
  bus already counts a chunk that starts while another is on the wire (a collision), and one that starts inside
  `turnaround_ms` (too soon). Both already apply to a 9-bit bus once the bit count is right. MDB has its own timing
  (a slave must answer within a set time, and the master holds off while a slave is), and that belongs in a rule set
  using the [response timing](response-timing.md) rules, not in the bus.

## What goes where

| Work | Community engine | Pro plugin |
|---|---|---|
| Carrying the ninth bit through ports, links, the bus, events and recordings | Yes: it is data path and must not wait for a plugin | |
| The 9-bit port setting, the monitor marker, the capability flag | Yes | |
| The bit-time of a 9-bit frame on a half-duplex bus | Yes | |
| Address-aware decode: names a message by its address byte, shows an MDB or other protocol view | | Yes, a decoder (`kind: decoder`) |
| Address filter on a link: pass only messages for chosen addresses | Rule match on "address byte" (engine, so no round trip) | The plugin builds the rule set and shows it |
| Routing between ports by address (one master to several slaves, each on its own port) | A rule or modifier action that selects a target | The plugin holds the table and the user interface |
| A simulated slave that answers by address | Rules and a fixture | A fixture type for a named protocol, such as MDB |

The engine keeps the carrying free because without it nobody can monitor or link a 9-bit port at all, and because a
plugin cannot sit on a per-byte path at microsecond timing (see [the plugin framework](plugins.md), section 6). The
Pro part is what an owner of an MDB or address-bus rig pays for: protocol decoders, address routing and ready-made
slaves. This follows [Editions](editions.md): Pro adds plugins and the engine stays the same.

## Recommendation

Do option 1, in the community engine, with the port setting as `data_bits = 9`. Keep option 2 as a decoder in the Pro
plugin for reading recordings of programs that toggle parity, with its guesses labelled as guesses. Do not do option 3.

Reasons: option 1 is the only one that is exact and that works with the links, the bus and the monitor that already
exist. Its cost on traffic that is not 9-bit is a pointer test for each chunk. The one place it needs a driver change is
the transmit side, and the receive side already works.

## Phased plan

| Phase | What | Rough effort |
|---|---|---|
| 1 | Chunk gains the optional ninth-bit field; events, traffic log, delay line and `read_stage` carry it. `data_bits = 9` accepted in `validate`; a link to a non-9-bit port drops and counts. Unit tests, and a benchmark of the worst case | 1 to 1.5 weeks |
| 2 | Half-duplex bus charges the right number of bits; garble covers the bit. Monitor marker; recording field; the decoder protocol field | 1 week |
| 3 | Driver: per-write parity tag and the hub read that returns it. Receive side through the line error. Guest test with a program that toggles mark and space | 1.5 to 2 weeks, with guest runs |
| 4 | Real ports by mark and space toggling, with a drain between bytes, on Windows and POSIX. Slow, and documented as slow | 1 week |
| 5 | Rule match on "address byte". Pro plugin: address-aware decoder, address filter, address routing, and an MDB fixture | 3 to 4 weeks, in the plugin repository |

The phases 1 and 2 stand alone and give a usable monitor and link for a 9-bit port that is fed by a file, a network
port or a fixture. Phase 3 is what makes a Windows program that toggles mark and space work.

## Open questions for Iain

1. Is `data_bits = 9` as the port setting the right way, or should it be a separate flag so that an 8-bit port set to
   mark parity never changes meaning?
2. Should an RFC 2217 or raw TCP port ever carry the ninth bit, by a private two-bytes-a-symbol framing between two
   Studios? If not, drop and count is the whole answer.
3. Which bus does a first customer need: MDB (9600 baud and its timing), an 8051-style multiprocessor bus, or an
   RS-485 bus of their own? It decides what the first fixture is.
4. Is a slow real-port mode (a drain after each byte) worth shipping, or should real ports with a ninth bit wait for
   the vendor's own adapters?
5. Do the findings of the baud, break and parity audit change how parity reaches the hub? Phase 1 waits for them.
6. Should "address byte" matching be in the rule model for everyone (Community), with only the decoders and routing
   in Pro?
