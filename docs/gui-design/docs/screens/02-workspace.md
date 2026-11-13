# Screens 02: workspace
Mockups: Workspace (Simple), Advanced, Compact (960 by 640), Dark. Frame and persistence rules are in `03-information-architecture.md`.

## Ports dock (Workspace, Advanced)
Header "Ports" with a count. Selection is shared with the Topology and Inspector.
- Simple: one PortDelegate row per port (3 lines): name (600) with StatusChip at the right; role line (12, ink3); mono path with CopyButton, plus "115200 8N1" for real devices.
- Role text: "Real device, shared with N programs", "Virtual port, linked to <name>", "In-process port. Only Studio can open it", "Fixture (mock)".
- States (see states in `docs/04-components.md`): Open, Studio only, Closed, Failed. "Open" is the normal state of a port that exists. Where the platform reports whether a program has opened it (`Capabilities::peer_open`), Open is refined into Waiting for program and Program connected.
- Advanced: filter field "Filter ports" above a table: Port (name over mono path), State (compact chip), RX, TX, Dropped (right aligned, mono). Dropped text is `bad` when above zero.
- Keyboard: Up and Down select, Enter moves focus to the Inspector, Delete removes (confirm, Undo toast), F2 renames, Ctrl+Shift+C copies the path.
- Below the list, when another program made virtual ports on this machine (`StudioBackend::other_program_ports`, Windows driver ports only): muted note "Another program made COM41. Studio cannot show or watch ports it did not make itself." It is read when the window opens, when the set of Studio's ports changes and every 5 seconds, and hidden when there are none or when `Capabilities::external_ports` is on.
- Context menu (right click, or the Menu key or Shift+F10 on a row): Inspect, then the port actions below, then Copy path. The row under the pointer becomes the selection.

## Port actions (Ports context menu and Topology node menu)
The same group appears in both menus, after Inspect: "Send bytes" (shows the Send dock and puts the cursor in its input, with this port as the target), "Start repeat" or "Stop repeat" (Advanced only; with nothing typed it shows "Type the bytes to repeat, then start the repeat."), "Open in monitor" (selects the port and shows the Monitor). A mock fixture in Advanced mode adds "Start generator" or "Stop generator" and a checkable "Echo received bytes". Echo is disabled with the tooltip "Not available in this build" when `Capabilities::fixture_echo` is false.
- Compact window: rows are two lines (name and mono path, then the chip), 240 wide.

## Inspector
Header "Inspector" and the port name.
- Simple: State (chip), Path (mono with copy), Traffic ("RX 0 B, TX 1.2 kB"). When waiting: "No program has opened COM7 yet. The program has to open exactly COM7; a different port will not receive anything.", a quiet small button "Copy name" (copies the path, toast "COM7 copied.") and the link "Why is nothing arriving?" (opens the troubleshoot dialog). "Control lines" label and six read-only chips in a 3 by 2 grid: DTR, RTS, CTS, DSR, DCD, RI, each "<name> on" (ok, filled) or "<name> off" (hollow).
- Advanced: also Serial ("115200 8N1, no flow control") for real devices. DTR and RTS become Switches ("DTR on", "RTS off"), a "Break" button, input chips CTS, DSR, DCD, RI, and a link "Open line matrix and faults".
- Virtual ports accept the operations and report their own state. Unsupported operations are disabled with a tooltip, never silent.
- Advanced, mock fixture only: a card "What this fixture does" between the line controls and the link. Checkbox "Echo received bytes" (disabled with "Not available in this build" without `Capabilities::fixture_echo`). Combo "Generator kind": Text line (default), Fixed pattern, Counter, Random bytes. Text field "Line" (text, sent with CRLF; placeholder "Text, sent with CRLF") or "Pattern" (hex; placeholder "Hex, such as 55 AA"); "Bytes each time" (1 to 256) for Counter and Random bytes; "Rate" (1 to 50 "per second"). Primary button "Start generator", which reads "Stop generator" while it runs. Status line: "Not running.", "Generating, <size> sent from this fixture." or "Stopped after <size>.". Errors: "The pattern needs pairs of hex digits, such as 55 AA.", "Type the line to send." and "The generator stopped: <reason>" (toast). The generator and the repeat are bound to the fixture they started on, whatever is selected later.

## Send
Header "Send". A line "Sending from: <name> (<path>)" (or "Sending from: no port selected") with a checkbox "Pin" (tooltip "Keep sending from this port when you select another."). Combo "Send from port" (follows the selected port unless pinned; a Send bytes action from a menu overrides the pin), primary "Send", input with placeholder "Type text, or hex if Hex is ticked", checkboxes "Hex" and "Add CRLF" (default on). Advanced adds "Repeat" (interval field appears) and a "Saved sends" combo. A running repeat is bound to the port it started on: selecting another port, or changing the target or the pin, never moves it. While it runs a row shows "Repeating every <n> ms from <name> (<path>)." and a button "Stop repeat". If that port is removed the repeat stops with the toast "The repeat stopped because <name> is gone." Hex input is validated live: invalid hex shows the inline error "Hex needs pairs of digits, such as 48 65 6C 6C 6F." and disables Send. Engine errors appear as a toast and in the Log.

## Monitor
Tabs: "Monitor". Advanced also "Log" and "Topology JSON".
- Toolbar: Pause, Clear, checkboxes Hex and Follow (Follow default on), right aligned "All ports". Space pauses and resumes when the table has focus.
- Advanced controls: port combo (All ports), checkboxes RX, TX, Dropped, search field "Search payload" (Ctrl+F), segmented ASCII or Hex, then right aligned Pause, Clear, Follow, Export (saves the visible rows as text) and Export capture. They sit on the pane's title row, after "Monitor", like the Add port button in the Ports header. When the row is too narrow or the text too large for all of them, they move to a row of their own under the title, never clipped.
- Columns: Time (HH:MM:SS.mmm), Port, Direction (DirectionBadge), Bytes (right aligned), Preview (fixed font). ASCII mode shows `\r` and `\n` as escapes and other non-printables as a dot. Hex mode shows `48 65 6C 6C 6F`.
- DROP rows add the muted `bad` text "not the owner, not forwarded".
- Performance: a bounded ring (default 10,000 rows, a named constant). Updates are batched per refresh interval and inserted on the GUI thread. Never create per-row widgets. Pause stops appending, traffic still flows and is counted.
- Empty state: "No traffic yet. Send some bytes, or open the port in a program."

## Status bar
Left: "Ports 4", "Links 1", "RX 38 B/s", "TX 114 B/s", "Hub running". Advanced adds a warn chip "214 B dropped, not owner" when relevant. Right: readiness chip and the Simple or Advanced segmented switch (24 high). If the canvas has to go back from the graphics card to the processor while Studio runs, the status bar shows "The canvas is drawn on the processor: <reason>" for ten seconds and the Log gets the same line.

## Threading
Engine callbacks run on hub threads. Marshal everything to the GUI thread with queued connections, coalesce by the refresh interval (default 250 ms, range 50 to 5000), and never block the GUI thread on the engine. Subscribers must capture state that outlives the hub, see `docs/architecture.md` in the repo.

## Acceptance
Select a port in any of Ports, Topology or Monitor and the other two follow. The window is usable at 960 by 640 and at 200 percent text. Dark theme has no unreadable text (run the contrast tool and a visual review of the Dark mockup).
