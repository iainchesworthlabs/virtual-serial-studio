# What this build cannot do

A control that the engine cannot back is shown disabled, with the tooltip "Not
available in this build". It never pretends. At the moment:

- **Undo of everything**: Undo and Redo cover connecting a link, removing a
  link, changing an owner and renaming a port, and, on the Advanced diagram,
  adding from the toolbox, duplicating, deleting, **Link selected**, link and
  port settings and reversing a link. They do not cover adding or removing a
  port with the menus and dialogs, a wizard or applying JSON, because a port that
  is made again is a different port. Any of those starts a new history.
- **Reconnection from the start**: a port that stops shows *Failed* or *Closed*
  and keeps its links. `vss_cli run --reconnect` opens a `tcp://` or
  `rfc2217://` client again by itself. The window turns reconnection on only
  when it makes a client that waits (**Create anyway and keep trying**, or a
  saved setup whose server is not there); until then a client that drops stays
  down. **Stop** and then **Start** (in the port's menu on the diagram, or in
  the Inspector) opens a real device or a network port again. The **Recreate
  ports** banner is for Windows driver ports that a restart took away (see
  *When the ports are taken away*).
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
- **A program's RTS on a half-duplex bus**: a bus made a half-duplex line reports
  two programs sending at once and an answer inside the turn-around time, but it
  gates by RTS on loopback ports only, and does not echo a station's own bytes.
  See [RS-485 is half-duplex, and a bus can be](timing-and-control-lines.md#rs-485-is-half-duplex-and-a-bus-can-be).
- A **half-duplex line** in a build whose engine has none: the choice in
  **Connect ports** is disabled with "Not available in this build", and a
  topology file whose bus has `half_duplex` is refused with "Half-duplex buses
  are not available in this build".
- A **shared bus** in a build whose engine has no bus link: the choice in
  **Connect ports** is disabled, and a topology file with a `bus` link is
  refused with "Bus links are not available in this build". The star (one port
  split to several, with a join to a listener) still works.
- **Ports that another program made**: Studio lists only the ports it made
  itself. A Windows driver port belongs to the program that made it.
- **The local address of a network client**: the Inspector says "Not
  reported", because the engine's client does not report it.
- **Plugin fixtures that play a recording**: a tile such as **Recorded device**
  is disabled, with the reason, until the engine can play a recording as a
  device.

## What does not exist yet

These are not built in this version. Pages and the licence table may name some
of them as things a licence adds.

- **Official plugins.** The plugin host, the **Plugins** page and `vss_cli
  plugin run` work, and run any plugin you add, but no official plugin has been
  released. A Pro or Enterprise licence names official plugins, and there are
  none for it to cover.
- **A driver signed by Microsoft.** The Windows driver is not production-signed.
  Windows loads it with test signing on, or after
  `drivers/windows/trust/install-trust.ps1` has made the computer trust the
  certificate it is signed with; the installer says so when Windows does not
  trust it. The installer carries the
  user-mode driver only, not the kernel-mode one.
- **Encrypted remote use.** A `tcp-listen://`, `udp-listen://` or
  `rfc2217-listen://` port carries its data as it is and has no password; see
  [Keeping it safe](network.md#keeping-it-safe). The control gateway listens on
  a local pipe or socket, and on `127.0.0.1` and `::1` only when it is started
  with `--loopback-port`. Nothing in Studio makes a TLS connection for your data.
- **Bindings other than Python.** The client for the control endpoint is a
  Python module (`adapters/python`) and there is no other.
- **Response timing rules, and fault injection from a script.** The rules that
  make a port answer after a distribution of delays are in the library only, and
  neither the window nor `vss_cli` loads one. Faults are injected in the window,
  on a mock fixture or a Windows driver port (see [Mock
  fixture](adding-ports.md#mock-fixture-advanced)), and neither `vss_cli` nor the
  control endpoint can inject one. `vss_cli respond` and a delay on a link are
  built.
- **Features that depend on the licence.** No control of Studio is turned on or
  off by an edition. Studio reads and shows the licence, and the plugin host
  asks whether it covers an official plugin before it starts one.
- **Update checks.** The code to check for an update, and to download and check
  one, is there. A build checks only when a key to check the update information
  with is built in, and no build has one yet: **Help → Check for updates...** is
  disabled with "This build cannot check for updates.", the check is never made
  and nothing is sent. See [Updates](updates.md#a-build-with-no-update-key).
- **The agent and the control gateway in a package.** `vss_agent` and `vss_gate`
  are built from source and started by hand. No installer or package holds
  them, and Studio does not use the agent.

The reasons and the plan for each are in
[gui-design/docs/08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).
