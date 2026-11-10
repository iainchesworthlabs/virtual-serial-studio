# Using Virtual Serial Studio

This guide is for people using the desktop application. It covers the two
levels of the window, the everyday jobs, the expert tools, and what to check
when something does not work.

The application is a window over the `vss::studio` engine: it creates ports,
wires them together, and shows what flows through them. Most of it can also be
scripted with `vss_cli`; the command equivalents are listed near the end.

The pictures were taken from Virtual Serial Studio running on Windows 11 and
Ubuntu 26.04 test machines, in the light theme unless the text says
otherwise. The dialogs look as pictured. The pictures of the whole window show
an earlier layout: now the toolbar sits above the diagram, with no New, Open
or Save buttons, and the **Simple | Advanced** switch is at the right of the
menu bar. Port names such as `COM7` and `/dev/pts/1` are examples; yours will
differ.

## Simple and Advanced

The window has a **Simple** level and an **Advanced** level. Simple is the
default and is built for one job: "I need a serial port, now." It is a list of
your setups and a page of jobs. Advanced is a diagram you build on, with the
panes an integrator or a diagnostician needs. Both show the same studio.
Nothing you have built changes when you switch, and a setup made at one level
opens at the other.

| | Simple | Advanced |
|---|---|---|
| Main area | **Your setups** (a list) and a page: the jobs, a job form, or the page of one setup | The topology diagram with a toolbox, and the Ports, Inspector, Send and Monitor panes |
| Add a virtual port, a linked pair or a real device | Jobs on the job grid | The launcher, the **Ports** menu and the toolbox |
| Connect ports, share a device, watch traffic | Jobs (and the Topology menu) | The **Topology** menu, the toolbox, and the diagram itself |
| Send text and see traffic | **Type to** and **Data going through** on a setup's page | The Send panel and the Monitor |
| Control lines, Break, line wiring, faults, owner, benchmark, Export as command, Log, Topology JSON | Hidden | Shown |
| Network endpoint and mock fixture | Hidden | Shown |
| Dialogs | Guided, with defaults | Editable addresses and More options |

Switch with **View → Advanced mode** (`Ctrl+Shift+A`), the **Simple | Advanced**
switch at the right end of the menu bar, **Settings**, or the **Turn on Advanced
mode** and **Advanced mode** links in the launcher and the dialogs. The choice is
remembered and applies to every document. Commands hidden in Simple still appear
in the command palette, under "Needs Advanced mode", with a button that turns it
on.

## The window at a glance

### Simple

```
┌──────────────────────────────────────────────────────────────────────┐
│ File Edit Ports Topology Tools View Help  [Search] [Simple|Advanced] │
├───────────────────┬──────────────────────────────────────────────────┤
│ Your setups       │ What do you want to do?                          │
│ COM7 shared (2)   │  My program needs a serial port                  │
│   COM8 (PuTTY)    │  I have a serial device                          │
│                   │  Several programs or devices at once             │
│ Save... Open...   │  Look and check                                  │
│       Remove all  │                                                  │
├───────────────────┴──────────────────────────────────────────────────┤
│ Ports 2  Links 1  RX  TX  Hub running                    Setup ready │
└──────────────────────────────────────────────────────────────────────┘
```

- **Your setups** (left): one row per setup, that is, the ports that were built
  together and the links between them, with the ports it holds underneath. A
  setup made at the Advanced level that no job describes carries the title "Made
  in Advanced mode". **New** goes back to the jobs. **Save…**, **Open…** and
  **Remove all** are below the list.
