# Adding a port

**Ports → Add port** (`Ctrl+Shift+N`) asks what the new port should be:

- **Virtual port** — a port other programs can open.
- **Linked pair** — two ports wired together like a null-modem cable.
- **Real device** — a serial device plugged into this PC, so you can share it.
- In Advanced, **Network endpoint** and **Mock fixture** cards also appear.
  They do not make anything yet: **Create port** stays off. Use **Ports →
  Network endpoint** and **Ports → Mock fixture** instead (see below).

![The Add a port dialog with five choices and the Virtual port settings below](../images/add-port-light.png)

## Virtual port

- **Name**: a unique suffix is added if the name is taken. With **How many**
  above one, names run `port-1`, `port-2` and so on. The number must be 1 to
  32.
- **COM number** (Windows, with the Studio driver and its helper service, and
  only for *Best available*): *Automatic* takes a number Windows has kept for a
  port that is gone and nothing uses, lowest first (never COM1 or COM2), so
  deleting COM17 and making a port again gives COM17. When there is none, it
  takes a number above the ports this PC already has. Or type the number a
  program expects, 1 to 255. It names one port, so it is off when **How many**
  is above one. A number that is refused is explained under the field and
  nothing is made; see [Choosing a COM number
  (Windows)](windows.md#choosing-a-com-number-windows).
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

## Linked pair

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

![The Connect two programs dialog with Create pair disabled and a warning that there are not enough free driver pairs](../images/linked-pair-light.png)

## Real device

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

![The Attach a real device dialog listing COM3 as available, with the address serial://COM3?baud=115200](../images/attach-device-light.png)

## Network endpoint (Advanced)

**Ports → Network endpoint** (**Put a device on the network**) bridges a real
serial device to a network address, as `vss_cli bridge` does. Choose the
**Device to share**, the transport (TCP, UDP or RFC 2217), then whether Studio
**connects to a server** or **waits for other computers to connect (server)**,
and the address and port. Waiting works for TCP, UDP and RFC 2217. **Start
bridge** links the device and the address both ways. **Test from this PC first**
picks the server role on `127.0.0.1`. **Edit address** lets you type the
address yourself (see [network addresses](network.md#network-addresses)), and the dialog
shows the equivalent `vss_cli bridge` command. To put a network port on a
[shared bus](connecting-ports.md#connecting-ports) rather than bridge a device to it, write the
port in a setup file or in **Topology JSON**; see [A device on another
computer](network.md#a-device-on-another-computer).

![The Put a device on the network dialog with TCP chosen, a server address and the equivalent vss_cli bridge command](../images/network-endpoint-light.png)

## Mock fixture (Advanced)

**Ports → Mock fixture** adds a scripted pretend device for tests without
hardware. Give it a name and a baud rate, and choose a port to link it to
two-way, or *Nothing yet*.

![The Add a mock fixture dialog with a name, baud rate and the port to link it to](../images/mock-fixture-light.png)

A fixture can do two things by itself. In the Inspector (or the port's context
menu) **Echo received bytes** sends every byte written to it straight back. The
**Generator** sends **Text line**, **Fixed pattern** (hex), **Counter** or
**Random bytes** at a chosen rate, until you press **Stop generator**.

Select the fixture and choose **Open line matrix and faults** in the Inspector
(or **Topology → Link wiring**) to inject a fault: a parity error, a framing
error, an overrun, a break, a hang-up or a disconnect. Each one is reported as
an error event on the port, and the fixture then stops and shows *Failed*, as a
real device would after the fault. Add another fixture to inject again. Every
fault you inject is listed in **Injected so far**. Faults work on fixtures and,
on Windows, on a port of the Studio driver. On a driver port all six are raised
at the program that has the port open: the program is told of a parity error, a
framing error, an overrun or a break as it would be on a wire, **Hang-up** drops
DCD and DSR, and **Disconnect** takes the port back from the program. The port
itself keeps running, does not show *Failed*, and Studio shows no error event.
The buttons are off, with the reason, until a program has the driver port open.
For any other port they are off.

The same dialog holds the **line matrix** for the link between the fixture
and the port it is linked to. The rows are the outputs DTR and RTS and the
columns are the inputs DSR, CTS, DCD and RI; tick a box to make that output
drive that input on the other port, or pick a preset. A change applies at once,
including to the inputs the other port already shows. The default is the
null-modem cable (DTR to DSR and DCD, RTS to CTS) and one wiring serves both
directions of the link. The wiring lasts while Studio runs and is not saved in a
setup file. A port whose driver cannot set RI shows it in Studio only.

![The line matrix and faults dialog for a fixture: six Inject buttons, the Injected so far list and the control line state](../images/fixture-faults-light.png)
