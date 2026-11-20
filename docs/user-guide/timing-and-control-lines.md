# Timing and control lines

## Delay and jitter on a link

A real wire is not instant, and a network hop adds more. A link can hold what it
carries for a set time, so a program that waits for a reply for a fixed window
can be tried with a reply that comes early, late or just on the edge. In a
topology file, add `delay_ms` (0 to 60000) to any link, and `jitter_ms` to vary
each chunk by up to that much either side of the delay. In the window at the
Advanced level, right-click the link and choose **Settings...**: **Delay** and
**Jitter** are the same two numbers.
Applying changes the link where it is: it keeps its ports, its owner and its
counters. Bytes it still holds for its delay are discarded when the delay
changes, and a new delay applies to what follows. A monitor link has no
settings.

```json
{ "mode": "bus", "members": ["master", "device-1", "device-2"],
  "delay_ms": 190, "jitter_ms": 10 }
```

Here every byte takes between 180 and 200 ms to reach each other member.

- **Jitter cannot exceed the delay**, so a byte is never due before it was sent.
  Studio refuses the file otherwise, and with `jitter_ms` alone.
- **Order is kept.** A chunk never overtakes the one before it on the same
  route, however the delays fall, so the extra wait may pile up behind a slow
  chunk but the stream is never shuffled.
- **Both ways.** On a duplex link each direction waits the full delay, so a
  round trip takes twice `delay_ms`. On a bus the delay is the time from a
  member sending to each other member hearing it.
- **Only the link is slowed.** The ports at either end, and any other link
  they are on, are not.
- **Removing the link drops what is still on the way.**
- The delay is a timer in Studio, not a model of baud rate: it adds to the time
  the ports themselves take. It is accurate to a few milliseconds, and a busy
  machine can make a byte late but never early.

## Wire timing on a link

Without it, a link hands on what it reads as soon as it can: ten bytes written
at once arrive at once, though a real wire would take a few milliseconds for
them. Some protocols depend on that time. Modbus RTU ends a frame with 3.5
characters of silence, DMX512 needs a gap after a break, and LIN a space before
the answer. Turn on **Wire timing** in the link's **Settings...** (right-click
the link at the Advanced level), or add `wire_timing` to the link in a topology
file:

```json
{ "mode": "duplex", "sources": ["master"], "targets": ["device"],
  "wire_timing": { "bit_rate": 115200, "framing": "8N1" } }
```

- **Each character takes its real bits**: a start bit, 5 to 8 data bits, a
  parity bit if there is one, and 1, 1.5 or 2 stop bits. At 9600 bit/s a byte of
  8N1 takes about 1.04 ms and a byte of 8E2 about 1.35 ms.
- **A long write is spread over the time it would take**, in short pieces. The
  next write waits until the one before it has finished. A break holds back what
  follows it for the length of the break (one character when its length is not
  known).
- **Whose rate**: the sending port's. If a program has set a rate on the port
  (a Windows driver port), that is used, then the port's own setting for a real
  serial port, then the **Bit rate** and **Framing** on the link. With none of
  them the link is not paced, and the Log says "no rate for this link" once.
  Leave **Bit rate** at "From the port" to use the first two.
- It is off unless you turn it on, it is not saved when off, and a bus does not
  have it because a half-duplex bus already has a wire of its own.
- A Studio from before wire timing will not open a file that has it.
- **Accuracy**: on Windows 10 (version 1803) and later a pace is kept to about
  half a millisecond; elsewhere it is a little better than that, and for a gap
  under a millisecond Studio waits on the processor for the last part of it. That
  uses at most a tenth of one processor core, and past that the release is made
  from the timer and may be late. The link counts how many releases were late.
  Over RFC 2217 the gaps a client sees are not reliable, because TCP joins and
  splits what it sends as it likes.

## RS-232, RS-422 and RS-485

A Studio port has no electrical mode. A program cannot see the wiring, so a
label of RS-232, RS-422 or RS-485 on a port would change nothing the program
can notice, and would promise behaviour Studio does not have. What differs
between the standards, for a program, is how many stations share the wire and who
hears whom, and links model that:

| Wiring | In Studio | Who hears whom |
|---|---|---|
| RS-232, one device | A **two-way** link (a null-modem cable) | Each end hears the other. |
| RS-422, or RS-485 with four wires: a master transmits to many devices, and the devices answer on a pair that only the master hears | A **split** from the master's port to the device ports, with modem lines off | A device hears the master. The master hears every device. The devices do not hear each other. |
| RS-485 with two wires: every station on one pair | A **shared bus**, as a half-duplex line if you want collisions reported | Every station hears every other, and none hears itself. |