- **The page** (right) is one of three things: the job grid, a job form, or the
  page of the setup you selected (see [Jobs](#jobs-simple)).
- The status bar shows the port and link counts, the receive and transmit rates,
  the hub state and a readiness chip that opens the setup check.

### Advanced

```
┌──────────────────────────────────────────────────────────────────────┐
│ File Edit Ports Topology Tools View Help  [Search] [Simple|Advanced] │
├───────────────┬──────────────────────────────────────┬───────────────┤
│ Ports         │ Attach Connect|Mock Share Fit        │ Inspector     │
│ port  state   │ +----+ [port A]---[port B]           │ state, path   │
│ RX TX Dropped │ |Tool|                               │ traffic, lines│
│               │ |box |                               │               │
│               │ +----+  owner bar                    │ Send          │
│               │                                      │               │
├───────────────┴──────────────────────────────────────┴───────────────┤
│ Monitor | Log | Topology JSON                                        │
├──────────────────────────────────────────────────────────────────────┤
│ Ports 2  Links 1  RX  TX  Hub running                    Setup ready │
└──────────────────────────────────────────────────────────────────────┘
```

- **Ports** (left): one row per port with its name, what it is for and its
  path (with a copy button), then its state, RX, TX and Dropped. **Filter
  ports** narrows the list. The path is what your other program opens. Select a
  port here, in the topology or in the monitor, and the other two follow.
- **Topology** (centre): one box per port and one line per link. Click a box to
  inspect it and a line to select the link. The row of buttons above it holds
  **Attach real device**, **Connect ports**, **Mock fixture**, **Share a
  device**, **Fit**, **Remove port** and **Remove setup**. The toolbox on its
  left builds on the diagram; see [Building on the diagram](#building-on-the-diagram-advanced).
  **View → Show links as a list** gives the same links as a list that a screen
  reader can walk.
- **Inspector** (right): details for the selected port and its control lines.
  A port that nothing has opened yet links to "Why is nothing arriving?".
- **Send** (right): write bytes into an open port.
- **Monitor** (bottom): traffic rows with time, port, direction, bytes and a
  preview. Its two tabs, **Log** and **Topology JSON**, are shown from the
  **View** menu. When the JSON has an error, the editor names the line, marks
  it, and **Go to line** jumps to it. That covers syntax mistakes, bad names,
  kinds, modes and links, and invalid serial settings.
- **Status bar**: port and link counts, receive and transmit rates, hub state,
  a chip that counts bytes dropped when there are any, a readiness chip that
  opens the setup check, and, while profiling is on, its address.

To rename a port, select it and press `F2`, or use **Ports → Rename port** or
the node's context menu. Links and the saved setup use the new name. A
pseudo terminal on Linux or macOS moves its stable path to the new name, so
a program that opened it by that path needs the new one. The path of every
other kind stays as it was made.

The window opens at 1280 by 800 and works down to 960 by 640. Dock sizes are
remembered; **View → Reset layout** puts them back. **Theme** (follow the
system, light or dark) and **Density** (comfortable or compact) are under
**View** too.

## Jobs (Simple)

At the Simple level the page on the right starts as the job grid. It opens with
a strip that says whether this PC is ready to create ports other programs can
open (**Details**, or **Open setup check** when something is missing), then the
jobs in four groups:

| Group | Job | What Studio builds |
|---|---|---|
| My program needs a serial port | **Give my program a serial port** | One virtual port. |
| | **Connect two programs to each other** | Two virtual ports linked two-way, like a null-modem cable. |
| I have a serial device | **Use my device through a new port** | The device and one virtual port for your program. |
| | **Share my device with several programs** | The device and a virtual port for each program. |
| | **Try my device from this window** | The device alone, to type to and read from here. |
| | **Let other computers use my device** | The device bridged to a network port (default 4001). |
| Several programs or devices at once | **Put several on one line** | A [shared bus](#connecting-ports), like an RS-485 line. |
| | **Send several into one** | A join: everything the senders write reaches one receiver. |
| | **Let programs take turns sending to a device** | A split from the device to the programs with an owner, so one program sends at a time and all of them get the device's replies. |
| Look and check | **See the data on a port** | Nothing new: it opens **Data going through** on a setup you already have, which only looks. |
| | **Check this PC is ready** | Opens the setup check. |

**Put several on one line** is disabled with "Not available in this build"
where the engine has no bus, and **Let other computers use my device** where it
has no network endpoints. Below the jobs, **Open a saved setup…** and **Start
from an example…** open the file chooser and the [templates](#saving-opening-and-templates).

A job opens a form. Its parts depend on the job:

- **The device**: **Plugged into this PC** (a list with **Refresh**) or **On
  the network** (an address, a port and TCP or RFC 2217).
- **The programs**: a row for each, with a port name and an optional program
  name such as PuTTY, which is only a label. **Add a program** adds a row, up to
  32 (64 for **Put several on one line**), and **Add a device** where a device
  may take part: on one line, and when sending several into one. With the Studio
  driver on Windows the port name is a list of the next free COM numbers;
  elsewhere Studio picks the name.
- **On the other end** (give my program a port): **I'll type to it from this
  window**, or **Send everything straight back**, which makes a mock fixture
  that echoes.
- **Device speed (baud rate)** and **Other device settings**, **Name (optional)**,
  and tick boxes such as **Let me type to the device from this window** and
  **Show me the data going through**.

A sentence at the bottom says what will be built. **Create** builds it, and if a
step fails Studio removes what it made.

The page of a setup says what to do next:

- **What to do now**: for each program, "In PuTTY, open **COM7** at 115200
  baud", and a reminder not to open the device itself.
- **Ports**: each port with its name, a copy button and a chip: **No program
  yet** until a program opens it, then **Open in a program** (just **Open**
  where Studio cannot see that). A device row shows **Open at 115200 baud**. A
  network listener shows how many computers are connected. When a program has
  set a different baud rate from the device's, a note says so. **Add a
  program** adds a port without interrupting the others, and the minus button
  closes one.
- **Type to** a port: a box for what you type and a view of what comes back.
  **Enter adds a line ending** is a switch.
- **Data going through**: **Show me the data going through** starts a live
  list of what is sent each way, with **Pause**, **Clear**, **Show as hex** and
  **Hide**. It only looks.
- **Change…** reopens the job form for the setup. **Remove setup** closes it,
  naming the ports it closes. A setup made at the Advanced level has **Change in
  Advanced mode** instead, and the page ends with **See it on the canvas
  (Advanced mode)**.

## The launcher (Advanced)

At the Advanced level an empty studio shows the empty diagram and its toolbox.
**View → Show launcher** and **File → New** bring up the launcher, a page like
the job grid with four jobs:

1. **Give my program a serial port.** Creates a virtual port and tells you the
   name to open.
2. **Connect two programs.** Creates a linked pair, like a null-modem cable.
3. **Share one device with several programs.** Opens the share wizard.
4. **Watch traffic.** Starts a read-only copy of a port's traffic.

It opens with a strip that says whether this PC is ready to create ports other
programs can open. Below the cards, **Saved setups** shows your most recent file
with **Open** and **Browse**. The launcher goes away when the first port exists.

![The launcher on Windows 11: a setup warning strip, four job cards and the Saved setups row](images/launcher-light.png)

The launcher in the dark theme:

![The launcher in the dark theme, with the same four job cards on a dark background](images/launcher-dark.png)

The launcher in the compact density at the smallest window size, 960 by 640:

![The launcher in the compact density at 960 by 640, with a toolbar of icons only](images/launcher-compact-light.png)

## Quick start

1. Choose **Give my program a serial port**, optionally give the port a name (on
   Windows with the Studio driver, pick a COM number), and press **Create**. At
   the Advanced level use **Ports → Add port** (`Ctrl+Shift+N`) and **Create
   port**; the **Port ready** dialog opens instead of a setup page.
2. Studio shows the name to open, for example `COM7` on Windows or `/dev/pts/3`
   on Linux. Press the copy button, or **Copy name** in **Port ready**.
3. In the other program, choose that port. Whatever it writes appears in the
   setup's **Type to** view, or in the monitor.
4. To try the port without another program, choose **I'll type to it from this
   window** when you create it, then type in **Type to**. At the Advanced level,
   use the **Send** panel: pick the port, type text, press **Send**, and watch
   the monitor.

On Linux the **Port ready** dialog looks like this:

![The Port ready dialog on Linux, with /dev/pts/1 in large type and a Copy name button](images/port-ready-linux-light.png)

## Adding a port

**Ports → Add port** (`Ctrl+Shift+N`) asks what the new port should be:

- **Virtual port** — a port other programs can open.
- **Linked pair** — two ports wired together like a null-modem cable.
- **Real device** — a serial device plugged into this PC, so you can share it.
- In Advanced, **Network endpoint** and **Mock fixture** cards also appear.
  They do not make anything yet: **Create port** stays off. Use **Ports →
  Network endpoint** and **Ports → Mock fixture** instead (see below).

![The Add a port dialog with five choices and the Virtual port settings below](images/add-port-light.png)

### Virtual port

- **Name**: a unique suffix is added if the name is taken. With **How many**
  above one, names run `port-1`, `port-2` and so on. The number must be 1 to
  32.
- **COM number** (Windows, with the Studio driver and its helper service,
  and only for *Best available*): *Automatic* takes a free number above the
  ports this PC already has (never COM1 or COM2), or type the number a program
  expects, 1 to 255. It names one port, so it is off
  when **How many** is above one. A number that is refused is explained under
  the field and nothing is made; see [Choosing a COM number
  (Windows)](#choosing-a-com-number-windows).
- **Kind**: *Best available (recommended)* uses the Virtual Serial Studio
  driver on Windows when it is installed, then com0com, and a pseudo terminal
  on Linux and macOS; *Pseudo terminal*; or
  *In-process loopback*, whose port only Studio can open. On Windows there are
  no pseudo terminals, so *Pseudo terminal* is disabled there.

A sentence under the form says what will be created. When you press **Create
port**, the **Port ready** dialog opens with the path in large type, **Copy
name**, a note on how to use it, and **Connect COM7 to something**, **Create
another** and **Done**. **Show details** gives the saved-setup entry and the
equivalent `vss_cli` command. Without a driver on Windows the title reads
"port-1 is ready inside Studio" and the dialog warns that other programs cannot
open it, with a button for the setup check. With a driver installed and a
port that only Studio can open (an in-process loopback), the note says so
instead of asking you to install a driver.

### Linked pair

**Connect two programs** creates two ports that talk to each other. Choose the
pair name. With the Virtual Serial Studio driver on Windows, Studio makes both
ports when you press **Create pair** and Windows chooses their COM numbers, so
the port fields read "Chosen when you create the pair" and no free pairs are
needed. With only com0com on Windows the two names are shown for you to read;
they come from the free com0com pairs, and the dialog uses a pair that already
exists and never creates one. On Linux and macOS they are paths such as
`/dev/pts/3`. DTR is wired to DSR and RTS to CTS, as on a standard null-modem
cable. Neither route asks for administrator permission. With com0com, when
there are not enough free driver pairs the button is disabled and the dialog
points to the setup check; the share wizard can create another pair. In
Advanced, **More options** shows **Line wiring** and **Port kind**. Only
*Standard null-modem* and *Best available* can be chosen in this build; the other
wirings and kinds are disabled with "Not available in this build".

The picture shows a Windows PC with no free com0com pairs: **Create pair** is
off and the dialog points to the setup check.

![The Connect two programs dialog with Create pair disabled and a warning that there are not enough free driver pairs](images/linked-pair-light.png)

### Real device

**Attach a real device** lists the serial devices when the dialog opens; press
**Refresh** after plugging one in. A com0com pair end is listed as *Virtual* and
cannot be chosen, because it is a port and not a device. A port made by the
Virtual Serial Studio driver is listed the same way. Studio cannot tell in
advance that another program holds a device: if it does, opening fails with a
message that offers **Try again** and **Choose another**. If the device is not
listed, **Enter a path instead** takes `COM3` or `/dev/ttyUSB0`. In the Add port
dialog, choosing *Real device* and pressing **Next** opens this dialog; **Attach
device** opens the device.

The settings (baud rate, data bits, parity, stop bits, flow control) are folded
away in Simple with a one-line summary and open in Advanced. **Use common
defaults** sets 115200 8N1. Keep **Capture traffic in the monitor** ticked to
record it. The dialog shows the address it will use, for example
`serial://COM3?baud=115200`; in Advanced you can edit it and the edit wins over
the fields.

Studio opens the device once and keeps it open. Your programs open the port
that Studio creates, not the device.

![The Attach a real device dialog listing COM3 as available, with the address serial://COM3?baud=115200](images/attach-device-light.png)

### Network endpoint (Advanced)

**Ports → Network endpoint** (**Put a device on the network**) bridges a real
serial device to a network address, as `vss_cli bridge` does. Choose the
**Device to share**, the transport (TCP, UDP or RFC 2217), then whether Studio
**connects to a server** or **waits for other computers to connect (server)**,
and the address and port. Waiting works for TCP, UDP and RFC 2217. **Start
bridge** links the device and the address both ways. **Test from this PC first**
picks the server role on `127.0.0.1`. **Edit address** lets you type the
address yourself (see [network addresses](#network-addresses)), and the dialog
shows the equivalent `vss_cli bridge` command. To put a network port on a
[shared bus](#connecting-ports) rather than bridge a device to it, write the
port in a setup file or in **Topology JSON**; see [A device on another
computer](#a-device-on-another-computer).

![The Put a device on the network dialog with TCP chosen, a server address and the equivalent vss_cli bridge command](images/network-endpoint-light.png)

### Mock fixture (Advanced)

**Ports → Mock fixture** adds a scripted pretend device for tests without
hardware. Give it a name and a baud rate, and choose a port to link it to
two-way, or *Nothing yet*.

![The Add a mock fixture dialog with a name, baud rate and the port to link it to](images/mock-fixture-light.png)

A fixture can do two things by itself. In the Inspector (or the port's context
menu) **Echo received bytes** sends every byte written to it straight back. The
**Generator** sends **Text line**, **Fixed pattern** (hex), **Counter** or
**Random bytes** at a chosen rate, until you press **Stop generator**.

Select the fixture and choose **Open line matrix and faults** in the Inspector
(or **Topology → Link wiring**) to inject a fault: a parity error, a framing error, an overrun, a break, a
hang-up or a disconnect. Each one is reported as an error event on the port, and
the fixture then stops and shows *Failed*, as a real device would after the
fault. Add another fixture to inject again. Every fault you inject is listed in
**Injected so far**. Faults work only on fixtures; the buttons are off for any
other port.

The same dialog holds the **line matrix** for the link between the fixture
and the port it is linked to. The rows are the outputs DTR and RTS and the
columns are the inputs DSR, CTS, DCD and RI; tick a box to make that output
drive that input on the other port, or pick a preset. A change applies at once,
including to the inputs the other port already shows. The default is the
null-modem cable (DTR to DSR and DCD, RTS to CTS) and one wiring serves both
directions of the link. The wiring lasts while Studio runs and is not saved in a
setup file. A port whose driver cannot set RI shows it in Studio only.

![The line matrix and faults dialog for a fixture: six Inject buttons, the Injected so far list and the control line state](images/fixture-faults-light.png)

## Building on the diagram (Advanced)

At the Advanced level you can build straight on the topology. Everything here
makes the same ports and links as the dialogs, so use whichever you prefer.

- **The toolbox** on the left of the diagram lists tiles. Under **Ports**:
  **Port**, **Fixture**, **Loopback**, **Device** and **Network**. Under
  **Bundles**: **Pair**, **Split**, **Join**, **Bus**, **Switch** (several
  programs sending to one port, one owner at a time), **Redirect**, **Monitor**,
  **Share** and **Bench** (several fixtures, each paired with its own port).
  **View → Show toolbox** (`Ctrl+Shift+B`) hides and shows it.
- **Add a tile**: drag it onto the diagram, or select it and press `Enter`. A
  small **Add** sheet asks for each end (a new port, a new fixture, a loopback or
  a port you already have) and a name, **How many** for a bundle with a count,
  and baud rate and framing under **More settings**. **Add** makes the ports and
  the link in one step, and removes what it made if a step fails. **Device**,
  **Network** and **Share** open their own dialogs. **Topology → Add port**, **Add
  fixture** and **Add pair** do the same from the menu.
- **Link two boxes**: drag from the round handle on the edge of one box onto
  another, then choose **Two-way**, **One way, from A to B** or **Monitor A on
  B**. Dropping on empty space offers **New port here**, **New fixture here** and
  **New loopback here**. A drop that would close a loop is refused with "That
  link would create a loop". With several boxes selected, **Link selected** in
  the box's menu builds a split, a join, a bus or a switch. From the keyboard,
  **Connect from here** (`Ctrl+L`) asks for the other port.
- **Right-click a box** for **Inspect**, **Settings...** (baud rate, data bits,
  parity, stop bits, flow control and timeouts, as far as the kind of port has
  them), **Rename**, **Duplicate** (`Ctrl+D`; not for a real device or a network
  client, which can be opened only once), **Connect from here**, the port actions
  (**Send bytes**, **Start repeat**, **Open in monitor**, and for a fixture
  **Start generator** and **Echo received bytes**), **Copy name** and **Delete**.
  **Right-click a line** for **Settings...** (see [Delay and
  jitter](#delay-and-jitter-on-a-link)), **Reverse direction** (a one-way link)
  and **Delete**. Right-click empty space to add a port, a fixture, a loopback or
  a bundle, or **Select all**.
- **Topology → Tidy layout** arranges the boxes, and **Fit** (`Ctrl+0`) fits them
  in the pane.

Most of these steps can be undone; see [What this build cannot do](#what-this-build-cannot-do).

## Connecting ports

**Topology → Connect ports…** (`Ctrl+L`, enabled once two ports exist) creates a
link:

| Relationship | Use it when | What the target sees |
|---|---|---|
| **Two-way** | Two ports should behave like a null-modem cable. | Everything, both directions. |
| **One-way redirect** | Data must flow from A to B only. | Writes from A; B cannot reply. |
| **Monitor** | You want to watch without disturbing. | A one-way copy that never drives modem lines. |
| **Split one port to many** | Several programs must see the same device. | Each target receives a copy of the source. |
| **Join many ports into one** | Several publishers feed one program. | The target receives every publisher's data. |
| **Shared bus** | Several programs share one wire, like stations on an RS-485 line. | Every other port's data; never its own. |

The dialog lists only ports that exist, refuses a port linked to itself, needs
at least two targets for a split, two publishers for a join and two ports for a
bus, and prints a sentence describing the result before you commit. A link that
would make a loop is refused, with a message naming the pair, and nothing
changes.

![The Connect ports dialog with the relationships on the left and the From and To ports on the right](images/connect-ports-light.png)

A join has **Choose one owner at a time** under **More options** (Advanced):
only the owner's bytes reach the target and the others' are counted as dropped.
On a two-way link or a split the owner is one of the targets: only its bytes
reach the source and the others' are counted as dropped, while what the source
sends reaches every target.
The owner bar under the diagram says who the owner is, with **Give to** and
**Release owner**. A shared bus carries no modem lines and has no owner, and a
port can be on only one bus (at most 64 members). Where the engine has no bus
(**What this build cannot do**), the choice is disabled with "Not available in
this build".

Remove a link by selecting its line and pressing `Delete`, or **Topology →
Remove link**. Removing a port asks first and removes its links too. When the
port belongs to something built together, such as a share, the question offers
**Remove only** that port or **Remove the whole setup** (also **Ports → Remove
setup**). Removing one program's port from a share keeps the others sharing. A
port on a shared bus just leaves the bus, which goes when fewer than two ports
remain.

## Sharing one device with several programs

**Topology → Share a device** opens a wizard.

1. **Choose the device** to share, and a name.
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
   of what each program should open. If any step fails, the wizard removes what
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

![Share step 1: choose the device, COM3, and review its settings](images/share-device-light.png)

![Share step 2: three programs, the port each will open, and a warning offering Create another pair](images/share-programs-light.png)

![Share step 3: terminal on COM7, test-tool on COM8 and logger as Studio only](images/share-ready-light.png)

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
3. Open one port in each program, as **Port ready** shows.
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
| RS-485 with two wires: every station on one pair | A **shared bus** | Every station hears every other, and none hears itself. |

Make the four-wire case with **Topology → Connect ports…** and **Split one port
to many**, or `{ "mode": "split", "sources": ["master"], "targets": ["device-1",
"device-2"], "emulate_modem_lines": false }`. A split already carries the
devices' answers back to the master. Do not add a one-way join from the devices
back to the master's port to "complete" it: that makes a loop, and Studio refuses
it ("That link would create a loop" in the window, "link would create a cycle"
from `vss_cli`). To listen without taking part, give a
listener a **monitor** or a **join** from the ports, as in [One master, several
programs and a silent listener](#one-master-several-programs-and-a-silent-listener).

### RS-485 is half-duplex, and the bus is not yet

On a real two-wire RS-485 line one station talks at a time. A station switches its
transmitter on, sends, and switches it off, either in hardware (automatic
direction) or by the program raising and dropping RTS. Studio's shared bus gives
the same hearing, and a master that waits for the answer to each poll works as it
would on the wire. It does not enforce the one-at-a-time rule:

- **Overlap is not reported.** If two programs send at once, their bytes are
  interleaved in the order they arrive. A real line would garble them, and Studio
  neither garbles them nor says that it happened.
- **RTS is not carried.** A program that raises RTS to enable its transmitter
  runs, but nothing is gated by it.
- **No echo.** A port never hears its own bytes. A transceiver that echoes what
  its station sends, which some adapters do, is not modelled, so a program that
  expects to read its own transmission back will not.
- **Turn-around time is yours to add.** The time a device takes to switch from
  receiving to sending is not modelled. To rehearse a late answer use a delay on
  the link or in a [scripted device](#delay-and-jitter-on-a-link), and measure
  with a capture.

A bus option that reports overlap, and optionally holds the line for a turn-around
time, is planned and is not in this build; the plan is in
[gui-design/docs/08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).

## Old programs on Windows

Many programs that talk to serial equipment were written long ago. They are
often 32-bit, they open one fixed port, and they have habits that a port made by
Studio has to meet. This is what to set up, and what to expect.

### 32-bit programs

The driver is 64-bit (x64 and ARM64); there is no 32-bit Windows driver. A
32-bit program on 64-bit Windows reaches it as it reaches any COM port, and
Windows passes the serial requests through unchanged, because they carry no
pointers. A small 32-bit test program and a script that runs it against the driver
(tests W1 to W4 and T1 to T7 in
[drivers/windows/README.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/drivers/windows/README.md))
are written, and the program is built in the project's checks. The README says
which results are in, so treat a 32-bit program as expected to work and try
yours.

### The name a program opens

A program opens a port by name, and there are two spellings:

- `COM3`, and the other names up to `COM9`, work in every program.
- `\\.\COM12` is the form that works for every number. A program that builds the
  name as `"COM%d"`, and not as the C string `"\\\\.\\COM%d"`, cannot open
  `COM10` or above, because
  Windows reads the short form only up to 9.

For an old program choose a number from 1 to 9 and keep it. Pick it in **Add
port** (the **COM number** field) or with `vss_cli virtual --com=3`; see [Choosing a
COM number (Windows)](#choosing-a-com-number-windows). A program that keeps its
port in a setting file (for example `CommPort=3`) then finds the same port in
every run, and a program that lists only `COM1` to `COM4` can be given a number in
that range if the real ports on the PC leave one free.

Make the ports before you start the program, with the numbers it expects. Many
old programs fill their list of ports once, when they start. A port that is made,
or that comes back, later is not in the list until the program is started again.

### Programs that try every port

Some programs look for their equipment by opening `COM1`, `COM2` and so on up to
`COM16` in turn when they start. Studio's driver ports behave like real ones here:

- **One program has a port at a time.** A second open of a port that a program
  holds fails with "access denied", as it would on real hardware. A probe that
  meets a port held by another program moves on to the next.
- **A probe that finds your port may keep it.** If the first program to start
  probes all sixteen and keeps what it finds, it can take a port meant for
  another. Start the programs one at a time, in the order they should claim their
  ports, and watch the Ports list: **Waiting for program** becomes **Program
  connected** when a program opens the port.
- **Fixed numbers make probing predictable.** With the ports at `COM3`, `COM4` and
  `COM5`, a program that probes finds them where it expects them, in the same
  order, every run.

### What stops when Studio does

Studio's driver ports belong to Studio and are removed when it quits, or if it
stops without warning. A running program loses its port at that moment and may
stop or show an error. Closing the window is not quitting: see
[When the window is closed](#when-the-window-is-closed).

## Watching traffic

**Topology → Watch traffic** makes a read-only copy of a port. Choose the port
and a name for the watcher (the dialog suggests `<port>-monitor`) and press
**Start watching**. The watcher is a Studio-only port, shown in the
ports list so you can remove it when you have finished, and it never drives
modem lines. With **Open the monitor when it is ready** ticked, Studio selects
the watched port and brings the monitor forward. The monitor is not filtered to
that port; you can set the port filter yourself. At the Simple level,
**Show me the data going through** on a setup's page also shows the traffic, without making a watcher port.

![The Watch traffic dialog with the port to watch, the watcher name and Open the monitor when it is ready ticked](images/watch-traffic-light.png)

## The monitor and the Send panel

The **Monitor** shows a row per transfer: **Time**, **Port**, **Direction**
(`RX`, `TX`, or `DROP`), **Bytes** and **Preview**. A `DROP` row is one line each
time a link's dropped count grows, for bytes that a port without the owner's
right to write tried to send. It shows the count and not the bytes, because the
hub does not publish them.

- **Pause** (or `Space` in the table) stops appending. Traffic still flows and
  is counted. **Clear** empties the view. **Follow** keeps the newest row
  visible.
- The preview switch shows payloads as **ASCII** or **Hex**. You can filter by
  port and by RX, TX or Dropped, search the payload (`Ctrl+F`), and **Export**
  the visible rows as text. **Export capture** saves the same rows as a capture
  file (see [Capture files](#capture-files)).
- The monitor keeps the latest 10,000 rows, and no more than 32 MiB of their
  bytes, oldest first. The **Preview** column stops at 256 bytes, but the
  rows keep every byte for **Export capture**. For a record that does not stop
  at what the window holds, use `vss_cli capture`.

### Capture files

A capture is the traffic written with its exact time, in a form another
program can read. The monitor's **Export** is for reading: its **Time** is when
the window received a batch, and it cuts each payload to the preview. **Export
capture** and `vss_cli capture` write every byte, and the time is the one the
engine read when the bytes moved. That is a single clock for every port,
including a port whose other end is on another computer: the remote machine's
own clock is never used.

Each record has an offset in milliseconds from the start of the capture, the
port, a direction and the bytes of one transfer. The direction is from the
port's point of view:

- **R** is bytes the port *received*, from whatever is attached to it: the
  program that opened it, a device, a remote client.
- **W** is bytes *written to* the port, which the hub sent on to that attachment.

On a link, one transmission is `R` on the sender's port and `W` on the port of
every receiver, and a watcher port made with **Watch traffic** shows it once, as
`W`. To keep one row for each message, capture only the port you want
(`--port` on the command line); in the window, set the port filter first, as
the export writes the rows that pass the filters.

| Format | What it holds |
|---|---|
| `.cap` | Text: `[         142] R 0x10\|0x02\|0x50`, one line per transfer. The offset is right aligned in ten columns, the bytes are lower-case hexadecimal, and lines starting with `#` are comments. The file has no port names, so capture one port per file. |
| `.csv` | A header row, then `time_utc,offset_ms,port,direction,bytes,hex` for each transfer: the absolute time, the offset, the port name, `R` or `W`, the length, and the bytes as `10 02 50`. |
| `.jsonl` | One JSON object per line. The first has `started_utc`, the wall clock at offset zero. Each later one has `offset_ms`, `time_utc`, `port`, `direction`, `bytes` and `hex`. |

Offsets are whole milliseconds, so two transfers in the same millisecond have
the same offset; the order of the lines is the order the engine delivered them.

**Send** writes bytes into an open port. The line above it says "Sending from"
the selected port, and the list beside **Send** can change it; **Pin** keeps
sending from that port when you select another. Type text, or tick **Hex** for
hexadecimal (it is checked as you type), leave **Add CRLF** ticked to end the
line, and press **Send**. **Repeat** sends every 50 to 5000 ms until you press
**Stop repeat**, and **Saved sends** keeps your favourites. Engine errors appear
as a message and in the log. At the Simple level, **Type to** on a setup's page
does the same for its port.

## Control lines

Select a port to see its control lines in the **Inspector**. **DTR** and **RTS**
are switches, **Break** is a button, and CTS, DSR, DCD and RI are chips ("CTS on"
or "CTS off"). A port that cannot drive lines says so. Virtual ports accept the
operations and report their own state; they have no physical wires. The
Inspector is an Advanced pane.

## Link wiring and ownership (Advanced)

- **Topology → Link wiring** opens the line matrix and faults dialog (see [Mock
  fixture](#mock-fixture-advanced)) for the selected port, or for the first
  fixture when nothing is selected. It sets which signals the port drives on the
  port at the other end of its two-way link.
- **Topology → Owner** goes to the owner bar under the diagram for a join or a
  split that uses an owner policy. The owner can be released or given to another
  port: on a join one of the sources, on a split one of the targets (never the
  device the targets share). A link with one target has no choice to make, so it
  has no bar.
- **Tools → Benchmark** sends data from one port and counts what arrives on
  another, so you can measure a cable or a driver pair. Choose the two ports,
  the amount (1, 10 or 100 MB) and the chunk size, then **Run**. The bar shows
  progress, and **Cancel** stops the run. The result is *Passed* when every byte
  arrived and *Failed* when some did not, with the number lost; **Copy result**
  puts the figures on the clipboard. The two ports must not be linked in
  Studio, because the data has to travel through the cable or the driver pair.
  It is the same measurement as `vss_cli bench`, and the dialog shows the
  matching command.

Some of these depend on what the engine can do; see
[what this build cannot do](#what-this-build-cannot-do).

## Choosing a COM number (Windows)

Many older programs open one fixed port, `COM3` for example, and keep it in a
setting file, or list only `COM1` to `COM4`. A port from the Studio driver can
be given the number you choose, as in Device Manager's *Advanced > COM Port
Number*, so the program always finds it.

From the command line:

```
vss_cli virtual --kind=windows-driver --com=3
```

or in a saved topology, as an optional field on a virtual port (1 to 255):

```json
{ "name": "master", "kind": "virtual", "virtual_kind": "windows-driver", "com_number": 3 }
```

Studio also remembers the number it was asked for and saves it with the topology.
A port with no `com_number` takes the lowest free number, and the same slot keeps
its number from one run to the next, but a different order of creation can give a
different one.

In the window, choose the number in **Add port** (the **COM number** field,
*Automatic* by default), or select a driver port and press **Change...** beside
its **COM number** in the Inspector. The change restarts the port, so the window
tells you when a program has it open and refuses until the program lets go; use
[Release port](#when-a-program-will-not-let-go-of-a-port-advanced) if it will
not. The library does the same (`set_driver_com_number`, `studio::set_com_number`).

What to know:

- **The helper service is needed.** A COM number is held in Windows' COM name
  database, which only an administrator may change. The service
  `vsscom_helper` does what Device Manager does for you; install it with
  `install-helper.ps1`. Without it, a port without a number is made as before,
  and a request for a number fails with a message that says so.
- **A number that another device has is refused.** A real port, a com0com port
  or a port that has gone but still keeps its name, are all "taken". Studio makes
  no port and says which number was refused, and the number stays free.
- **To change the number of a port that exists, close the program using it
  first.** The port restarts, which takes a second or two. For that moment it
  loops back, and bytes that are waiting carry on afterwards. If a program
  opens the port at that instant, Windows will not restart it again until the
  port is removed and made again.
- Names above `COM9` must be written `\\.\COM12` by programs that open ports by
  name, which most do not. A program that opens `"COM%d"` fails above 9, so
  choose a number from 1 to 9 for such a program.

## Ports under running programs

A port of the Studio driver, and on Linux a pseudo terminal, tells Studio when a
program opens it. The Ports list and the Inspector then read **Waiting for
program** until something opens the port and **Program connected** once it does,
so with several programs to start you can see which one has not yet.

Studio's ports disappear with Studio, so a program that holds one loses it when
you remove the port, start a new setup or close the window. Old programs may
stop or show a bare error. Before any of those Studio names the ports that
programs hold and asks you to confirm; it asks nothing when no program is known
to hold a port. For a port Studio cannot observe (a hardware port, com0com, or
anything on macOS) it does not guess, so it does not warn either.

## When a program will not let go of a port (Advanced)

Windows frees a port at once when its program ends, so this is rare: a program
that is still running holds the port, and a second program cannot open it.
Turn on **Advanced**, select the port (it reads **Program connected**) and press
**Release port...** in the Inspector. The dialog names the program and its
process and says how long no data has passed on the port. That is only a fact:
Studio cannot tell a stuck program from a quiet one, so it never says the program
is stuck. Choose one way, then press the button:

- **Take the port back** (try this first). The program keeps running, but its
  connection to the port stops working, and another program can open the port at
  once with the same COM number. The Studio driver does this; Studio does not
  touch the program.
- **End the program.** Windows closes it straight away, as *End task* in Task
  Manager does, and it does not get to save anything. Studio ends only your
  own programs. For a program of another user, run Studio as administrator
  (**Restart as administrator** in **Tools → Setup check**); until then this
  choice is off and says why.

## Saving, opening and templates

A setup is a JSON document that lists the ports and links.

- **File → Save** (`Ctrl+S`) and **Save as** (`Ctrl+Shift+S`) write the current
  setup. **File → Open** (`Ctrl+O`) applies a saved one; **Open recent** opens
  the latest.
- The title bar shows `*` while there are unsaved changes. Closing, **New** and
  **Open** ask first, and you can turn that off in Settings.
- **File → Setups and templates** opens **Setups**, with a **Templates** tab and
  a **Recent** tab (and **Save current setup as template**). The templates are a
  test bench with no hardware, a device shared with three programs, a device on
  the network, and two programs sharing one device one at a time. A template
  that needs a real device says "Needs a real device" and cannot be used yet;
  **Use template** opens one that does not.

![The Setups dialog with four templates; two say Needs a real device](images/templates-light.png)

- **File → Export as command** (Advanced) shows the `vss_cli` command that
  builds the current setup.

![The Export as command dialog showing vss_cli run setup.json --seconds=60](images/export-command-light.png)

The same file can be replayed without the window:

```bash
vss_cli run topology.json --seconds=60
```

## When the window is closed

Your ports run on this computer until you quit Studio or open another setup.
The window is only for setting them up and watching them.

- **Closing the window** with ports running keeps them running. Studio stays in
  the notification area (the menu bar on macOS); click its icon, or start
  Studio again from the Start menu, to bring the window back with everything
  live. There is only one Studio at a time. With no ports, closing quits.
- **Quitting** (**File → Quit**, `Ctrl+Q`, or **Stop the ports and quit** in
  the icon's menu) stops every port and link. Studio asks first.
- **The running setup** is kept by itself, a second after each change. After a
  restart, a sign-out or a crash, the same ports start again the next time
  Studio starts. After a Quit they do not start by themselves: a banner offers
  **Start again**.
- **Start when I sign in** (in the icon's menu, or **Settings → Running**)
  starts Studio hidden when you sign in, so the ports are back after a restart
  without opening anything. It is off until you turn it on, and it is not
  available on macOS yet.
- **Saved setups** are unchanged: **File → Open** replaces what runs with the
  saved setup, which then runs until you quit or open another, and you can
  change it and save it again.
- **Settings → Running → When I close the window** can make closing quit, as
  before.
- On a desktop with no notification area, closing with ports running asks
  whether to minimise instead.

`vss_cli run setup.json --seconds=60` runs a setup without any window, in its own
process, and exits after the time you give. Studio cannot see those ports.

## Network addresses

Under the choices in the dialogs, every endpoint is an address. The engine
accepts these forms, and you can type them in Advanced and in `vss_cli`:

| Address | Meaning |
|---|---|
| `serial://COM3?baud=115200` | A real serial device with framing options. |
| `tcp://host:port` | A TCP client: Studio connects to a server. |
| `udp://host:port` | UDP datagrams sent to and received from one address. |
| `tcp-listen://host:port` | A TCP server: Studio waits for other computers. Any number can connect; what you send goes to all of them. |
| `udp-listen://host:port` | UDP datagrams from any sender; replies go to the senders heard from lately. |
| `rfc2217://host:port` | RFC 2217 serial-over-network: Studio connects to an access server. |
| `rfc2217-listen://host:port` | An RFC 2217 access server: Studio waits for serial-over-network clients such as pySerial's `rfc2217://`. |

An in-process loopback port reports its path as `loopback:<name>`.

An RFC 2217 server speaks Telnet with the COM-PORT option, so a client opens
it as a serial port. Data passes through as it would for TCP. The client's
DTR and RTS are the endpoint's output lines, so a linked port sees them as
DSR, CTS and DCD, and the CTS, DSR, DCD and RI that reach the endpoint are
sent to the client as modem state.

To let other computers connect, allow the port through the host firewall and
listen on an interface they can reach (`0.0.0.0` for all interfaces). Try
`127.0.0.1` first. Studio does not use the network unless you add an endpoint
like these.

### Who may connect

A listening address can carry a list of the computers that may connect, after
the port: `?allow=` and then addresses or ranges, separated by commas.

```text
rfc2217-listen://192.168.1.10:7001?allow=192.168.1.0/24,10.0.0.5
tcp-listen://0.0.0.0:4001?allow=fd00::/8
```

A computer that is not on the list is disconnected as soon as it connects,
before it can send or receive a byte, and a UDP datagram from one is ignored.
An address alone means that computer; `a.b.c.d/n` is every address that shares
the first `n` bits. IPv4 and IPv6 both work, and an IPv4 address that arrives
written as IPv6 counts as the IPv4 one. The list is not a password and does not
encrypt anything: an address can be forged, and the data crosses the network
as it is. It keeps strangers on the same network away from a port. To use it in
the network dialog, turn on Advanced and edit the address. Nothing in a saved
setup changes: the list is part of the address.

An RFC 2217 server has no password at all. The network dialog and `vss_cli
bridge` warn when such a server can be reached from another computer and has no
`allow=` list. Listen on `127.0.0.1` when the other program is on this computer.

Every connection Studio accepts is set to send what it writes at once, so a
short reply (a reply to a poll on a serial bus, for example) is not held back
waiting for more data.

### A device on another computer

A device program on another computer can be one member of a bus. The window's
**Network endpoint** dialog bridges a real device and does not make a port to
put on a bus, so write the port in a setup file, or paste it into **Topology
JSON** and press **Apply**: an `rfc2217` port whose address is
`rfc2217-listen://`, listed as a member of the bus (a complete file is in
[scenarios](scenarios.md#put-a-device-from-another-computer-on-the-bus)). On the
other computer open it with a serial-over-network client such as pySerial's
`rfc2217://host:port`. What to expect:

- **The member is there with or without the other computer.** With nobody
  connected, what the bus sends to it is dropped without an error and nothing
  comes from it, which is what an unplugged device looks like. Studio does not
  yet show whether a computer is connected.
- **Time is Studio's.** Bytes are stamped once, when they pass through Studio, with
  Studio's clock. The other computer's clock is not used, so a capture shows its
  device on the same time axis as the others.
- **The network is part of the reply window.** A master that waits 200 ms for an
  answer waits for the network hop as well. On one computer the hop is under a
  millisecond; across a network add its round trip to the device's own time, and
  use a [delay on the link](#delay-and-jitter-on-a-link) to rehearse it. Every
  connection Studio accepts is set to send at once, so a short reply is not held
  back for more data.

### Keeping it safe

An RFC 2217 server has no password, and nothing it carries is encrypted. The list
in [Who may connect](#who-may-connect) keeps strangers on the same network away,
but an address can be forged, and the data crosses the network as it is. Choose
the least open way that does the job:

1. **The same computer.** Listen on `127.0.0.1`. Nothing else can reach it.
2. **An SSH tunnel.** Listen on `127.0.0.1` on the Studio computer, and on the other
   computer forward a port to it, then open `rfc2217://127.0.0.1:7001` there:

   ```bash
   ssh -N -L 7001:127.0.0.1:7001 user@studio-computer
   ```

   The traffic is encrypted and authenticated by SSH, and Studio sees only a
   connection from itself.
3. **A VPN.** Listen on the address the VPN gives the Studio computer, not
   `0.0.0.0`, and add `?allow=` with the VPN's range, for example
   `rfc2217-listen://10.8.0.1:7001?allow=10.8.0.0/24`. Only members of the VPN can
   reach it.
4. **A network you control.** Listen on the interface they can reach, add
   `?allow=` with the computers that need it, and allow only that port through
   the firewall. Use this only on a network whose other users you trust.

Do not listen on `0.0.0.0` on a network you do not control, and do not make the
port reachable from the Internet. The network dialog and `vss_cli bridge` warn
when a server can be reached from another computer and has no `allow=` list.

## When the operating system asks, or says no

Windows, Linux and macOS each protect the computer in ways that can stop a
serial program or a network listener. Studio looks at these for you in **Tools →
Setup check**, says what it found, and tells you the smallest thing to do. It
follows three rules:

- **It tells you before anything happens.** Studio never raises its own rights,
  never changes a firewall rule and never changes a system setting without a
  button you press, after it has shown you exactly what it will do.
- **It says "Not checked yet" when it cannot tell.** It does not guess.
- **It never turns a protection off.** The fix is always a narrow permission, or
  a command for you to run yourself.

### Windows

**Administrator rights.** Studio and its command line run as you, and none of
them asks for administrator rights when it starts. A step that needs them (a new
com0com pair, a firewall rule) asks Windows for permission when you press its
button. Windows shows its own **User Account Control** window; that window is
the permission. The Setup check row *Administrator rights* says whether Windows
will ask you only to confirm (your account is an administrator) or to type an
administrator's password (it is not). *Restart as administrator* restarts the
whole program with those rights; if you decline the prompt, Studio keeps running
as before and says so.

**Windows Firewall.** Nothing is asked while Studio listens on this PC only
(`127.0.0.1`). When a network endpoint waits for other computers on `0.0.0.0` or a
network address, Windows Firewall blocks them until a rule allows it. The
*Network endpoint* dialog then shows **Check Windows Firewall for this port**, and
the Setup check shows a *Windows Firewall* row. They open a dialog that says:

- what Windows Firewall will do to this port, from reading its rules (Studio only
  reads them);
- the one rule Studio can add: this program, one protocol, one port, computers on
  your local network only, on the network you are on now (private, domain or
  public) and no other;
- the same rule as a `netsh` command, if you would rather run it yourself.

**Allow in Windows Firewall** starts a small helper (`vss_cli`, the command line
beside the program) through the Windows permission prompt. The helper does that
one thing and refuses any program that is not Studio. A rule that blocks the
program, or the *Block all incoming connections* setting, is never changed by
Studio; the dialog says where to change it. Remove the rule in Windows Defender
Firewall (its name starts with *Virtual Serial Studio*), or with the command
`netsh advfirewall firewall delete rule name="…"` that the dialog shows.

The same check is available from a terminal and changes nothing:

```
vss_cli permissions check --listen=tcp-listen://0.0.0.0:4001
```

### Linux

**Serial devices.** Who may open `/dev/ttyUSB0` is decided by the group that owns
it (`dialout` on Debian, Ubuntu and Fedora, `uucp` on Arch). The Setup check row
*Serial device access* reads the owner and mode of each device it found and says
which of four things is true:

- you may open it;
- your account is not in the group, and the row shows
  `sudo usermod -aG dialout <you>`. If `pkexec` (polkit) is installed the dialog
  also has **Add me to the dialout group**, which asks your desktop for your
  password and runs that one command. Sign out and back in afterwards;
- your account is in the group but this login does not know yet (sign out and
  in, or `newgrp dialout` in one terminal);
- the group may not read and write the device: the dialog shows the `ls -l`
  command. Studio does not change device permissions, because that is a system
  setting.

**When the file permissions allow it and it is still refused.** AppArmor, SELinux,
Flatpak and Snap can refuse a device or the folder Studio keeps its link names in
(`/run/vss`, then `/tmp/vss`). Studio recognises this when a device that the
permissions allow was refused, and the *Confinement* row (shown only when something
confines the program) gives the fix for the one that applies:

| Confinement | What Studio shows |
|---|---|
| Flatpak | `flatpak override --user --device=all <application id>` |
| Snap | `snap connections <snap>`; a serial device needs the `serial-port` interface connected |
| AppArmor | the lines to add to `/etc/apparmor.d/local/<profile>` (devices, pseudo terminals, link folder) and `sudo apparmor_parser -r /etc/apparmor.d/<profile>` |
| SELinux | `sudo ausearch -m AVC,USER_AVC -ts recent`, then `audit2allow -M vss_studio` and `semodule -i`; read what `audit2allow` proposes before you install it |

SELinux in enforcing mode is normal on Fedora and Red Hat systems, so it is
information and not a problem. Studio never edits a profile or a policy.

**Firewall.** Studio cannot read `ufw` or `firewalld` rules without administrator
rights. When a listener is open to other computers and `ufw` or `firewalld` is on,
the *Firewall* row shows the command that opens the port (`sudo ufw allow
4001/tcp`, or `sudo firewall-cmd --permanent --add-port=4001/tcp` and a reload).
With neither on, it says "Not checked yet" because `nftables` or `iptables` may
still filter.

`vss_cli permissions check --device=/dev/ttyUSB0 --listen=tcp-listen://0.0.0.0:4001`
prints the same facts.

### macOS

**Application Firewall.** Nothing is asked while Studio listens on this Mac only
(`127.0.0.1`). When a network endpoint waits for other computers and the firewall
is on, macOS shows its own question the first time one of them connects: *Do you
want the application to accept incoming network connections?* Choose **Allow**.
macOS remembers the answer for each program. The Setup check row *macOS firewall*
says which case you are in and, if you chose *Don't Allow* by mistake, shows the two
`socketfilterfw` commands that fix it (or you can set the program to *Allow
incoming connections* in **System Settings → Network → Firewall → Options**).
*Block all incoming connections* overrides every program, so there the row asks
you to turn that off. Studio only reads the firewall state; it never changes the
firewall.

**Local Network.** From macOS 15 an app needs your permission to find and connect
to devices on your local network, and macOS asks once. Studio cannot read the
answer, so the row *Local Network access* is information, and says where the switch
is (**System Settings → Privacy & Security → Local Network**) for the case where
you chose *Don't Allow* and other computers cannot connect.

**Serial devices.** `/dev/cu.*` devices are ordinary files that an ordinary user
may open, so the *Serial device access* row mostly says "you may open them". If a
USB serial adapter does not appear at all on a Mac with Apple silicon, macOS may be
asking you to allow the accessory (**System Settings → Privacy & Security → Allow
accessories to connect**); that prompt cannot be read from here either.

**The App Sandbox.** The release runs without the sandbox. A sandboxed build needs
the entitlements `com.apple.security.device.serial`, `network.client` and
`network.server`; the *Confinement* row names them if Studio finds itself sandboxed.

**Downloaded builds.** Until a release is signed and notarised (see
[Releasing](releasing.md)), Gatekeeper asks you to allow the app: right-click it, choose
**Open**, then **Open** again, or run `xattr -dr com.apple.quarantine
vss_gui.app`.

## Finding things: the command palette

`Ctrl+K`, or the **Search actions** box in the menu bar, opens a list of every
action. Type a few letters, use the arrow keys and press `Enter`. Actions that
need Advanced mode are listed under **Needs Advanced mode**, with a button that
turns it on.

## Settings

**Tools → Settings** (`Ctrl+,`) has four pages. Changes apply immediately.

- **General**: *Advanced mode*, *Default baud rate* (offered first when you
  attach a device) and *Ask before discarding an unsaved setup*.
- **Running**: *When I close the window* (*Keep the ports running in the
  background* or *Stop the ports and quit*) and *Start when I sign in*; see
  [When the window is closed](#when-the-window-is-closed).
- **Appearance**: *Theme* (follow the system, light or dark), *Density*
  (comfortable or compact) and *Link lines* (curved, the default, or right
  angles with rounded corners). Text size, display scale, high contrast and reduced
  motion follow your operating system.
- **Performance**: *Refresh interval*, 50 to 5000 ms. Shorter feels live;
  longer is lighter on a busy machine. Under *Profiling*, *Record a performance
  trace* lets a Tracy profiler watch where Studio spends its time, which is how
  a slowdown is found. It is off every time Studio starts, listens on this
  computer only, and sends nothing until you connect the profiler; the status
  bar shows the address while it is listening. *Let other computers connect*
  opens it to your network and can be chosen only before the first start.
  Turning the trace off stops recording, but the port stays open until Studio
  quits. Builds without the profiler show both switches as unavailable. See
  [profiling](profiling.md).

**Reset to defaults** restores them. The values are stored with `QSettings` (the
registry on Windows, a configuration file on Linux and macOS).

![The Settings dialog on the General page, with Advanced mode, Default baud rate and the unsaved setup prompt](images/settings-light.png)

## Help and diagnostics

- **Tools → Setup check** says what this PC needs so other programs can use
  your ports. On Windows it looks for the Virtual Serial Studio driver (and
  names its version) or com0com, whether administrator rights will be needed,
  and the real serial devices. Either driver counts as ready, and the com0com
  install guidance appears only when neither is found. It also says whether
  Windows Firewall lets other computers reach a port you listen on. On Linux and macOS it
  looks at pseudo terminals and your serial port group. The status bar chip
  shows "Setup ready" or "Setup needs attention" and opens the same dialog.

![The Setup check dialog on Windows: the driver, administrator rights and real serial devices, each marked OK](images/setup-check-light.png)

- **Help → Why is nothing arriving?** walks the steps between the device and
  your program (the port is open, it is linked, the device is sending) and
  suggests what to try. On Linux Studio can see whether a program has opened
  a pseudo terminal, and on Windows whether one has opened a port of the Studio
  driver, so those checks say so. Everywhere else it cannot, and the last two
  checks read "Not checked yet".

![The Why is nothing arriving dialog: the port is open, it has no link, and the later checks are not yet run](images/troubleshoot-light.png)

- **File → Diagnostic bundle** saves a ZIP with the setup, the event log, the
  environment and the port counters. Traffic contents are left out unless you
  tick them. Nothing is sent anywhere; you choose where to save the file.

![The Create a diagnostic bundle dialog with five choices; Traffic contents is off by default](images/diagnostic-bundle-light.png)

- **Help → Keyboard shortcuts** lists every shortcut; **Help → Copy
  environment** copies the build details for a bug report.
- **Help → About** shows the application icon, the version and the licence
  at the top, and has four tabs: About (the GPLv3 notice and a link to the
  source code),
  Environment (commit, branch, build, compiler, Qt, operating system and the
  language chosen), Licences (Qt, Boost, Boost.VirtualSerial, Tracy and the
  optional com0com driver) and Fonts (none are bundled).

The application is available in nine languages (English, French, Spanish,
German, Hebrew, Yiddish, Japanese, Simplified Chinese and Arabic). It follows
the system language, and there is no language setting in the window. See
[i18n.md](i18n.md).

## Keyboard shortcuts

`Ctrl` is `Cmd` on macOS.

| Shortcut | Action |
|---|---|
| `Ctrl+N` / `Ctrl+O` | New / Open |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save as |
| `Ctrl+Q` | Quit (the platform's Quit shortcut) |
| `Ctrl+Shift+N` | Add port |
| `Ctrl+Shift+C` | Copy port name |
| `F2` | Rename the selected port |
| `Delete` | Remove the selected port or link |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo the last change that can be undone |
| `Ctrl+L` | Connect ports |
| `Ctrl+K` | Search actions |
| `Ctrl+,` | Settings |
| `F5` | Refresh now |
| `Ctrl+Shift+A` | Advanced mode |
| `Ctrl+0` | Fit the topology (Advanced) |
| `Ctrl+D` | Duplicate the selected ports (Advanced) |
| `Ctrl+Shift+B` | Show or hide the toolbox (Advanced) |
| `Enter`, `Alt+Enter` | In the topology: inspect, open Settings for the selected port (Advanced) |
| `F1` | Help contents |
| `Space`, `Ctrl+F` | In the monitor: pause, search |

## When the ports are taken away

A port of the Windows driver lives as long as the helper service or the driver
that made it. If the helper service stops (Windows starts it again after five
seconds) or the driver is restarted, every port it made is removed under the
programs that had them open. Studio sees the ports stop, keeps them in the
setup with their links and COM numbers, and shows a banner above the diagram:

> Some COM ports were lost. The helper service or the driver was restarted and
> removed the ports your programs had open.

**Recreate ports** makes each one again with the COM number it asked for, and
puts it back under the same name and links, so nothing has to be connected
again. A port without a requested number can come back under a different COM
name, because Windows gives the lowest free number: ask for a number if a
program opens a fixed name (see *Choosing a COM number*). A program that had the
old port open has to open it again; Studio cannot do that for it. If a port
cannot be made, the banner stays and the status bar says why.

Connections that Studio makes itself can be retried. `vss_cli run
topology.json --reconnect` opens a `tcp://` or `rfc2217://` client again when
its connection drops, after one second and then twice as long each time, up to
thirty seconds, and logs each try as an event. Listeners and serial devices
are not retried. In the engine, `studio::set_reconnect_policy` sets the delays
and the most tries, and `studio::reopen` opens any port that has an address
again by hand.

## What this build cannot do

A control that the engine cannot back is shown disabled, with the tooltip "Not
available in this build". It never pretends. At the moment:

- **Undo of everything**: Undo and Redo cover connecting a link, removing a
  link, changing an owner and renaming a port, and, on the Advanced diagram,
  adding from the toolbox, duplicating, deleting, **Link selected**, link and
  port settings and reversing a link. They do not cover adding or removing a
  port with the menus and dialogs, a wizard or applying JSON, because a port that
  is made again is a different port. Any of those starts a new history.
- **Reconnect a failed network port from the window**: a port that stops shows
  *Failed* or *Closed* and keeps its links. The engine can open it again
  (`studio::reopen`), and `vss_cli run --reconnect` does so for `tcp://` and
  `rfc2217://` clients, but the window has no button for it yet. The one thing
  the window does is the **Recreate ports** banner for Windows driver ports
  that a restart took away (see *When the ports are taken away*).
- **Editing a link in place**: the engine cannot change a link that exists.
  **Settings...** on a link (Advanced) makes it again with the new delay,
  jitter, merge policy and modem-line emulation, and bytes in flight can be lost.
- **"In use" for a device**: shown only if the engine can prove it, and it
  cannot, so a busy device fails when you attach it.
- **Real-device templates**: they need the attach dialog to open from a
  template.
- **"Waiting for a program" and "Program connected" on macOS, and for any
  Windows port that is not from the Studio driver**: Studio can see whether a
  program has opened a port for a pseudo terminal on Linux and for a port of the
  Studio driver on Windows (it also logs each time with the process). Everywhere
  else a port reads **Open** and no more.
- **Settings sent by an RFC 2217 client** (baud rate, data bits, parity, stop
  bits, flow control): the server accepts and reports them but does not apply
  them to a real device linked to it. DTR, RTS and break from the client are
  passed on as control lines.
- **Half-duplex behaviour of a bus**: a shared bus does not report two programs
  sending at once, does not hold the line for a turn-around time and does not
  carry RTS. See [RS-485 is half-duplex, and the bus is not yet](#rs-485-is-half-duplex-and-the-bus-is-not-yet).
- A **shared bus** in a build whose engine has no bus link: the choice in
  **Connect ports** is disabled, and a topology file with a `bus` link is
  refused with "Bus links are not available in this build". The star (one port
  split to several, with a join to a listener) still works.

The reasons and the plan for each are in
[gui-design/docs/08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).

## Troubleshooting

**No COM ports appear on Windows.** A virtual COM port needs a driver: the
Virtual Serial Studio driver or com0com. Open **Tools → Setup check**. With
neither, *Best available* falls back to an in-process loopback, whose path is `loopback:<name>` and which other
applications cannot open. Pseudo terminals do not exist on Windows.

**Windows would not make the port.** The message names the part that is
missing: the driver, or the helper service (not installed, not running, out of
date, or closed to your account). **Set up Virtual Serial Studio** in the dialog
installs or repairs it; see **Tools → Setup check**.

**A COM number was refused.** `COM7 already belongs to another device on this PC`
means a real port, com0com or a port that was removed but keeps the name has it;
pick another number, or remove the old device in Device Manager (*View > Show
hidden devices*). `Choosing a COM number needs the Virtual Serial Studio helper
service` means `vsscom_helper` is not installed or is older than the driver; leave
the number on *Automatic* or install the helper. `A program has this port open`
means close the program, or use Release port in Advanced mode, then ask again.
`No more ports can be made` means you hold 16 driver ports (or the driver 32 in
all): remove one.

**A pseudo terminal path is shown but another program cannot open it.** On
Linux the path is `/dev/pts/N`; it exists only while the studio is running.
Check the permissions of the user running the program.

**A real device cannot be opened.** Another program may hold it, or it was
unplugged. Close the other program and try again. On Linux, add your user to
`dialout` and log in again; the setup check shows the command.

**Network peers cannot connect.** Check the firewall (on Windows, the Setup check
row and the dialog behind it in the Network endpoint dialog read the rules and
show what is missing), that you listened on an
address the peer can reach, and that the port number matches. Test locally with
`127.0.0.1` first.

**The counters do not move.** Check the state in the Ports pane (Advanced) or on
the setup's page; a port that is *Failed* or *Closed* does not carry traffic. Use **Help → Why is nothing
arriving?**, and in Advanced the **Log** tab, which records state changes and
errors.

**A port failed while running.** The row shows *Failed* and the links to it are
kept. Fix the cause (for example, plug the cable back in) and attach the device
again.

## Command line equivalents

| Task | Command |
|---|---|
| Create one virtual port | `vss_cli virtual --kind=auto` |
| Create a Windows port with the COM number you choose | `vss_cli virtual --kind=windows-driver --com=3` |
| Create a port with a stable name (Linux, macOS) | `vss_cli vport --name=ttyVSS0` |
| Create and hold a topology | `vss_cli run topology.json --seconds=60` |
| Share a device | `vss_cli share serial://COM3 --listeners=3 --seconds=60` |
| Join publishers | `vss_cli join tcp://a:1 tcp://b:2 --target=serial://COM4 --seconds=60` |
| Put ports on a shared bus, with a listen-only tap | `vss_cli bus --virtual=3 --tap=virtual` |
| Bridge two transports | `vss_cli bridge serial://COM3 tcp-listen://0.0.0.0:4001 --seconds=60` |
| Loopback demo | `vss_cli loopback --seconds=30` |
| List devices | `vss_cli devices` (add `--json` for a script) |
| Record traffic to a file | `vss_cli capture topology.json --out bus.cap --port master` |
| Record a port without a topology | `vss_cli capture --uri=serial://COM5 --out run.jsonl --seconds=60` |
| Play a capture into a port | `vss_cli play bus.cap --uri=serial://COM6 --speed=2` |
| Make a port answer like a device | `vss_cli respond device.json --uri=serial://COM7` |
| Measure throughput | `vss_cli bench --from=… --to=…` |
| Self-test the wiring | `vss_cli selftest` |
| Check what the system may block | `vss_cli permissions check` |

`virtual`, `vport`, `bus` and `capture` keep running until `Ctrl-C` or `SIGTERM`,
or for `--seconds=N`. `run`, `share`, `join`, `bridge` and `loopback` keep their
ports only for the `--seconds=N` you give; without it they print what they made
and exit. `--verbose` prints the port table every half second while `run`,
`share`, `join`, `bus`, `bridge`, `loopback` and `vport` wait, and `vss_cli
--help` lists every option.

### Recording with `vss_cli capture`

```bash
vss_cli capture <topology.json | --uri=<uri> ...> --out=<file>
                [--format=cap|csv|jsonl] [--port=<name>[,<name>...]]
                [--seconds=N]
```

`capture` applies the topology (or opens each `--uri`, which may be repeated;
each one is named by its URI), starts recording and keeps the ports open until
you press `Ctrl-C`, it receives `SIGTERM`, or `--seconds` has passed. It does
not wait for a window to refresh, and it does not cut payloads, so every byte
that moves is in the file, with the time the engine saw it. The format is
`--format`, or comes from the extension of `--out` (`.cap`, `.csv`, `.jsonl`),
or is `cap`. Each line is flushed as it is written, so a file is complete up to
the last transfer even if the process is killed. A port whose *Capture
traffic* is off in the topology is not recorded. `--port` names the ports to
keep (see [Capture files](#capture-files) for why you usually want one), and
the command stops with a list of the real names if one is wrong. The file is
readable by its owner only, where the system has such a setting.

### Replaying with `vss_cli play`

```bash
vss_cli play <file> [<topology.json>] (--uri=<uri> | --port=<name>)
             [--speed=X] [--loop] [--direction=R|W] [--from=<name>]
             [--seconds=N]
```

`play` reads a `.cap` or `.jsonl` capture (not `.csv`; record JSON Lines when
you want the port names) and writes the records of one direction into one port,
each at its recorded time. That lets a recorded master or device drive the
other programs on a bus without the program that made the recording.

- The port is what a program would open: give `--uri` for a port of the
  operating system (`serial://COM6`, a pseudo terminal path, `tcp://host:port`),
  or a topology and `--port=<name>` for a port of that topology. What `play`
  writes goes out of that port to whatever is on its far end, so for a port
  Studio made, open the end the program would have opened. A loopback port echoes
  its writes back in as received bytes, which makes it a stand-in for the
  program in a topology of its own.
- **R** records (the default) are the bytes the recorded port received, so they
  replay what the program on that port *sent*. `--direction=W` plays the other
  direction. `--from=<name>` keeps only the records captured on that port, for a
  file that holds several; a `.cap` file has no names, so it needs none.
- `--speed=2` plays twice as fast and `--speed=0.5` half as fast. Times are
  absolute deadlines, so they do not drift over a long file.
- `--loop` starts again after the length of the capture, which is the offset of
  its last record whichever direction that is, so a recording repeats at the pace
  it was taken.
- It stops when the last record has been sent, or at `Ctrl-C`, or after
  `--seconds`, and prints how many records and bytes it sent. Lines of the file
  that cannot be read are skipped and counted.

### Scripted devices with `vss_cli respond`

```bash
vss_cli respond <script.json> [<topology.json>] (--uri=<uri> | --port=<name>)
                [--seconds=N]
```

`respond` makes one port act as a device, or as a master that polls, with no
program behind it. The behaviour is in a script, a JSON file of its own: the
topology file and its schema do not change. The port is chosen as for `play`:
the end a program would open.

```json
{
  "version": 1,
  "seed": 7,
  "rules": [
    { "name": "status", "match": "02 31", "mask": "ff f0",
      "reply": "02 31 41 03", "delay_ms": 150, "jitter_ms": 10 }
  ],
  "poll": {
    "start_ms": 100, "interval_ms": 1000, "cycles": 0,
    "steps": [ { "send": "02 31 03" },
               { "send": "02 32 03", "at_ms": 250, "jitter_ms": 5 } ]
  }
}
```

**Rules** make a device. `match` is the bytes to wait for, in hex. `mask`, when
given, has as many bytes as `match`, and only the bits that are 1 in it are
compared, so `"02 30"` with `"ff f0"` matches `02 30` to `02 3f`. `reply` is
sent after `delay_ms` (default 0), plus or minus a random amount of up to
`jitter_ms`. The delay is counted from the moment the request arrived, not from
when the script got to it, so a reply meant for a 200 ms window is not late by
the time the program took to look.

- Bytes are matched as a stream. A request that arrives in two reads is found,
  and the bytes up to the end of a match are used up. When several rules could
  match, the one that starts first wins, and then the one listed first.
- A reply is written to the port as one piece.

**Poll** makes a master. A cycle starts `start_ms` after the script starts, and
a new one every `interval_ms`; each step is sent `at_ms` after its cycle
started, plus or minus `jitter_ms`. `cycles` is how many cycles to run, and 0
means until you stop it. With no `interval_ms` the list is sent once. A script
may have rules, a poll list, or both.

`seed` makes the jitter repeat from run to run. A key the script does not know
is an error that names it, so a misspelt `delay_ms` is found and not ignored.
Delays are limited to one hour. `respond` runs until `Ctrl-C` or `--seconds`, or,
for a poll-only script with a number of cycles, until the last step has gone,
and prints how many requests it matched and what it sent.

See [ux.md](ux.md) for the design reasoning, [scenarios.md](scenarios.md) for
worked examples and [the screen specifications](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/00-product-brief.md) for the
screen specifications.
