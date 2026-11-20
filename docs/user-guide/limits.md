# What this build cannot do

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

The reasons and the plan for each are in
[gui-design/docs/08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).
