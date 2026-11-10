# Scenario recipes

Each recipe can be built in the GUI, scripted with `vss_cli`, or written as a
JSON topology that both accept. The URIs are examples. `vss_cli` keeps most of
these setups alive only for the `--seconds` you give, as the examples do; see
[Command line equivalents](user-guide.md#command-line-equivalents).

## Share one serial port with several applications

The device is opened once; every listener gets its own virtual port.

```bash
vss_cli share serial://COM3 --listeners=3 --seconds=60
```

```
listener-1  COM5      <- open this in PuTTY
listener-2  loopback:listener-2
listener-3  loopback:listener-3
```

On Windows with the Virtual Serial Studio driver, every listener is a driver
port with its own COM number. With only com0com, a listener takes the free end
of a com0com pair; once the free pairs are used, further listeners fall back to
loopback, because a null-modem pair can only be wired into the hub once.
On Linux and macOS, listeners are pseudo terminals. In the window, **Share a
device** does the same and also lets you add or remove a program later.

In JSON:

```json
{
  "name": "share",
  "ports": [
    { "name": "device", "kind": "physical", "uri": "serial://COM3?baud=115200" },
    { "name": "app",    "kind": "virtual", "virtual_kind": "auto" }
  ],
  "links": [
    { "mode": "split", "sources": ["device"], "targets": ["app"] }
  ]
}
```

## Bridge a device to the network

```bash
vss_cli bridge serial://COM3 tcp://192.168.1.50:4001 --seconds=60
```

The same command accepts `udp://host:port` and `rfc2217://host:7001`, and the
listening forms `tcp-listen://`, `udp-listen://` and `rfc2217-listen://` that
wait for other computers (see [Network
addresses](user-guide.md#network-addresses)). The bridge is a duplex link, so
data from the network reaches the device and vice versa. Both directions are
visible in the monitor. In the window the same job is **Ports → Network
endpoint** (Advanced), or **Let other computers use my device** at Simple.

## Merge several publishers into one device

```bash
vss_cli join serial://COM3 serial://COM5 --target=tcp://10.0.0.2:4001 --seconds=60
```

By default the publishers interleave. To hand the link to one publisher at a
time, use an `owner` policy:

```json
{ "mode": "join", "sources": ["a", "b"], "targets": ["merged"],
  "policy": "owner" }
```

The hub has `set_owner` and `release_owner`, and the window's owner bar
(Advanced) uses them; while one publisher owns the link the other publishers'
bytes are counted as dropped, not silently interleaved. The owner is one of the
publishers on a join. On a duplex link, a `connect` or a `split`, the owner is one
of the targets: only its bytes reach the source and the others' are counted as
dropped, and what the source sends reaches every target. At Simple, **Let
programs take turns sending to a device** builds that split, so the device's
replies reach every program.

## Put several programs on one bus

A polled protocol such as Modbus RTU runs on a single line with one master and
several devices, and every station hears every byte. A shared bus gives each
program its own port and joins them like that line: what one writes goes to all
the others and never back to itself.

```bash
vss_cli bus --virtual=3 --tap=virtual --seconds=60
```

```
member-1 -> /dev/pts/4          <- open this in the master
member-2 -> /dev/pts/5          <- and these in the devices
member-3 -> /dev/pts/6
tap -> /dev/pts/7               <- a listener that cannot transmit
bus of 3 members (link 1)
```

Add `<uri>` arguments to put real or network ports on the bus as well, for
example `vss_cli bus serial://COM3 tcp://10.0.0.2:4001 --virtual=1`.

In JSON the bus lists its members, and the listener is a join from them that
carries no modem lines:

```json
{
  "name": "bus",
  "ports": [
    { "name": "master",   "kind": "virtual" },
    { "name": "device-1", "kind": "virtual" },
    { "name": "device-2", "kind": "virtual" },
    { "name": "listener", "kind": "virtual" }
  ],
  "links": [
    { "mode": "bus", "members": ["master", "device-1", "device-2"] },
    { "mode": "join", "sources": ["master", "device-1", "device-2"],
      "targets": ["listener"], "emulate_modem_lines": false }
  ]
}
```

A bus has no modem lines, no owner policy, and a port can be on one bus only.
A route out of the bus and back into it is refused as a loop. The listener is
outside the bus on purpose: a port that is not a member cannot send onto it.

## Put a device from another computer on the bus

A device program on another computer can be one more station on the bus. Studio
waits for it as an RFC 2217 server, and the bus treats that port like any other.

```json
{
  "version": 1,
  "name": "remote-bus",
  "ports": [
    { "name": "master",   "kind": "virtual", "virtual_kind": "auto" },
    { "name": "device-1", "kind": "virtual", "virtual_kind": "auto" },
    { "name": "remote",   "kind": "rfc2217",
      "uri": "rfc2217-listen://127.0.0.1:7001" }
  ],
  "links": [
    { "mode": "bus", "members": ["master", "device-1", "remote"] }
  ]
}
```

It listens on `127.0.0.1`, so nothing but this computer can reach it. On the
other computer, forward a port to it with SSH and open the forwarded port with
any RFC 2217 client:

```bash
ssh -N -L 7001:127.0.0.1:7001 user@studio-computer
python -m serial.tools.miniterm rfc2217://127.0.0.1:7001 9600
```

With a VPN instead, listen on the address the VPN gives this computer and add the
VPN's range: `rfc2217-listen://10.8.0.1:7001?allow=10.8.0.0/24`. There is no
password and nothing is encrypted, so do not listen on `0.0.0.0` on a network you
do not control; [Keeping it safe](user-guide.md#keeping-it-safe) lists the
choices. When the other computer is not connected, the member is silent, like a
device that is unplugged.

The master's reply window includes the network. Add `delay_ms` to the link to
rehearse a slow hop, and `capture` the master's port to read the time of each poll
and its answer; they are stamped by Studio's clock, not the other computer's.

## Model RS-422 and RS-485

A port has no electrical mode, but the wiring decides who hears whom, and links
model that. Four-wire RS-422, or four-wire RS-485 multi-drop, is a split: the
master reaches every device, the devices answer to the master alone and do not
hear each other.

```json
{ "mode": "split", "sources": ["master"], "targets": ["device-1", "device-2"],
  "emulate_modem_lines": false }
```

Two-wire RS-485 is a bus, where every station hears every other (see above). A
bus does not enforce one station at a time, report overlap or carry RTS; see
[RS-232, RS-422 and RS-485](user-guide.md#rs-232-rs-422-and-rs-485).

Do not add a one-way join from the devices back to the master after a split: that
is a loop and is refused. The split already carries the answers back.

## Run an old program against a fixed COM port

Many old programs open one COM number kept in a setting file. Make the port with
that number before the program starts:

```bash
vss_cli virtual --kind=windows-driver --com=3
```

or put `"com_number": 3` on a virtual port in the topology. Names above `COM9`
fail in programs that build the name as `"COM%d"`, so keep to 1 to 9 where you can.
[Old programs on Windows](user-guide.md#old-programs-on-windows) covers 32-bit
programs and programs that try every port.

## Watch traffic without disturbing the device

`monitor` links are one-way and do not emulate modem lines, so the observer
cannot change what the device sees. **Topology → Watch traffic** makes one.

```bash
vss_cli loopback --seconds=30   # two in-process ports and a one-way redirect, no hardware
```

The GUI monitor shows both directions with ASCII or hex previews. The Send panel
writes into any open port.

## One master, several programs and a silent listener

One program polls and several others answer, as on a shared serial line, and
you want a record of the whole conversation with the exact time of every
message. Each program opens its own port. Studio joins the ports and records
them; nothing needs to be added to any of the programs.

```json
{
  "name": "poll-lab",
  "ports": [
    { "name": "master",   "kind": "virtual", "virtual_kind": "auto" },
    { "name": "device-1", "kind": "virtual", "virtual_kind": "auto" },
    { "name": "device-2", "kind": "virtual", "virtual_kind": "auto" },
    { "name": "listener", "kind": "virtual", "virtual_kind": "auto" }
  ],
  "links": [
    { "mode": "split",   "sources": ["master"],
      "targets": ["device-1", "device-2"] },
    { "mode": "monitor", "sources": ["master"], "targets": ["listener"] }
  ]
}
```

The master's port is the line as the master sees it: it receives what the
master sends and is written everything the devices answer. The `monitor` link
gives a fourth program a read-only copy of what the master sends; that program
cannot send anything back. Start the programs on the ports `vss_cli` prints,
then record the master's port:

```bash
vss_cli capture poll-lab.json --out poll.cap --port master --seconds=120
```

`poll.cap` has a line for each message in either direction:

```
[      1204] R 0x02|0x31|0x03
[      1251] W 0x02|0x31|0x41|0x03
```

`R` is a poll the master sent and `W` is an answer written to it, 47 ms
later. For the whole picture with the names of the ports, record every port to
JSON Lines and read it with any tool:

```bash
vss_cli capture poll-lab.json --out poll.jsonl
```

One message shows up once for each port it crossed: `R` on the sender and `W`
on each receiver. Filter on `port` and `direction` to pick the rows you want.
The `.cap` file has no port column, so keep one port per file with `--port`.

### Play the master again without the master

The `R` records of the master's port are what the master sent, with the gaps
between them. Play them into the port the master program had opened, and the
devices see the same polls at the same times:

```bash
vss_cli play poll.cap --uri=serial://COM6 --seconds=60
vss_cli play poll.cap --uri=serial://COM6 --speed=4 --loop    # faster, until Ctrl-C
```

Record the replay with `capture` as before, and the two captures can be compared
line by line: the same bytes in the same order, with the same gaps to within a
few milliseconds.

### Answer the master without the device programs

A device that must answer inside a window, say 200 ms, can be scripted. This
`device-1.json` answers a poll for address `31` after 150 ms, which leaves 50 ms
of margin, and one for `32` after 60 to 120 ms:

```json
{
  "version": 1,
  "seed": 1,
  "rules": [
    { "name": "poll 31", "match": "02 31", "reply": "02 31 41 03",
      "delay_ms": 150 },
    { "name": "poll 32", "match": "02 32", "reply": "02 32 42 03",
      "delay_ms": 90, "jitter_ms": 30 }
  ]
}
```

```bash
vss_cli respond device-1.json --uri=serial://COM7
```

Run one `respond` for each device port, play the recorded master into its port
with `vss_cli play`, and `capture` the master's port. The `W` lines are the
answers, and the gap from a poll to its answer is the time the device took. A
script with a `poll` list can stand in for the master instead, so the whole bus
can run with no program on it.

## Test software that watches the control lines

1. Attach a device (or create a pair of virtual ports).
2. Link them duplex with modem emulation on.
3. Toggle DTR in the inspector.
4. The peer's DSR and DCD follow, which is exactly what a null-modem cable does.

From code:

```cpp
studio app;
const auto a = app.create_virtual_port("a", vss::virtual_port_kind::loopback);
const auto b = app.create_virtual_port("b", vss::virtual_port_kind::loopback);
app.connect("a", "b");

(*a)->set_line(vss::control_line::dtr, true);
// b's DSR is now asserted
```

Note that two loopback devices are echo devices; use a one-way link (`monitor`
or `redirect`) when the point is observation rather than full duplex.

## Measure a device end to end

```bash
vss_cli bench --from=serial://COM3 --to=serial://COM5 --bytes=10MB --chunk=4KB
```

`sender` and `receiver` are deliberately not linked: the bytes travel through
the cable, the receiver counts what actually arrived, and the command fails if
the count is short. It prints the bytes sent and received, the time taken and the
throughput. **Tools → Benchmark** (Advanced) runs the same measurement.

## Reproduce a failure

The serial library's mock backend is reachable as an endpoint:

```cpp
studio app;
const auto device = app.create_mock_port("modem");
```

The fixture can inject parity errors, framing errors, overruns, breaks,
hang-ups and disconnects, and the hub reports them as endpoint error events.
That makes cable-pull and noisy-line behaviour reproducible without hardware. In
the window, add a fixture with **Ports → Mock fixture** and use **Open line
matrix and faults** in the Inspector.

## Save and replay

**File → Save** writes whatever topology the window is running. `vss_cli run
lab.json` applies it again, so a support case, a demo or a benchmark can be
reproduced with a file:

```bash
vss_cli run lab.json --seconds=30 --verbose
```