Make the four-wire case with **Topology → Connect ports…** and **Split one port
to many**, or `{ "mode": "split", "sources": ["master"], "targets": ["device-1",
"device-2"], "emulate_modem_lines": false }`. A split already carries the
devices' answers back to the master. Do not add a one-way join from the devices
back to the master's port to "complete" it: that makes a loop, and Studio refuses
it ("That link would create a loop" in the window, "link would create a cycle"
from `vss_cli`). To listen without taking part, give a
listener a **monitor** or a **join** from the ports, as in [One master, several
programs and a silent listener](sharing-a-device.md#one-master-several-programs-and-a-silent-listener).

### RS-485 is half-duplex, and a bus can be

On a real two-wire RS-485 line one station talks at a time. A station switches its
transmitter on, sends, and switches it off, either in hardware (automatic
direction) or by the program raising and dropping RTS. A shared bus gives the same
hearing, and a master that waits for the answer to each poll works as it would on
the wire. Made the plain way it does not enforce the one-at-a-time rule: two
programs that send at once are both heard, whole, in the order their bytes
arrived.

A bus can be made a **half-duplex line** instead. It then has a line rate, and
Studio tells you when two stations were on the line together or one answered too
soon. Tick **Half-duplex line** on the **Shared bus** page of **Topology →
Connect ports…** (Advanced), or in **Settings for** the bus, or write it in the
topology file:

```json
{ "mode": "bus", "members": ["master", "device-1", "device-2"],
  "half_duplex": { "bit_rate": 9600, "turnaround_ms": 2,
                   "collisions": "garble", "rts_gating": false } }
```

Every member of `half_duplex` may be left out; `"half_duplex": {}` is a line at
9600 bit/s that reports and changes nothing. A bus without `half_duplex` is the
plain bus, as before, and Studio writes nothing for it.

- **The line rate** (`bit_rate`, 50 to 12000000 bit/s; **Line rate**). A byte
  takes ten bits, as at 8N1. A message of `n` bytes is on the line for
  `10 × n ÷ bit_rate` seconds from the moment its program sent it, and **the
  other stations hear it when its last byte has left the line**, not before: 8
  bytes at 9600 bit/s arrive 8.3 ms after they were sent. If a program sends two
  messages in a row, the second follows the first. Set the rate your equipment
  uses. It is the bus's own rate and does not read or change the baud rate of any
  port.
- **Overlap is reported.** A message that starts while another station's is still
  on the line has collided with it. The Monitor and the Log show a line such as
  "device-2 sent 6 bytes while device-1 was sending", and the bus counts it
  (**collisions**).
- **What the stations hear of a collision** (`collisions`; **If two send at
  once**). With `report` both messages arrive as they were sent, so your programs
  carry on and you read what happened in the log. With `garble` every byte of
  both messages arrives inverted (`41` arrives as `BE`), as a receiver on a real
  line reads rubbish; a checksum fails, and a master that retries does. The bus
  counts the bytes it spoilt (**garbled**), and a capture still shows what was
  sent. Studio spoils the whole of each message it was handed, not only the part
  that overlapped, because it does not know where a frame ends. A collision never
  loses a byte.
- **Turn-around** (`turnaround_ms`, 0 to 60000; **Turn-around**). The line stays
  taken for this long after a station's last byte, while its transmitter lets go.
  Another station that starts inside that time has answered too soon: it is
  reported ("device-1 sent 5 bytes inside the turn-around time after master")
  and counted as a collision, and with `garble` the hasty answer alone is
  spoilt. The turn-around holds nothing back. To make a device answer *later*,
  use a delay in a [scripted device](command-line.md#scripted-devices-with-vss_cli-respond); a
  delay on the bus itself (`delay_ms`) is added after the line, to every station
  alike.
- **RTS** (`rts_gating`; **Send only while RTS is high**). A station sends only
  while its RTS is high, and holds the line for as long as it is: what it sends
  with RTS low is not carried (reported, and counted as **dropped**), and anyone
  else who sends while it holds RTS high has collided with it. **In this build
  this applies to loopback ports only**, where you set RTS yourself in the
  Inspector. Every other kind of port always sends, as a station with an adapter
  that switches by itself does, and is never muted. The reason is in the next
  list.
- **Too far ahead.** A program that writes faster than the line for a long time
  would fill the memory. A message that could not start within a minute of being
  sent is dropped, reported and counted as **dropped**.

What a half-duplex line still does not do:

- **It does not follow a program's RTS.** A program on a port raises RTS, writes
  and drops RTS within microseconds, and Studio does not learn of the RTS in step
  with the bytes: a Windows driver port reports it only when asked, a pseudo
  terminal has no modem lines, and the RTS of a real serial port is Studio's own
  output to the device. Gating by an RTS that was read too late would mute a
  program that did everything right, so Studio does not. For the same reason RTS
  dropped before the last byte has gone, which cuts a frame short on real
  hardware, is not noticed.
- **No echo.** A port never hears its own bytes. A transceiver that echoes what
  its station sends, which some adapters do, is not modelled, so a program that
  expects to read its own transmission back will not.
- **No electrical mode.** A port still has no RS-232, RS-422 or RS-485 label.
  The line belongs to the bus.
- **Real devices bring their own time.** Bytes from a real serial port or from
  another computer have already spent their time on a real wire or a network
  when Studio reads them, and the line rate is added on top. Two real devices
  that overlapped before the bytes reached Studio are not seen as a collision.
- **Timing is the PC's.** Nothing arrives before its time on the line is over; a
  busy machine can make it later.

## Control lines

Select a port to see its control lines in the **Inspector**. **DTR** and **RTS**
are switches, **Break** is a button, and CTS, DSR, DCD and RI are chips ("CTS on"
or "CTS off"). A port that cannot drive lines says so. Virtual ports accept the
operations and report their own state; they have no physical wires. The
Inspector is an Advanced pane.
