# 03 Information architecture

## Window frame (identical in Simple and Advanced)
One 48 high header row (the menus, then the action search and the Simple and Advanced switch on the right), the body, and a 32 high status line. Body (8px gutter, 8px gaps on the window ground): a top row of three rounded panes, then the Monitor pane below. Each pane has its own header in place of a dock title bar; see `10-design-language.md`.
- Ports pane left: 300 wide, minimum 240.
- Topology pane centre: flexible. Its tool row (Attach real device, Connect ports, the Advanced tools, Remove port, Remove setup) heads the pane. Add port is in the Ports pane header. New, Open and Save are in File.
- Right column 320 wide, minimum 260: Inspector above Send (Send is 162 high). At the 960 by 640 minimum window the right column becomes one tabbed dock (Inspector, Send) 260 wide, the tool row becomes icon only with a More overflow, and the Monitor is 156 high.
- Monitor pane bottom: 196 high, minimum 160.
- Default window 1280 by 800, minimum 960 by 640. Dock sizes persist through `saveState`.

## Simple mode body
In Simple mode the body is not the dock grid. It is the setups list and a page for the chosen job or setup, specified in `screens/08-simple-mode.md`; the job grid there replaces the launcher. View, Show launcher opens the job grid. The dock grid and the canvas are Advanced mode. Menu bar, toolbar and status bar are always visible.

## Launcher (Advanced mode)
In Advanced mode an empty setup shows the empty canvas with its toolbox (`screens/07-canvas-editor.md`), not the launcher. The launcher is shown in place of the dock grid from View, Show launcher. Contents: readiness strip, heading "What do you want to do?", four job cards, a Saved setups row with the most recent file, and an Advanced mode hint. The header row and status line are always visible.

## Menu structure (same for both levels)
Items marked **A** are visible only in Advanced mode and hidden in Simple. **new** means it does not exist in the current GUI. Ctrl is Cmd on macOS.
| Menu | Items |
|---|---|
| File | New (Ctrl+N), Open (Ctrl+O), Open recent *(new)*, Setups and templates *(new)*, Save (Ctrl+S), Save as (Ctrl+Shift+S), Export as command **A** *(new)*, Diagnostic bundle *(new)*, Quit (Ctrl+Q) |
| Edit | Undo (Ctrl+Z) *(new)*, Redo (Ctrl+Y, Cmd+Shift+Z) *(new)*, Rename port (F2, Return on macOS) *(new)*, Remove selected (Delete), Remove setup |
| Ports | Add port (Ctrl+Shift+N) *(new)*, Virtual port, Linked pair *(new)*, Attach real device, Network endpoint **A** *(new)*, Mock fixture **A**, Copy port name (Ctrl+Shift+C) *(new)*, Remove port, Remove setup |
| Topology | Connect ports (Ctrl+L), Share a device *(new)*, Watch traffic *(new)*, Add a program port, Remove link, Link wiring **A** *(new)*, Owner **A** *(new)*, Fit (Ctrl+0) **A** |
| Tools | Search actions (Ctrl+K) *(new)*, Setup check *(new)*, Benchmark **A** *(new)*, Refresh now (F5), Settings (Ctrl+,) |
| View | Advanced mode (Ctrl+Shift+A) *(new shortcut)*, Monitor, Log **A**, Topology JSON **A**, Theme, Density *(new)*, Reset layout *(new)*, Show launcher *(new)* |
| Help | Help contents (F1) *(new)*, Why is nothing arriving? *(new)*, Keyboard shortcuts *(new)*, Copy diagnostic information, About |

The old Scenarios menu is retired. Share and bridge live under Topology and Ports, and the null-modem demo becomes a template. Menu items hidden in Simple still appear in the command palette with a "Needs Advanced mode" prompt.

## Toolbar
Simple: New, Open, Save, separator, Add port, Attach device, Connect, Remove port, Remove setup, spacer, Search actions field (Ctrl+K). Advanced adds after Remove: separator, Mock device, Share, Fit. Buttons show icon and text. Remove and Connect are disabled when they cannot act.

## Status bar
Left: Ports N, Links N, RX rate, TX rate, Hub state. Advanced also shows a warning chip when bytes were dropped. Right: setup readiness chip (Setup ready, or Setup needs attention linking to the setup check) and the Simple or Advanced segmented switch. Mode is global, never per document.

## Mode switching
Stored in settings and applied immediately. Advanced mode additions by area:
| Area | Simple | Advanced adds |
|---|---|---|
| Toolbar | as above | Mock device, Share, Fit |
| Ports dock | name, role, state, path with copy | RX, TX, Dropped columns, filter |
| Topology | boxes and lines | link bar (policy, owner, Release, Give to), Fit |
| Inspector | state, path, traffic, control lines read-only | switches for DTR and RTS, Break, link to line wiring and faults |
| Send | text or hex, Add CRLF | saved sends, Repeat |
| Monitor | Pause, Clear, Hex, Follow | filter by port and direction, search, ASCII or Hex segmented, Export, Log tab, Topology JSON tab |
| Dialogs | guided with defaults | editable address and command, More options sections |

## Persistence (QSettings keys, suggested)
`ui/advanced` bool, `ui/theme` (system, light, dark), `ui/density` (comfortable, compact), `ui/linkStyle` (curved, elbow), `ui/refreshMs` int, `ui/defaultBaud` int, `ui/askOnDiscard` bool, `ui/closeAction` (background, quit), `ui/backgroundNoticeShown` bool, `ui/windowState`, `ui/geometry`, `ui/recentFiles` list. The title bar shows `*` while there are unsaved changes.

## Removing things
Three levels, all in Simple mode. **Remove link** removes one link. **Remove port** (Delete) removes the selected port. **Remove setup** closes everything that was built together: the ports joined by links, directly or through other ports. That is a linked pair, a shared device with all its program ports, or an attached device with its virtual port. It is on the toolbar, the Edit, Ports and Topology menus and in the command palette. Delete on a port that belongs to a setup asks which one is meant, with the buttons Cancel, "Remove only <port>" and "Remove the whole setup". A port with no links gets the plain "Remove port" confirmation.

The setup confirmation names what closes: title "Remove this setup?", body "This closes 2 ports and 1 link: COM7 (alpha) and COM8 (beta). A program that has one of these ports open loses it. Nothing is saved until you save the setup." Buttons "Remove setup" (destructive, never the default) and Cancel.

Removing one program port of a shared device keeps the other programs shared: Studio takes the port off the link with `hub::remove_target` and then removes it, so the link, its id and the other programs are not disturbed (see `08-engine-gaps.md`). **Add a program port** (Topology menu, command palette) adds one more program port to the share or attached device that holds the selection, and the share wizard's last step has "Add another program" for the same thing.
