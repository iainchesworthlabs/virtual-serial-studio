# Sharing a device

## Sharing one device with several programs

**Topology → Share a device** opens a wizard with three steps, **Device**,
**Programs** and **Ready**.

1. **Which device?** Choose the device to share. Its settings are summarised
   under **Device settings**, with **Change**.
2. **How many programs?** (1 to 32). The table lists the port each program will
   open and whether other programs can open it. In step 1 you can change the
   device's settings or use **A network device instead**. On Windows with the
   Virtual Serial Studio driver, Studio makes a port for each program when you
   share, so no free pairs are counted; the wizard warns only when you ask for
   more ports than one user can hold (16), for example "Only 16 of the 17
   programs can open their port". With only com0com, each outside program
   needs its own com0com pair. When there are not enough free pairs the wizard
   warns, for example "Only 2 of the 3 programs can open their port", and
   offers **Create another pair** (administrator permission) or **Keep the
   third as Studio only**. On Linux and macOS every port is a pseudo terminal.
3. **Share device** builds the ports and the split link, and ends on a summary
   of what each program should open, with **Copy**, **Open monitor** and **Done**. If any step fails, the wizard removes what
   it created. **Add another program** on the summary adds one more port
   (to 32). Studio makes the port and adds it to the link that exists. The link
   keeps its identity and the other programs keep their ports; no bytes are lost
   for them.

Everything the device receives is copied to all of the ports. Whatever a
program writes goes back to the device. To add a port to a share later, select
any port or link of it and choose **Topology → Add a program port**. In
Advanced, **Share a device** is also on the toolbar.

The three steps of the wizard, on Windows. The second picture shows the case
where no driver pair is free, so the ports work only inside Studio.

<picture><source srcset="../../images/share-device-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/share-device-light.png" width="760" height="700" alt="Share step 1: choose the device, COM3, and review its settings"></picture>

<picture><source srcset="../../images/share-programs-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/share-programs-light.png" width="760" height="760" alt="Share step 2: three programs, the port each will open, and a warning offering Create another pair"></picture>

<picture><source srcset="../../images/share-ready-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/share-ready-light.png" width="760" height="640" alt="Share step 3: terminal on COM7, test-tool on COM8 and logger as Studio only"></picture>

## One master, several programs and a silent listener

Some serial protocols put one master and several devices on a single two-wire
line, the way Modbus RTU does over RS-485: the master asks, the addressed device
answers, and every station hears every byte. A shared bus gives each program its
own port and makes the ports behave like that one line, so you can try a master
against several device programs, and watch the line, without hardware.

1. **Add a port** for each program: the master, device 1 and device 2.
2. **Topology → Connect ports…**, choose **Shared bus**, tick the three ports
   and choose **Connect**. The sentence reads "Each of master, device 1 and
   device 2 hears what the others write, and none hears itself."
3. Open one port in each program, as the dialog that opens when a port is made (titled, for example, **COM7 is ready**) shows.
4. To listen without being on the line, add one more port, called *listener*,
   and choose **Join many ports into one**. Tick the three bus ports as the
   ports that send and *listener* as the target. Open an analyser on *listener*.
   It hears what each of the three writes. Nothing it writes reaches the bus,
   because a join is one way and *listener* is not on the bus.

The same setup as a topology file, for `vss_cli run` or **File → Open**:

```json
{
  "version": 1,
  "name": "polled-bus",
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

A topology file must have `"version": 1`, and a member Studio does not know
is an error, so a misspelt name is found when the file is opened.

From the command line, `vss_cli bus --virtual=3 --tap=virtual` makes the three
bus ports and the listener and prints the port each program should open.

What to expect:

- **No echo.** A port never hears its own bytes, so a program that reads back
  what it sent, as an RS-485 transceiver in a loop would, sees nothing of its
  own. Bytes from different ports are mixed in the order they arrive, and
  nothing arbitrates when two programs talk at once; they are interleaved, not
  corrupted. A master that polls and waits for the answer is not affected.
- **No modem lines.** A program that toggles RTS to switch its transceiver
  still runs; the line goes nowhere. Do not rely on a bus to carry DTR or RTS.
- **One bus per port**, and no route may run from the bus out to another port
  and back in to it. Studio refuses that with "That link would create a loop".
- **Ports on other computers.** A TCP, UDP or RFC 2217 port can be a member too,
  so a device on another machine can sit on the same bus.

## Who is on the other side of a port

Next to a port's name the list and the inspector can say what Studio has learned
about the program or the computer behind it:

- **The speed a program set.** "Program set 19200 7E2" is the baud rate, data
  bits, parity and stop bits the program chose with `SetCommState` (Windows
  driver ports) or `tcsetattr` (pseudo terminals). Studio does not guess: a port
  nobody has configured, a TCP or UDP pipe and a mock fixture say nothing. On a
  Linux pseudo terminal the kernel forces the slave to 8N1 whatever a program
  asks for, so only the baud rate is known and the row says "Program set 19200"
  with no framing; macOS keeps the framing. Studio asks the system which it is.
  A program that sets exactly 38400 8N1 on a pseudo terminal cannot be told from
  one that set nothing, and the last setting stays after the program closes the
  port.
- **Clients of a listener.** A `tcp-listen`, `udp-listen` or `rfc2217-listen`
  port says "Clients connected: 2", or "No remote connected" when nobody has
  joined. A device that has dropped off a bus is silent, as an unplugged one
  would be, and this is where it shows.
- **A speed mismatch.** If the ports of one link do not run at the same baud
  rate and framing, the port is marked "Differs from *name* in baud rate or
  framing" in the warning colour, and `vss_cli run` prints
  `warning: link 3: members run at different baud rates or framing: ...`. A real
  serial port and an RFC 2217 client count with the speed they were opened at.
  Studio still carries the bytes across a mismatch, so a program set to 19200
  on a 9600 line works in Studio and would fail on a real wire: this is the
  warning that tells you. A port whose speed is not known is left out of the
  comparison, so not knowing is never a warning.

`vss_cli run` also prints "program set", "connected" and "last data N s ago"
under each port. The last activity is the time a port last moved a byte, which
the engine already keeps for each port (`endpoint_info::stats.last_activity`);
the window does not show it.
