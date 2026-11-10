# Screens 07: the Advanced canvas editor
Mockup: `reference/mockups/CanvasEditor.html`. Extends `03-topology-view.md`, which still holds the node and link drawing, layout, zoom, Fit and resizing rules. This document adds what Advanced mode lets a person build on the canvas. Status: approved on 2026-11-06. Colours, shapes and spacing follow `10-design-language.md`: the canvas is the `sunken` ground with a dot grid in `line2`, a node is a raised `panel` card (radius `card`) with a 32 px kind tile (`accentSoft` for virtual ports and loopbacks, `violetSoft` for fixtures, `okSoft` for real devices), and selection is a 2 px `accent` edge with a soft `accentSoft` halo. Where the mockup's look differs, the design language wins.

Not built yet, and kept here so the next change has them: Add to the split and Add to the join in the link chooser (the engine edits both in place: `hub::add_target` for a split, `hub::add_source` for a join; the chooser does not offer them yet), the compact icon-only toolbox below 500 px, the ghost of a whole bundle while a tile is dragged (a single ghost card shows today), the "Creating..." state for slow driver ports (the Add runs on the GUI thread), and Shift+click range selection (Shift+click adds one item, like Ctrl+click).

## Simple and Advanced divide the work
- **Simple is the wizard.** "I want this set up and go." Simple mode is the setups list and a guided page per job (`08-simple-mode.md`): it asks a few questions and builds a whole setup. It shows no canvas, no toolbox, no port handles and no Settings item.
- **Advanced is the canvas.** Everything Simple has, plus a toolbox of ports and bundles to put on the board, handles to wire nodes by dragging, Settings for ports and links, multi-select, Duplicate, and undo of every change made on the canvas.
- Both modes build with the same backend calls. Switching mode never changes the setup, and a setup made in one mode opens in the other. Toolbox items appear in the command palette in both modes; in Simple they say "Needs Advanced mode", as other Advanced items do.
- An empty setup in Simple mode shows the job grid of `08-simple-mode.md`. In Advanced mode it shows the empty canvas with its toolbox and the text "Drag a port or bundle from the toolbox, or press Enter on one."; File, New and the launcher's own entry points still reach the launcher.
- The flows stay in Advanced too. A toolbox bundle is the quick way; a flow is the guided way, for example Share, which has to find the real device first.

## Layout
The topology dock gains a toolbox rail on its left edge, 76 px wide, with the `sunken` fill and a 1 px `line` edge on its right. It is a vertical list of tiles in two groups, each under a small group label (12 px, `ink3`): **Ports** and **Bundles**. A tile is 64 by 56: an icon from `IconProvider` (20 px) over a one-word label (12 px, `ink2`). The rail scrolls when the dock is short. View, Show toolbox (Ctrl+Shift+B) hides and shows it; the choice is kept in `QSettings`.

Below 500 px of dock width (the compact canvas) the rail shows icons only, 40 px wide, and each tile's label becomes its tooltip and accessible name.

### Ports
| Tile | Adds | Backend call |
|---|---|---|
| Port | A virtual port of the best available kind | `create_auto_port` (or `create_driver_port` with a COM number) |
| Fixture | A mock device | `create_mock_port` |
| Loopback | A port that sends back everything it receives | `create_virtual_port(loopback)` |
| Device | A real serial device on this computer, chosen from a list | `attach_serial` |
| Network | A TCP, UDP or RFC 2217 endpoint, client or server | `attach_serial` with a built URI |

### Bundles
A bundle is a set of ports and the link between them, added in one step. Each end of a bundle is a slot that can be **New port**, **New fixture**, **Loopback** or an **existing port** on the canvas, so "a pair of two fixtures" is the Pair bundle with both slots set to New fixture.

| Tile | What it builds | Slots | Backend call |
|---|---|---|---|
| Pair | Two ports linked both ways, like a null-modem cable | A, B | `connect_ports` |
| Split | One port feeds several; their replies merge back | Source, 2 to 8 targets | `split` |
| Join | Several ports send into one, one way | 2 to 8 sources, target | `join` |
| Bus | Several ports on one shared line; each hears all the others | 2 to 8 members | `bus` (needs `shared_bus`) |
| Switch | Several programs send to one port, one owner at a time | Device, 2 to 8 programs | `join` into the device with `merge_policy::owner`, then `set_owner` to the first program (the owner of a join is one of its sources; on a split or a two-way link it is one of the targets) |
| Redirect | Bytes from one port go to another, one way | From, To | `redirect` |
| Monitor | A passive copy of one port's traffic | Watched, Copy | `monitor` |
| Share | A real device given to several programs | Device (chosen from the list), 1 to 8 program ports | `share_serial` |
| Bench | Several fixtures, each paired with its own port | 1 to 16 pairs | `create_mock_port` and `connect_ports`, repeated |

Anything else ("complex" setups) is built by placing ports and wiring them; there is no separate tile for it.

## Adding from the toolbox
- **Drag** a tile onto the canvas. While it is over the canvas a ghost of the nodes it will make follows the pointer (`accentSoft` fill, dashed `accent` edge, 60 percent opacity), placed as the automatic layout would place the bundle. Dropping opens the **Add sheet** anchored at the drop point. Dropping outside the canvas, or Esc, cancels.
- **Enter** (or Space, or a double-click) on a focused tile opens the same sheet. The nodes go into the first free spot right of the existing content, at the height of the view's centre, using the free-space rule in `03`.
- Dropping a Port, Fixture or Loopback tile **on an existing node** adds the port and opens the link chooser between the new port and that node (see Wiring).

### The Add sheet
A small non-modal popup (`QFrame`, `panel` fill, 1 px `control` edge, 6 px radius, `shadow`), 320 px wide. Contents, top to bottom:
1. Title: "Add pair" (the tile name).
2. One row per slot: a label ("End A"), a kind picker (New port, New fixture, Loopback, or an existing port by name), and a name field filled with the next free name (`port-1`, `fixture-1`, ...). A slot that holds an existing port has no name field.
3. For bundles with a count (Split, Join, Bus, Switch, Share, Bench): "How many" spin box, which adds or removes slot rows.
4. For Port: the kind (Best available and the platform's kinds) and, when `com_numbers` is on, "COM number" (Next free by default).
5. "More settings": a disclosure that shows the port Settings fields (below) for the new ports, defaulting to 115200 8N1, no flow control.
6. Buttons: Cancel, **Add** (default). Enter adds, Esc cancels.

On Add the ghosts become real nodes in a **Creating** state (dashed edge, a small busy indicator, label "Creating..."), because a driver port takes a second or two. Creation runs on a worker thread and the canvas stays usable. When it finishes the new nodes are selected and the view scrolls them into view. If any step fails, the steps already done are undone, the ghosts go, and the sheet reopens with the reason under the slot that failed (for example "COM5 is taken by another device."). One Add is one undo step.

## Wiring
- **Handles.** A node shows two handles, a 10 px circle centred on its left and right edges, when it is hovered, selected or focused, and on every node while a wire is being dragged. The hit area is 24 by 24. The handle is `panel` with a 1.5 px `control` edge, and `accent` when hovered.
- **Drag a wire** from either handle. A 2 px dashed `accent` line follows the pointer from the handle. Nodes it can link to show a drop highlight (`accentSoft` fill, `accent` edge). A node that would close a loop shows a `bad` edge, and the pointer shows "That link would create a loop" as a tooltip. Esc or a right-click cancels.
- **Drop on a node** opens the link chooser, a small menu at the drop point: **Two-way**, **One way, from <a> to <b>**, **Monitor <a> on <b>**. Two-way is first and is the default for Enter. If the target already has a split or a join from the same source, **Add to the split** and **Add to the join** are offered (`08-engine-gaps.md`): a split takes the target with `hub::add_target` and a join takes the source with `hub::add_source`, which the chooser does not use yet.
- **Drop on empty canvas** opens: **New port here**, **New fixture here**, **New loopback here**. The choice adds the port at the drop point and then shows the link chooser.
- **Several at once.** With two or more nodes selected, the node menu has **Link selected**: Two-way (exactly two), Split from <focused>, Join into <focused>, Bus (needs `shared_bus`), Switch on <focused>. This is how the bus, split and join shapes are made by hand.
- Every link made by wiring can also be made from the keyboard (below) and in the Connect ports dialog, so nothing needs a drag (WCAG 2.5.7).

## Selection and hover
- **Hover a node:** the edge becomes `control2` and the handles appear. **Hover a link:** it draws 3 px in `ink2`, and its tooltip names it ("two-way: rx-sim and tx-sim").
- **Select:** click. Ctrl+click adds or removes one node or link. Shift+click adds the range in Tab order. Dragging on empty canvas draws a rubber band (1 px `accent` edge, `accentSoft` fill at 40 percent) and selects every node it touches; with Ctrl it adds. Ctrl+A selects every node and link. Esc clears the selection.
- **Pan:** middle-button drag, or hold Space and drag. (Left-drag on empty canvas is now the rubber band.)
- Every selected node draws the `03` selection (accent edge, 2 px `accentSoft` halo). The keyboard-focused node also gets the focus ring (2 px `focus`, 2 px offset), so selection and focus are told apart. The dock header reads "Topology, 3 selected" while more than one item is selected.
- Dragging any selected node moves the whole selection, and the arrow keys move the whole selection. The settle rule in `03` applies to the group.
- The Inspector shows the focused item. With several selected it shows "3 ports, 1 link selected" and the actions that apply to all: Delete, Duplicate, Link selected.

## The node menu (Advanced)
In this order, with separators where shown. Items that cannot act are disabled with a tooltip that says why.
1. Inspect (Enter)
2. **Settings...** (Alt+Enter)
3. Rename (F2) (needs `rename_port`)
4. **Duplicate** (Ctrl+D)
5. ---
6. Connect from here (Ctrl+L)
7. The port actions from `02-workspace.md`: Send bytes, Start or Stop repeat, Open in monitor, and for a fixture Start or Stop generator, Echo received bytes
8. ---
9. Copy name (Ctrl+C)
10. **Delete** (Delete)

With several nodes selected the menu is: Link selected (submenu), Duplicate, Delete, and the Settings item applies one set of settings to all of them ("Settings for 3 ports...").

## Port Settings
A dialog, "Settings for <name>", 440 px wide. Fields:

| Field | Values | Default |
|---|---|---|
| Baud rate | Editable list: 300 to 921600, and any whole number from 50 to 4000000 | 115200 |
| Data bits | 5, 6, 7, 8 | 8 |
| Parity | None, Odd, Even, Mark, Space | None |
| Stop bits | 1, 1.5, 2 | 1 |
| Flow control | None, Hardware (RTS/CTS), Software (XON/XOFF) | None |
| Read timeout | 0 to 60000 ms, 0 means wait | 0 |
| Write timeout | 0 to 60000 ms, 0 means wait | 0 |
| COM number | Driver ports only, when `com_numbers` is on; the existing Change... flow | |

A one-line summary under the fields updates as they change: "115200 8N1, no flow control". `vss::validate` checks the values; a refusal is shown under the field it names.

What the settings do depends on the kind of port, and the dialog says so in an info note at the top:

| Port kind | Note | Fields | Applying |
|---|---|---|---|
| Real device | "Studio opens the device with these settings." | All | Reopens the device with the new settings |
| Fixture | "The fixture behaves as a device set to these values." | All but the timeouts | Restarts the fixture |
| Virtual port (driver, pseudo terminal, named pipe) | "A program that opens this port sets its own speed. These are the values saved with the setup." When `line_format` knows it: "Program set 19200 7E2." | Baud, data bits, parity, stop bits, flow control | Saves the values; the port does not restart |
| Loopback | "A loopback has no line speed. These values are saved with the setup." | Same as a virtual port | Saves the values |
| Network (TCP, UDP) | "A network port has no line settings." | None | Settings is disabled in the menu with that tooltip |
| RFC 2217 client | "Studio asks the remote device to use these settings." | Baud, data bits, parity, stop bits, flow control | Reopens the connection |

When applying restarts the port, the dialog's primary button reads "Apply and restart" and a warn note says "Restarting the port drops bytes that are in flight. Programs that have it open keep their handle." Otherwise the button reads "Apply". A setting change is one undo step.

## The link menu (Advanced)
1. Inspect (Enter)
2. **Settings...** (Alt+Enter): Delay (0 to `max_link_delay_ms`), Jitter (0 to the delay), If two write at once (Mix them, One owner at a time), Emulate modem lines (two-way links), and the existing Line wiring when `line_matrix` is on. Applying changes the link in place with `set_link_options`, so it keeps its id, ports, counters and, unless the policy leaves One owner at a time, its owner. Bytes the link still holds for its delay are discarded when the delay changes. A bus offers Delay and Jitter only.
3. Reverse direction (one-way and redirect links)
4. ---
5. **Delete** (Delete)

## Duplicate
Duplicate (Ctrl+D) makes a copy of each selected port with the same kind and settings, named with the next free number (`fixture-2` from `fixture-1`, `rx-sim-copy` when the name has no number), 24 px right and down of the original, unselecting the originals and selecting the copies. A driver port gets the next free COM number. A real device or a network client cannot be duplicated (only one program can open it); the item is disabled with "This port can only be opened once." Links between two selected ports are copied as well, so a duplicated pair is a second working pair. Links to ports outside the selection are not copied.

## Delete
Delete removes the selection. One port with no links goes at once. Otherwise a confirmation names what goes: "Delete rx-sim, tx-sim and the link between them?" with Cancel and Delete. The Simple-mode "Remove only <port>" and "Remove the whole setup" choice from `03-information-architecture.md` stays for Remove setup; on the canvas, Delete removes exactly what is selected and the links that end on it. One Delete is one undo step.

## Undo and redo
Ctrl+Z undoes and Ctrl+Y (Ctrl+Shift+Z on macOS) redoes. Edit, Undo shows what it will undo: "Undo Add pair", "Undo Delete rx-sim", "Undo Settings for tx-sim", "Undo Move 3 ports". Covered: Add from the toolbox, wiring, link settings, Duplicate, Delete, Rename, port Settings, moving nodes and Tidy layout, and owner changes.

Deleting a port is undone by making it again with the same name, kind, settings, COM number and links. For a pseudo terminal or a named pipe the new port may get a different path; the status bar says so: "Recreated rx-sim as /dev/pts/9 (it was /dev/pts/7)." A real device is reopened by its path; if it is gone, the undo fails and says why, and the history stops there. This changes the rule in `08-engine-gaps.md` that history is cleared when the set of ports changes; see the gap table below.

## Keyboard
| Key | Does |
|---|---|
| F6 / Shift+F6 | Move between the toolbox, the canvas and the other docks |
| Up, Down (toolbox) | Move between tiles |
| Enter, Space (toolbox) | Open the Add sheet for the tile at a free spot |
| Tab, Shift+Tab (canvas) | Nodes, then links, as in `03` |
| Enter | Inspect |
| Alt+Enter | Settings |
| F2 | Rename |
| Ctrl+D | Duplicate |
| Ctrl+L | Connect from here: the canvas enters wiring mode; Tab or the arrow keys pick the target (the same highlights as a drag), Enter opens the link chooser, Esc cancels |
| Delete | Delete the selection |
| Ctrl+A, Esc | Select all, clear the selection |
| Shift+Space | Add or remove the focused item from the selection |
| Arrow keys, Shift+arrow | Move the selection 8 px, 32 px |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Shift+F10, menu key | The menu for the focused item, or the canvas menu |
| Ctrl+0, Ctrl+plus, Ctrl+minus | Fit, zoom in, zoom out |

The canvas menu (empty canvas) gains, before Tidy layout and Fit: Add port, Add fixture, Add loopback, Add bundle (submenu of the bundle tiles) and Select all. There is no Paste: ports have no clipboard format.

## Accessibility
- The toolbox is a list (`QListView`, role List) with the accessible name "Toolbox". Each tile's accessible name is its label and its description is the "What it builds" text, for example "Pair: two ports linked both ways, like a null-modem cable".
- The Add sheet, the link chooser and Settings are ordinary widgets with labelled fields, reachable by Tab, and they return focus to the canvas item they came from.
- The view's accessible description (the `03` summary) adds the selection: "4 ports, 1 link. 2 selected: rx-sim, tx-sim."
- Ghosts, handles and drop highlights are visual aids only; each action they offer has a menu and a keyboard path.
- Targets are at least 24 px: tiles are 64 by 56, handles 24 by 24 hit areas, menu rows follow `04-components.md`.
- Every colour above is a token, so both themes work without extra work. At 200 percent text the tiles grow to fit their labels and the rail widens up to 120 px; past that the labels wrap.

## Copy
Every string in this document is the copy, wrapped in `tr()`. New strings: "Toolbox", "Ports", "Bundles", the tile names and their "What it builds" text, "Add <tile>", "End A", "End B", "Source", "Target", "Targets", "Sources", "Members", "Device", "Programs", "From", "To", "Watched", "Copy", "How many", "More settings", "New port", "New fixture", "Loopback", "Creating...", "Two-way", "One way, from %1 to %2", "Monitor %1 on %2", "Add to the split", "Add to the join", "New port here", "New fixture here", "New loopback here", "Link selected", "Split from %1", "Join into %1", "Switch on %1", "Settings...", "Settings for %1", "Settings for %n ports...", "Duplicate", "Delete", "Apply", "Apply and restart", the per-kind notes, "Restarting the port drops bytes that are in flight. Programs that have it open keep their handle.", "This port can only be opened once.", "Delete %1?", "Recreated %1 as %2 (it was %3).", "Topology, %n selected", "Reverse direction".

## Engine support and gaps
Checked against `libs/vss_core`, `libs/vss_studio` and the GUI backend on 2026-11-06.

| Need | Status | Plan |
|---|---|---|
| Pair, Split, Join, Redirect, Monitor | Provided (`hub::connect`, `split`, `join`, `redirect`, `monitor`) | Use directly. |
| Bus ("several ports on one line") | Provided (`hub::bus`), `Capabilities::shared_bus` | Without the flag the Bus tile is disabled with "Not available in this build". |
| Switch (one owner at a time) | Provided (`merge_policy::owner`, `set_owner`, `release_owner`) | Use directly; "Make owner" stays in the Inspector and the node menu of a switched port. |
| Share | Provided (`studio::share_serial` with rollback) | Use directly. |
| Loopback | Provided (`virtual_port_kind::loopback`: an in-memory pipe that sends back what it receives, lines modelled) | Use directly. |
| Fixture, several at once (Bench) | Provided (`create_mock_port`) | Bench is plain repeated calls, rolled back on failure, in the GUI. |
| Add a target to an existing split or join | Not in the engine (targets are fixed) | Compose with `backend/share_edit`, as `08` already describes. |
| **Change a port's serial settings after it is made** | **Not provided.** `endpoint_options` (with `serial`) is fixed when the endpoint is made and `vss_core` has no setter; `endpoint::reopen` takes a fresh transport, and `studio::reopen` reopens with the old settings | New `Capabilities::port_settings`. Proposed: a thin `vss::studio::reconfigure_port(id, settings)` that updates the studio's record and, for a real device, fixture or RFC 2217 client, reopens it with a fresh transport through the existing `endpoint::reopen`; for a virtual port or loopback it updates the record only (the saved values). `port_view::serial` is then read from the studio's record, because `endpoint_info::serial` in `vss_core` keeps the value the endpoint was made with. No `vss_core` change. Without the flag, Settings opens read-only with "Not available in this build" on the Apply button. |
| Change a link's delay, jitter, policy or modem lines | Not in the engine for an existing link (only `set_link_wiring`) | Remake the link (disconnect, then the same call with new options). The link id changes; the selection follows the new link. The topology JSON already stores these fields. |
| Undo across port creation and removal | `08` clears the history when the set of ports changes | Proposed: keep the history, with removal undone by making the port again (name, kind, settings, COM number, links) and the status bar naming a changed path. Needs the owner's agreement, because it is a recreation, not an exact inverse. |
| Node positions | Not in the JSON | Unchanged: session only. |
| Pacing traffic at the port's baud rate | Not in the engine (the hub forwards at once; the baud rate is only a label for virtual ports) | Not offered. The Settings note for virtual ports says the program sets its own speed. A future engine option gets a capability flag and a disabled "Pace at this baud rate" box. |

## Acceptance
- From an empty canvas, using only the canvas: add a Pair with both ends set to New fixture, see two fixture nodes linked two-way, send bytes from one and see them arrive on the other.
- Right-click a port, choose Settings, change 115200 8N1 to 9600 7E1, apply, and see the Inspector and the saved setup show 9600 7E1.
- The same two tasks with the keyboard alone.
- Wire two existing ports by dragging from a handle and choosing Two-way.
- Select three nodes with the rubber band, delete them, undo, and see them and their links back.
- Duplicate a pair and get a second working pair.
- Every toolbox tile is disabled with a reason when its capability is missing.
