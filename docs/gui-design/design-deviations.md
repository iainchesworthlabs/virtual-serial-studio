# Design deviations and recorded decisions

The design bundle is the contract. Where the bundle is silent, or where its
wording conflicts with the repository, the choice and its reason are recorded
here. Entries name the work package that owns the decision.

## WP0

### Deviations file path

`docs/09-qa-and-definition-of-done.md` says `docs/design-deviations.md`;
`AGENTS.md`, WP0 and WP9 say `docs/gui-design/design-deviations.md`. This file
uses the bundle path. The QA reference is treated as a typo.

### GUI logic tests use a Qt-aware runner

WP1's prompt says to test with the in-tree framework. The in-tree framework in
`tests/framework` is deliberately Qt-free and has no `QCoreApplication`. The
backend has signals and a `QTimer`, and widgets need a `QApplication`, so both
test executables reuse the in-tree `TEST_CASE` and `REQUIRE` macros with a
small Qt-aware runner (`vss_gui_backend_tests`, `vss_gui_theme_tests`). The
whole-window walk stays in `vss_gui --selftest`. No new third-party test
framework is added, so the intent of the rule is met.

### No per-write callback from the engine thread

The design says failures must be visible locally. A per-write completion
callback would have to touch a `QObject` from the endpoint strand and risks a
use-after-free when the backend is destroyed with a write pending. The adapter
therefore returns an immediate error for a missing or closed port, queues the
write, and surfaces a queue-full rejection as the engine's error event through
the coalesced batch. The Send panel can show "queued" and the event; it does
not need a per-write signal.

### Integration branch

Work packages merge into `gui/integration`, not `develop`, until WP9. The
design replaces the shell and leaves placeholders between WP2 and WP8, so
merging each package to trunk would make `develop` unusable for the duration.
This is a delivery decision, not a change to the design.

### Qt version pin for builds

The machine has Qt 6.10.3 under `C:\Qt` and Qt 6.12 under
`C:\Repos\common\qt`. A `QT_ROOT` value with a trailing space makes CMake miss
one and silently select the other; the binary then imports a version tag that
the runtime does not export (`qt_version_tag_6_12` against 6.10). All builds
pin `QT_ROOT` without a trailing space and also pass
`-DCMAKE_PREFIX_PATH`.

WP0 used 6.10.3. WP1 needs the approved `Qt6::Svg` module, which the 6.10.3
installation does not have; the 6.12 installation does. GUI builds therefore
pin 6.12.0 from `C:\Repos\common\qt\6.12.0\msvc2022_64`. This is an
environment decision, not a product dependency change: `Qt6::Svg` is the
dependency the bundle already approved.

## WP1

### Offscreen gallery and the system font

The offscreen platform on Windows has no native font database, and the
project forbids bundling a font. `vss_gui_theme_tests` points
`QT_QPA_FONTDIR` at `C:/Windows/Fonts` so the review PNGs show real text
instead of missing-glyph boxes. This is test-only; the product continues to
use the system UI font through `QFontDatabase`, and the test adds no font to
the repository or the build output.

### SegmentedControl uses QRadioButton

The segmented control is a group of exclusive `QRadioButton`s with the
indicator hidden by QSS. That gives screen readers radio-group semantics and
arrow-key behaviour without inventing an accessible interface. The selected
segment is still an accent fill with `onAccent` text, as the design specifies.

### CopyButton paints itself

A 24 px `QToolButton` inherited the 10 px toolbar padding and clipped the
16 px copy glyph. `CopyButton` is now a custom-painted `QAbstractButton` with
the same accessible name, tooltip, focus ring and 24 px target.

## WP2

### The old shell stays compiled until WP9

`main.cpp` now constructs `ShellWindow` from `src/shell/`, but the flat
`main_window.cpp`, the old panels and dialogs, `theme.cpp`, and `selftest.cpp`
are still in the executable's source list and still compile. They are not
referenced by `main.cpp`. Deleting 24 files in the same commit as the first
working frame would have made the WP2 diff much harder to review and risked a
link surprise while the panes were still placeholders. WP9 owns the deletion,
the compatibility `theme.hpp` removal and the whole-tree literal scan. The
new shell does not link to or instantiate the old `MainWindow`.

### Placeholder handlers show a message

Actions and launcher cards whose dialog or pane belongs to WP3 to WP8 are
registered, have their final menu/palette/toolbar placement and level, and
show a status-bar sentence that names the work package boundary. They do not
simulate a result and do not disable the rest of the frame. The handlers are
the only temporary part; WP3 to WP8 replace them in place.

### Unsaved-changes prompt uses QMessageBox

`ConfirmDialog` has two buttons. Discarding a setup needs Save, Don't save and
Cancel, so WP2 uses a `QMessageBox` with the spec's title, body and button
labels and honours `ui/askOnDiscard`. WP8 replaces it with the shared dialog
and the Settings toggle. The decision protects user data in the interim.

### Compact layout at 960x640

The design says that at the 960x640 minimum the right column becomes one
tabbed dock and the toolbar drops to icons. The implementation switches at
width < 1100 px or height < 700 px, so a 960-wide window is already compact,
the toolbar is icon-only, Inspector and Send are tabbed, and Monitor starts at
156 px. Qt's toolbar extension handles overflow; there is no hand-built More
menu. The shell requests the token sizes for the ports, inspector and monitor
docks. With the current one-line placeholder panes, QMainWindow's size hints
settle the split nearer the minimum; WP3 and WP4 set the final pane content and
the reviewed 360/340/196 split. The minimum sizes are already the token values.

### Menu and icon colours after a theme switch

The bundle's QSS template leaves `QMenuBar` and `QMenu` text colour to the
palette. On Windows after a runtime switch to dark that rendered the menu text
dark-on-dark. The product template now binds those three selectors to the
`ink` token. Action icons had the same problem because `QAction` stores the
rendered `QIcon`; `ActionRegistry` flushes `IconProvider` and rebuilds every
registered icon on `Theme::themeChanged`. No token value changed.

### Shell library name

The plan proposed one `vss_gui_ui` library. WP2 used `vss_gui_shell`, which
links the widget, backend and theme libraries and is what the shell tests
link with `FakeStudioBackend`. The old executable retains its flat sources.
The dependency direction matches the plan; only the target name and the
deletion timing differ.

### Toolbar Share is deferred to WP6

Share stays in the Topology menu in Simple, matching the IA. The design's
Advanced toolbar adds Share, but WP2 has no share dialog; WP6 owns that flow
and can add the toolbar-only advanced action (or change the toolbar policy)
without making the menu action Advanced. Mock and Fit are present in Advanced
so the mode difference is already visible.

### Review-only screenshot switches

`VSS_SCREENSHOT_EMPTY=1` renders the launcher instead of the sample topology,
`VSS_SCREENSHOT_THEME=light|dark` selects the theme, `VSS_SCREENSHOT_ADVANCED=1`
selects the mode and `VSS_SCREENSHOT_COMPACT=1` renders 960 by 640. They exist
only on the documentation `--screenshot` path; normal startup ignores them. The
advanced switch writes `ui/advanced` only on this review path so the image is
repeatable regardless of the user's saved mode. The default screenshots use two
loopback ports, one link, one mock port and a short write so the frame shows
real data shapes.

## WP3

### Legacy flat sources leave the executable now

WP2 kept the old `main_window`, panels, dialogs and legacy `theme.cpp` in the
`vss_gui` executable so the first shell build could land without a 24-file
deletion. WP3's new panes reuse the old class names (`PortModel`,
`MonitorModel`, `InspectorPanel`, `SendPanel`), so compiling both sets produced
duplicate symbols. The executable now builds only `main.cpp`; the legacy files
remain on disk but are not compiled. WP9 owns deleting them and broadening the
no-hex scan to the tree. This is an earlier build-list cleanup than the WP2
plan, forced by the name collision.

### SelectionService lives in panes

The one selected port/link moved from `src/shell/selection_service.hpp` to
`src/panes/selection_service.hpp`. Ports and Monitor need its metaobject, and a
header-only Q_OBJECT in the shell library would have forced the pane tests to
link the whole shell. The shell links panes, so both the frame and the data
panes share the one service without a circular target.

### Default dock minimums are the reviewed defaults during WP3

The frame sets Ports to 360, Inspector to 340 and Monitor to 196 at normal
width, with the compact minima 240/260/156. Qt's placeholder size hints kept
settling near the minima, so WP3 uses the reviewed defaults as the current
minimums to make the real panes and screenshots match the spec. This is a
temporary layout constraint; WP9 can relax it once the panes own their content
sizes and the splitter is re-checked at 200 percent text.

### Monitor tabs and the Log view

The Monitor dock title already names the dock, so Simple hides the one-tab tab
bar instead of drawing "Monitor" twice. Advanced restores the bar and adds Log
and Topology JSON. Log is a read-only `QPlainTextEdit` capped at 10,000 blocks,
fed from the coalesced event batch. It is not a model/view pair because it has
no per-row actions; this keeps it outside the no-per-row-widget rule and
preserves the text selection users expect in a log.

### DROP rows aggregate the real link counter

The engine increments `link_stats.dropped` but does not publish a per-drop
event with a payload or direction. Monitor therefore adds a DROP row when a
link's dropped counter grows, labelled "not the owner, not forwarded", with the
source/target name and the delta. It never invents a payload. The row is
filtered like any dropped direction.

### Advanced Send and Monitor heights

The spec gives Send 162 and Monitor 196. WP3 keeps those defaults in Simple.
Advanced Send adds the saved-sends row and Repeat, and Advanced Monitor adds the
tab bar and filters, so both grow by their content height (about 230) rather
than clipping controls. Compact still uses the spec's 156 Monitor. WP9 owns the
final compact/200 percent pass.

### Peer-open honesty

`EngineStudioBackend` reports `peer_state::unknown` for every port it cannot
observe, so those ports show "Open" and Inspector omits the "No program has
opened ..." sentence. The one case it can observe is a pseudo terminal on
Linux: `vss::peer_open()` in `vss_ports` reads `/proc/<pid>/fd` of the other
processes it may inspect and looks for a descriptor that points at the slave
device. `studio::peer_states()` asks for every pseudo terminal in one pass, and
the backend maps the answer to `peer_state` and sets `Capabilities::peer_open`
where the platform supports it. A port the platform cannot observe has no
entry, which reads as unknown and never as "not open".

Three limits are stated rather than hidden. The scan sees only processes whose
descriptors this process may read, so a program run by another user, or a
privileged one, can be missed; a pseudo terminal slave belongs to the user who
made it, so the programs that open it are almost always that user's. Opening a
pseudo terminal raises no engine event, so the backend looks again once a
second with a timer, and the Ports list can lag a program by up to that long.
macOS and Windows return unknown: macOS has `libproc` for this but it is not
built here, and the Windows driver would need a call to say whether a handle is
open. "Waiting for program" and "Program connected" are also exercised by the
fake in tests. No waiting text is shown for Unknown. The design says the same:
Open is the normal state, and the two refinements belong to
`Capabilities::peer_open`.

### F2 rename

WP3 left F2 out because `capabilities::rename_port` was false. It is true for
the engine backend now (see "Rename" under WP4), so F2 and **Ports → Rename
port** are registered. A backend without the capability hides the menu item,
leaves F2 inert and keeps the action disabled in the command palette.

## WP4

### The topology module is `vss::gui::topology`

The legacy flat `apps/vss_gui/topology_view.cpp` was still compiled when WP4
branched, and it already defines `vss::gui::TopologyView`; linking the new
module under that name produced duplicate symbols. WP3 later removed the legacy
file from the executable, but the new module keeps the nested namespace
(`TopologyView`, `NodeItem`, `LinkItem`, `LinksList`, layout helpers) and
`vss::gui::topology_text` for its copy helpers. No product behaviour depends on
the namespace; WP9 can flatten it when it deletes the remaining legacy files.

### Rename

`hub::rename_endpoint` changed only the hub's name table. An endpoint kept a
`const` name, so events, traffic and `endpoint::info()` went on reporting the
old one, and the studio's records, a saved snapshot and a pseudo terminal's
stable link all kept it too. That is why Rename was hidden.

Rename is built in three layers. `vss_core`: an endpoint's name is now guarded
by a mutex, `endpoint::name()` returns a copy, and the hub sets it when it
renames. This is the one change to `libs/vss_core` in the capability work; it
fixes the hub's own rename and changes no other behaviour. `vss_studio`:
`studio::rename_port` renames in the hub and in the studio's records, so
`snapshot()` and the links in it use the new name, and for a pseudo terminal it
makes the stable link again under the new name before removing the old one, so
a refused name leaves the old link in place. Every other kind keeps the path it
was created with, because a loopback, named pipe or driver path is fixed when
the port is made. The GUI: `Capabilities::rename_port` is true for the engine
backend, and the topology node menu, F2 and **Ports → Rename port** open the
Rename dialog. Rows already written to the traffic log keep the name they had
when they were written.

### Remove has confirmation and a limited Undo

Remove link and Remove port confirm the destructive action and say nothing is
saved until the setup is saved. Remove link can then be reversed with Undo
(see "Undo and Redo reverse changes, not snapshots" under WP8). Remove port
cannot, because a port that is made again is a different port. No Undo toast
is offered for either; the confirmation stays.

### Nodes are nudged with the keyboard, not dragged

The design forbids requiring a drag (WCAG 2.5.7) and the acceptance asks for
keyboard-only operation. Arrow keys move the focused node by 8 px, Shift by
32 px, and the offset is kept per port for the session. Pointer dragging is
not implemented, so a click can never move a node by accident. Offsets are
never written to the JSON snapshot.

### Node sizes are minima, not fixed boxes

168 by 56, 168 by 64 for a real device or the target of a join, and 120 by 48
in compact mode are the design sizes. `node_size_for()` grows the box when the
name, detail line and badges need more room, so 200 percent text cannot clip
(WCAG 1.4.4). At the default system font the boxes are the design sizes; the
test asserts that and the doubled-font case.

### Compact switches at 500 px of topology width

The design says compact sizes apply at the 960 by 640 minimum window. The
actual central width there is about 430 px after the Ports and right column
docks, so the view switches to the 120 by 48 nodes below 500 px of its own
width. This keeps the default 1280 by 800 window on the comfortable sizes.

### Unlinked ports get their own column

The design asks for a deterministic automatic layout and gives example
positions but not a rule for unlinked ports. Putting them in the same left
column as a split source pushed that source off the vertical centre, so an
unlinked port now sits in a final column after the graph. Split and join
sources and their target group stay centred on each other.

### Monitor links keep their arrow head

`03-topology-view.md` says monitor links are dashed and that one-way and
redirect links end in a filled arrow head. A monitor link is one-way in the
engine, so it is drawn dashed with an arrow head; split and two-way links have
no heads.

### Loop feedback lives in the topology

`06-support-and-system.md` asks for a modal refusal with Close and "Show the
loop". That dialog belongs to WP6's Connect ports flow. WP4 provides the
topology half: `show_loop_note()` finds the cycle, highlights its links in
`bad`, and shows the exact danger note in a `NoteBanner` with a "Show the loop"
action that frames the cycle. The Connect dialog will call it; Escape
dismisses the note in the meantime.

### Per-item QAccessibleInterface is not implemented

`03-topology-view.md` says to give each node a `QAccessibleInterface` "if
practical". QGraphicsItem is not a QObject and a custom interface would
duplicate the accessible model that WP3's Ports dock and WP4's Links list
already provide. The view instead sets the name "Topology", generates the
specified description ("4 ports, 1 link: split from device to terminal,
test-tool and logger") and updates it with a DescriptionChanged event; the
Links list exposes every link with its names and state to the keyboard and to
screen readers.

### Links list placement

"Show links as a list" is a View menu item in Simple mode. The list is a dock
tabbed with the Monitor and hidden by default, because the Ports dock is WP3
and the design does not name a home for it. Turning the action on shows the
dock; choosing a row selects the link through `SelectionService`.

### The topology auto-fits until the user zooms

Fit is an Advanced command (Ctrl+0), but the first view and every resize fit
the content until the user zooms or pans, so a fresh workspace never opens
with the graph off-screen. `fit_content()` restores that automatic behaviour.

### Single selection between ports and links

Selecting a node clears the selected link and vice versa, matching the
"Click a box ... Click a line ..." copy and the old at-most-one contract.
`SelectionService` still carries both ids; the topology keeps one non-zero.

### Owner and Dropped badges are read-only here

WP7 owns the owner control (the Advanced link bar) and the owner flow. WP4
paints the Owner, Dropped and "not linked" badges from the snapshot so the
nodes match the Advanced mockup, and its Owner action is a placeholder naming
WP7. Nothing in the topology changes ownership.

### Placeholders name their package

Connect ports, Share a device and Watch traffic say they arrive with WP6
(connect and share). Link wiring and Owner say WP7 (Advanced tools). Fit,
Remove link, Copy name and the Links list are implemented here. Each
placeholder is a status-bar sentence, never a fabricated result.

## WP7

### Fault injection

WP7 built the six fault tiles, their buttons and the history table disabled
with "Not available in this build", because the endpoint keeps its transport
private and `vss_studio` had no accessor.

That follow-up is done. `vss::studio::inject_fault(endpoint_id, fault_kind)` is
the thin forwarding method (nothing in `libs/vss_core` changed). The studio
remembers the fixture behind each mock port and posts the injection to the
endpoint's strand, so the call is safe from the GUI thread and runs where the
endpoint reads. `EngineStudioBackend` sets `Capabilities::faults` and forwards
to it, and `FakeStudioBackend` stands in for tests.

The tiles are enabled only for a fixture that is running. The engine stops an
endpoint on a read failure, so after any fault the fixture shows *Failed* and
the tiles go off with "The fixture is not running."; for any other port they say
"Faults are only available on fixture ports." The history row reads "Error event
reported on <port>" because the call has succeeded when the fault is queued and
the engine emits the error event on the fixture's next read. A failure from the
call shows its message in that row instead.

A port of the Windows driver takes the six tiles too, where
`capabilities::driver_faults` is set (a driver is installed). Parity error,
Framing error, Overrun and Break are raised at the program that has the port
open through `vss::raise_driver_line_error`, and the tiles are enabled only
while the port is running and its peer is open. The port keeps running, and the
studio sees no error event: the program is the one that sees the error.
Hang-up drops DCD and DSR at the program, as a modem that hangs up does, through
the endpoint's lines; they stay low until the port linked to this one moves
them again, and a port whose two lines are low already shows the program
nothing. Disconnect takes the port back from the program with the driver's
revoke request, the one Release port uses: the program's handle stops working,
as that of an unplugged adapter does, and the port stays for the next program.
When the port is not running the tiles say "The port is not running.", and when
no program has the port open (or Studio cannot tell) they say "No program has
the port open.", since the driver drops an error that nobody is there to
receive and the other two would show nobody anything. The history row reads
"Line error raised at the program on <port>", "DSR and DCD dropped on <port>"
or "Port taken back from the program on <port>". These two reasons and three
outcomes are not in the specs. A fixture is unchanged, and a port that is
neither says "Faults are only available on fixture ports."

### Per-link wiring

`StudioBackend` has `link_wiring()` and `set_link_wiring()` with
`unsupported`/`nullopt` defaults, so a backend without the engine feature shows
the Standard preset read-only. The engine backend now has the feature and sets
`Capabilities::line_matrix`, so the matrix is editable. The preset table lives
in `advanced/line_presets.cpp`; Standard is the engine's default wiring
(DTR→DSR, RTS→CTS, DTR→DCD). The bundle does not define "Loopback"; it is two
wires, DTR→DSR and RTS→CTS.

The engine side is `vss::modem_wiring` in `vss_core`: eight booleans, one per
output and input pair, in `link_options`, with `apply()` giving the peer's
inputs for the outputs that are high (an input is high when any output wired to
it is). `hub::route_lines` uses the link's wiring instead of the fixed
`null_modem_peer()`, which now calls `modem_wiring{}.apply()` and gives the same
result. `hub::set_link_wiring()` changes a link that exists and sends the
current lines through it at once, so a peer's inputs follow the new wires
without waiting for the next change. This is the second change to `vss_core`
in the capability work. `emulate_modem_lines` still switches all crossing off.

The wiring is not in the topology JSON. The schema is unchanged, so a saved
setup, `snapshot()` and Undo's remade link use the default wiring. Saving it
would add an optional field to each link; that is left for a decision. RI can
be wired, and Studio shows it, but a transport that cannot set RI (the
pseudo terminal and the serial transports set only CTS, DSR and DCD) cannot
pass it on to a program.

The bundle's Faults mockup shows the matrix on the left of "Inject a fault";
the wording in `screens/05` says rows DTR and RTS and columns DSR, CTS, DCD,
RI, which is what the implementation draws. The caption and the generated
sentence use the linked port's real name.

"Emulate modem lines" and "If both write at once" are read-only for the same
reason: `link_options` is copied when the hub creates a link, and no engine
call changes it afterwards. Both controls reflect the live link and carry the
degraded tooltip rather than pretending to be changeable.

### The owner link bar is hosted under the topology placeholder

WP4 owns the topology diagram and the node chips, and WP3 owns the Ports table
and the Monitor. WP7 hosts `OwnerBar` directly under the central topology
placeholder, shows it only in Advanced, and chooses its link in this order:
the selected link, the selected port as a join target, then the first
owner-policy join. That fallback keeps the control usable before WP3 supplies
a Ports selection and before WP4 supplies a link selection. When WP4 lands it
can move the same widget into the Topology dock unchanged.

The `Owner` and `Dropped` node chips, the Ports `Dropped` column and DROP rows
in the Monitor are not shipped here: they belong to WP3/WP4 and the last one
also needs a drop event the hub does not publish. Their owner/state data is
available through `link_view::owner` and `link_stats::dropped`. The one part
WP7 owns, the status bar sentence, now reads "N dropped, not owner" when
every dropped byte came from an owner-policy link.

### Monitor drops have no engine event

`hub::route_data` increments `link_stats.dropped` for a non-owner publisher
and continues; it does not publish an event. The Log therefore shows state
changes and errors, not DROP traffic rows, and the Monitor cannot draw them
without an engine change. Adding such an event would change `libs/vss_core`
and is out of scope. This is the main visible gap against `screens/05`.

### Log and Topology JSON are docks, not Monitor tabs

WP3 owns the Monitor bottom tab set. Until it lands, WP7 adds `logDock` and
`jsonDock`, tabifies them with `monitorDock`, and hides them in Simple mode.
Both are advanced-only actions, so the menu, palette and mode behaviour match
the design. `TopologyJsonEditor` and `EventLogPanel` are standalone widgets;
WP3 can move them into the final tab widget without a rewrite.

### Topology JSON validation is layered

Syntax errors come from `QJsonDocument::fromJson` and carry its offset, so
they always have a line. Schema errors are found by explicit GUI checks that
name the field (unknown port kind, unknown virtual kind, missing address,
duplicate names, unknown link mode with a `Did you mean` suggestion, bad
arity, missing port references and loops). The line of each field comes from
a path index of the text: one pass over the document records where every
value starts under a path such as `ports/1/serial/baud_rate`, so a problem in
the second port is on the second port's line and not the first match of the
same word. The check order matches the hub: a loop is tested against routes
created so far, and a duplex link's reverse route is added with it.
`vss::config::from_json()` has the final word and `Apply` is disabled unless
it accepts the document, so an invalid document is never applied. It returns
only an error code, so when the GUI checks pass and the engine still refuses,
the editor asks the engine again for each port on its own, then for each
serial setting of the failing port, and reports the line of the smallest piece
the engine refuses. No engine rule is copied into the GUI and the schema is
unchanged. "Go to line" is hidden only when the cause is not in a port or a
link. `Capabilities::json_positions` is true for the engine backend, because
the editor does the placing.

`Export file` in the JSON editor writes the editor text verbatim when it
parses; `Apply` always uses the engine's parsed document. `Format` uses the
engine's canonical serialisation when the document is valid and Qt's
indented form when only the JSON syntax is valid.

### Benchmark

WP7 shipped the dialog only, with `Capabilities::benchmark` false, because the
core lived in `vss_cli` and `implementation-plan.md` recorded the maintainer
default as "no extraction". The design review recommended building it, so the
core is now extracted and the dialog runs for real.

`vss::run_benchmark` (`libs/vss_studio/include/vss/studio/benchmark.hpp`) writes
a seeded pattern to one endpoint in chunks and counts what another receives. It
blocks, reports progress at most every 50 ms and stops when a flag is set.
`vss_cli bench` calls it and prints the same four lines as before. Bytes that
reached the receiver before the run are not counted. `StudioBackend` gains
`run_benchmark`, the engine backend forwards to the function and sets
`Capabilities::benchmark`, and `FakeStudioBackend` gives tests a run with a set
loss and duration that touches no port.

The dialog runs the call on a worker thread, so the window stays responsive.
While a run is going the Run button reads "Cancel" and the choices are locked;
closing the dialog asks the run to stop, and the destructor joins the thread.
The result chip reads "Passed" when every byte arrived and "Failed" when some
did not, and the Lost metric says by how much. A cancelled run shows
"Cancelled" and cannot be copied. A write that fails or times out shows
"Failed" with the engine's message. The linked-port refusal is real: it is
computed from the live links, shown in a warning note and used as the Run
tooltip. The primary button reads "Run" rather than the mockup's "Run again",
because the mockup shows a completed run and this dialog starts without one.

### Add a mock fixture grows a truthful sentence

The fixture and its two-way link are real `create_mock_port` and
`connect_ports` calls. The result sentence uses the design wording when both
`faults` and `line_matrix` exist; in this build it says "After it exists you
can open it from the Inspector." instead of promising fault injection and
wiring changes the build cannot do. A failed link rolls the new fixture back so
the setup is not left half-created.

### Export as command has no mockup

There is no dedicated mockup for File, Export as command. It reuses the JSON
view's "Run it without the window" block: the real file name when the setup
has been saved, a suggested `<name>.json` otherwise, "Copy as command" and
"Export file". Export writes the current snapshot through
`vss::config::save_file` and updates the command afterwards.

### Event log timestamps are GUI arrival times

Hub events carry `steady_clock` time points, which have no wall-clock origin.
The Log stamps a row when the coalesced batch reaches the GUI thread, using
`QTime::currentTime()` with millisecond precision. That is the only honest
timestamp available without an engine change.

### Advanced dialogs and theme changes

The shared footer, `ink2` labels and the matrix caption rebuild their palette
or style when `Theme::themeChanged` fires, so a dialog opened before a theme
switch stays readable. This matters because the review PNGs construct each
dialog in one theme and grab it in both.

### Testable degraded state

`line_wiring` and the adapter methods live in `backend/view_types.hpp` and
`studio_backend.hpp`, not in `vss_core`. No topology schema field was added,
no node position was written to the document, and no new dependency was
introduced.

## WP8

### Undo and Redo reverse changes, not snapshots

The design asks for a stack that restores the previous topology. The engine's
`snapshot()` document records port names, kinds and settings, but not the
endpoint identifiers or the advertised path an auto, pseudo terminal or
com0com port received. `apply()` therefore creates new endpoints and, for an
auto port, a new COM number or pseudo terminal; a program that had the old
path open loses it. A snapshot restore is not exact, so none is used.

`Capabilities::undo` is true for the engine backend and means something
narrower: the window keeps a history (`UndoService`, 50 steps) of changes that
have an exact inverse. Connecting a link is undone by removing it, removing a
link by making it again with the same shape, ports, options and owner, an
owner change by setting the previous owner, and a rename by naming the port as
it was (the Topology JSON editor, the wizards and adding or removing ports are
not recorded). A link is matched by the ports it
joins and not by id, because the engine gives a link that is made again a new
id. No `QUndoStack` is used, because its commands are objects with their own
lifetime and the history here is only closures over the backend.

The history describes the ports it was recorded on, so it is cleared whenever
the set of ports changes: adding or removing a port, opening or applying a
document, a wizard. A step that cannot be applied (the link it names has gone)
clears the history rather than leave later steps that assumed it, and the status
bar says so. Undo and Redo are disabled while there is nothing to reverse,
and their text names the step ("Undo Connect ports"). A backend without the
capability keeps them disabled with "Not available in this build".

### Support code uses the `vss::gui::support` namespace

WP2 kept the old flat `apps/vss_gui` translation units compiling until WP9.
Those files already define `vss::gui::ui_settings`, `load_ui_settings`,
`save_ui_settings`, `SettingsDialog` and `AboutDialog`, so the redesigned
surfaces could not reuse those names in the same namespace and link. The new
`apps/vss_gui/src/support/` library therefore lives in
`vss::gui::support`; the shell brings the names in with one using-directive in
each translation unit that needs them. WP9 can delete the legacy files and, if
desired, flatten the namespace then. This is a build-integration decision, not
a design one: the classes, copy and behaviour are the ones specified.

### Keyboard shortcuts are one generated table

`Shortcuts.html` shows an "Available today" table and a "Proposed" table with
a New tag. The dialog is generated from `ActionRegistry`, and the registry has
no way to know which shortcuts the mockup considered proposed, so a static
split would have to be hand-typed and would drift. `ShortcutsDialog` renders
one table of every registered action with a non-empty shortcut, in registry
order, and filters it. The registry is the single source of truth the design
asks for; the two-column split is a design-time illustration.

### Help contents is a small index

`03-information-architecture.md` adds Help, Help contents (F1), but no screen
spec describes a help browser, no documentation is bundled with the GUI, and
the GUI has no network access. WP8 therefore ships a compact "Help contents"
dialog that indexes the four specified support surfaces: Setup check, Why is
nothing arriving?, Keyboard shortcuts and About. It chooses a topic and the
shell opens that dialog after the index closes, so no modal loops are nested.
The alternative, opening the shortcuts dialog from Help contents, would have
made two different menu items do the same thing.

### Setup check details not specified by the mockup

The mockup covers the Windows, driver-missing state. The following choices fill
the gaps and are visible in the dialog:

- The driver-instructions dialog copy and the "Why is a driver needed?" text
  are new. The dialog never downloads or installs; it opens the official
  com0com project page with `QDesktopServices`, and it also shows the URL in a
  copyable block for a machine with no browser. The URL lives in one constant
  (`com0com_project_url()`), per `08-engine-gaps.md`.
- "Continue in test mode" closes the dialog and shows the toast "Test mode.
  New ports only work inside Virtual Serial Studio." The setup is untouched.
- The carry-on row is Windows-only. On Linux and macOS the spec lists pseudo
  terminals, group membership and real devices, so no carry-on row is added.
- The dialout row is answered by `getgroups()` on Linux. On macOS there is no
  dialout check, so that row says "Not checked yet" and explains that the
  /dev permissions are the place to look. Elevation is answered by the Windows
  token elevation API, or by `geteuid()` on POSIX, where the row is not shown.
- "Why is a driver needed?" appears only on Windows, where a driver is needed.
- The top note's bold "inside" is rendered as plain text in `NoteBanner`,
  because that shared widget takes plain text. The sentence is verbatim and
  the accessible name reads the same.

### Readiness copy for the states the spec did not cover

The launcher strip's warning sentence is Windows/driver specific ("until a
driver is installed"). On Linux and macOS a missing pseudo terminal now shows
"Setup needs attention. New ports may not be usable by other programs.", and a
build that cannot run the check shows "Setup was not checked. Open the setup
check to see what this PC needs.". The status bar chip gets a third state,
"Setup not checked", for the same case; the design only specified "Setup
ready" and "Setup needs attention". Both surfaces, and the dialog, read the
same `query_setup_report()` provider, so they cannot disagree.

The readiness strip and status chip consider only whether new virtual ports
can be opened by other programs (the Virtual Serial Studio driver or com0com
on Windows, a pseudo terminal elsewhere). A Linux dialout problem is still shown in the setup check as
"Needs attention", but it does not turn the launcher / status bar warning on,
because it does not stop port creation.

### About and licence copy

RC3 feedback changed the About design. The single Environment tab became two,
Build information (version, tag, revision, branch, working tree, build number,
build time, build type, compiler, Qt and Boost built with) and Environment
(operating system, architecture, kernel, Qt running, platform plugin, display,
language, profiling), each with its own copy button, and the footer button
became "Copy diagnostic information", which copies both and is what
`environment.txt` in the diagnostic bundle holds. The header shows one full
version string. The Licences tab lists only what a build contains, read from
`third_party.json`: the com0com driver is installed separately by the user and
is no longer listed, and Tracy appears only in a build that has the profiler. The
Fonts tab is read from the same file (`fonts`), so adding a bundled font is one entry there; with
none, it keeps the text the spec has. The Licences tab summarises each licence (component, licence name,
one line of context) instead of embedding the full licence texts; the
product's own GPLv3 notice and warranty disclaimer are in the About tab, and
no font is bundled. The standard GPLv3 "This program is free software..."
paragraphs are the one place new copy is used, because the About tab has to
carry the licence notice.

### Diagnostic bundle traffic and archive

The engine's bounded traffic ring is reachable through `hub::traffic()`, so the
bundle's "Traffic contents" option is real: the adapter gained
`StudioBackend::traffic(limit)`, both backends implement it, and the option
writes `traffic.txt` with hex and an ASCII preview. It stays unticked by
default and is the only option that can contain payload bytes. Every text file
passes through `redact_secrets()`: URL credentials, `password=`, `token:`,
`api_key=`, bearer tokens and the home directory are removed, and the
replacement keeps JSON valid. The ZIP is stored (uncompressed), written with
`QSaveFile` and no third-party library; the test validates the headers and CRC
and opens the archive with Python's `zipfile`.

If `capabilities::diagnostics` is false (a build without this layer), the
dialog says so, disables the include list and Save bundle, and keeps "Copy
environment only" available. Nothing is ever sent anywhere and the GUI makes no
network request; opening the com0com page delegates to the user's browser and
only when the user presses the button.

### Bundle and About need the build identity in a library

The About window and `environment.txt` need the commit, branch, date, build
type, compiler and version. CMake used to compute these only for the `vss_gui`
executable. WP8 moves the Git detection above the new support target and passes
the values as public compile definitions on `vss_gui_support`, so the About
dialog and its tests see the same values as the executable. The definitions
fall back to "unknown" if the values are missing, and no secret or machine name
is included.

## WP6

### The network bridge has no server role

The design's default is "Wait for other computers to connect (server)". WP6
re-checked Boost.VirtualSerial: `tcp_bridge_port` and `udp_bridge_port` only
resolve and connect, there is no acceptor or bind path, and
`vss::studio::bridge` attaches both URIs as clients. The server role cannot be
built without an engine change, which this package is not allowed to make.

`capabilities::network_server` is added and is false in the real adapter. The
network dialog defaults to the client role, disables the server option with
the tooltip "Not available in this build", and shows a visible warning that
this build can connect to a server but cannot wait for one. The firewall note
is therefore not shown in this build; it appears when a backend reports the
capability. URI preview, validation and the client bridge are real. The
bundled "Device on the network" template uses a loopback client target
(`tcp://127.0.0.1:4001`) for the same reason and still validates.

### A warning for an RFC 2217 server that other computers can reach

RFC 2217 has no password, and the spec has no text for that. The network
dialog shows a warn note, a new string wrapped in `tr()`, when the address of
an RFC 2217 server is not this computer (`0.0.0.0` or any other address) and
has no `?allow=` list: "RFC 2217 has no password. Anyone who can reach this
port can use the device. Choose This PC only (127.0.0.1), or in Advanced add
?allow=192.168.1.0/24 to the address to say who may connect." It follows the
Advanced address too. TCP and UDP servers get no such note; their default
stays "All network interfaces".

### Create another pair is a GUI adapter method

The share wizard's "Create another pair" needs `vss::create_com0com_pair`,
which the studio facade does not expose. WP6 adds one narrow method,
`StudioBackend::create_com0com_pair()`, implemented by the real adapter with
the elevation prompt and by the fake backend for tests. It is gated by
`capabilities::com0com_pair_creation` (false off Windows) and documented for
WP5 to reuse. No library behaviour changed.

### Real-device templates wait for the Attach flow

`share-three-programs` and `device-on-the-network` open a real serial device.
The design says the device is chosen through Attach, which belongs to WP5 and
does not exist on this branch. WP6 does not guess a device: those template
cards say "Needs a real device", the "Use template" button is disabled while
one is selected, and an info note says to use Ports, Attach a real device
first. The templates still load and validate in the unit tests, and the
dialog returns their documents once WP5 supplies the device flow.

### Share step 3 shows Open until the engine can see a program

Where `Capabilities::peer_open` is false (Windows and macOS), a real backend
reports `peer_state::unknown`. The Ready table then shows the "Open" chip and the
result note omits the sentence about Waiting labels changing, following
`08-engine-gaps.md`. A backend that reports a definite `not_open` or `open`
shows "Waiting for program" or "Program connected"; the fake backend and the
tests cover both mappings, and a pseudo terminal on Linux reports them for real
(see "Peer-open honesty").

### Add another program adds to the share in place

The first "Share device" is one operation: `studio::share_serial` creates the
source, every listener and the split link as one rollback unit, and a failure
leaves the studio unchanged. From step 3, "Add another program" does not run
it again. It makes one more program port and adds it to the share's link with
`add_target` (`add_program_port` in `backend/share_edit.cpp`). The device, the
link and the other programs' ports are not touched, so a program that has its
port open keeps it, and the wizard stays on step 3 with the new program in the
title and the table. If adding fails, the new port is removed, and the wizard
says "Could not add another program" and "The share is as it was. Check the
device, then try again." The wizard stops at 32 programs.

Going back to step 2 and choosing "Share device" again still removes the
previous share and creates the complete new one. A failure then reports "The
previous share was removed. Check the device, then try again."

### Error pattern is inline in these dialogs

The shared error pattern in `screens/06` is a dialog. Connect, Share and the
network endpoint render the same danger title, cause and "Copy details" as an
inline panel so the wizard keeps its step and the entered values. A refused
link keeps the Connect dialog open exactly as the screen says. The "Show the
loop" button is not present because highlighting a cycle is WP4 topology
work; the loop sentence names the pair instead.

### Toolbar-only Advanced Share

The IA adds Share to the Advanced toolbar while the Topology menu keeps it in
Simple. `topology.shareToolbar` is a second, Advanced-only action placed on the
toolbar. `action_spec` gains a `palette` flag and this action sets it false,
so the command palette does not list the same command twice. No existing menu
action changed level or position.

### Copy added where the deck is silent

The following sentences are new because the specs do not name them: the
self-link reason ("A port cannot link to itself. Choose a different port."),
the connect result toasts ("Connected A and B.", "Redirected A to B.",
"Monitoring A on B.", "Split A to B and C.", "Joined A and B into C."), the
client-role result sentence ("Studio connects to X on port N and talks to
Y..."), the capability note, the validation sentence for a missing or invalid
address and the hint for a port outside 1 to 65535, "Turn on Advanced mode"
in the join More options row, "Use a device instead" in the share wizard, the
"No serial device is available" note, and the Templates note that a setup
needs Attach. All use the shared vocabulary and sentence case.

### Free-pair prediction and device settings

The share wizard predicts a port for each program before the engine runs:
each unclaimed com0com pair contributes the end the studio advertises (the
second end; the engine tries that order), and programs beyond the free pairs
are predicted as `loopback:listener-N`. The Ready step always uses the real
ports returned by `share_serial`. On platforms without com0com the planned
cell reads "PTY". Device settings are carried in the share URI's query string,
so no engine signature changed.

### Recent setups keep the WP2 keys and add per-file metadata

The launcher still reads `ui/recentFiles` and `ui/recentSummary`. WP6 adds
`ui/recentEntries/<sha1>/summary` and `.../lastOpened` for the Recent table,
recorded by `record_recent()`. The file list stays the ordering source, capped
at ten, and nothing is stored outside `QSettings`.

### No Qt Network module for the listen address

The design allows "a specific address" on the Listen on combo. Enumerating
local interfaces would add `Qt6::Network`, which is not an approved dependency
for this GUI. The combo offers all interfaces, this PC only, and a specific
address field instead; Advanced address editing accepts the full URI. This
covers the same values without a new module.

### Small widget extensions

`PickCard` gained `set_trailing()` for the A-to-B mode glyphs and now keeps its
content at the top with a fixed vertical policy, so template cards no longer
stretch. `NoteBanner` and `ResultSentence` gained a rich-text overload that
carries a plain accessible sentence, which keeps bold port names out of the
screen-reader text. `ActionRegistry` gained the `palette` placement flag
described above. The new QSS rules are `QLabel[secondary="true"]`,
`QFrame[dashed="true"]`, `QFrame[spin="true"]` and `QFrame#dialogFooter`; all
use existing tokens.

## WP5

### Linked pair names are chosen by the adapter, not typed

The design lets the user edit First port and Second port on Windows. The
current adapter cannot advertise a chosen driver port name: `studio` creates a
virtual port from a free com0com pair and advertises the pair's other end, and
there is no API to select the half or override the advertised path. Honouring
the fields would either lie about the path or require a `vss_studio` change,
which WP5 is not allowed to make. The pair form therefore shows the real free
COM ports the adapter found, read-only, and creates only when at least two
complete com0com pairs are free. On Linux and macOS the fields are read-only
placeholders because the pseudo-terminal paths do not exist until creation.
The inline uniqueness message still exists for a stale or duplicate free-pair
value, but a user normally cannot edit the fields to reach it; the free-pair
filter and the disabled "There are not enough free driver pairs" state are the
visible degradation. With the Studio driver the fields instead read "Chosen
when you create the pair" and nothing is filtered, because the driver makes
the ports when asked. The pair result sentence still names the free ports, and
the status-bar summary after creation names the paths the engine actually
advertised.

### The Studio driver counts as a virtual port driver

The designs were drawn when com0com was the only Windows driver, so every
Windows row, note and count speaks of com0com. The repository now ships its own
driver (`vsscom`, `vsscom_um`), and `studio` tries it before com0com. The GUI
therefore reads one more fact, `setup_facts::driver_found` (with
`driver_version`), from `vss::driver_available()` and `vss::driver_version()`,
and treats either driver as "virtual ports ready" in the setup check, the
launcher strip, the status chip, the pair panel, the share wizard and the
port-ready dialog. The copy deck has no strings for this, so the new sentences
are written to the same pattern and say which driver was found and its version:

- Setup check, driver row: "The Virtual Serial Studio driver is installed, so
  other programs can see ports Studio makes as COM ports. Driver version 2."
  With com0com as well, it adds that Studio tries its own driver first. With
  neither, the row reads "Neither the Virtual Serial Studio driver nor com0com
  was found." and keeps the "Install com0com" action and instructions, which
  are shown only in that case.
- Administrator row: the driver makes ports without administrator rights (a
  normal user may ask the control device for a port), so the row is OK.
- The driver makes a port when asked, up to 16 for a user and 32 in all, so
  the com0com "free pairs" rule is not applied to it. The pair form shows
  "Chosen when you create the pair" in both fields, because Windows picks the
  COM numbers (the COM number editor is a follow-up), and is enabled without a
  pair count. The share wizard lists "A free port" for each program and warns
  only above the per-user limit, hiding "Create another pair", which cannot
  raise it.
- A port the driver made is listed by the system as a serial port. The adapter
  marks one whose hardware id starts with `VSSCOM\` as a virtual device, so the
  attach dialog and the share wizard show it as *Virtual* and do not offer it as
  a real device.
- Limit: the Studio driver stops at 16 ports for one user. The wizard counts
  the non-loopback virtual ports Studio already holds as driver ports, which
  over-counts when some of them came from com0com; it errs towards warning.

### Linked pairs use existing driver pairs and do not elevate

The mockup says Windows will ask for administrator permission. This WP5 path
never runs `setupc.exe`: it claims two free com0com pairs that are already
installed, then links the two Studio ends. Showing the warning would be false,
so the form shows "Windows uses free com0com pairs for this pair, so it does
not ask for administrator permission." instead. `permission_denied` from the
adapter is still mapped to the error pattern with copyable details and recovery
buttons, so a future adapter that creates a new pair can reuse the dialog.

### Add a port does not duplicate the Attach form

WP5 builds the full Attach a real device dialog, but the Real device choice in
Add a port shows a one-sentence continuation note and the primary "Next" opens
that dialog. Embedding a second copy of the device list and settings would
double the state, validation and accessibility surface for the same flow. The
Linked pair form is embedded because it is the same dialog the launcher calls
with Linked pair selected.

### Attach opens the device and creates a separate port

The spec says programs should open the port Studio creates, not the physical
device. `studio::attach_serial` alone exposes the physical path as the
endpoint's path, so AttachDialog composes three real adapter calls: attach the
device under `<name>-input`, create an automatic virtual port under `<name>`,
and split the device to it. A failure at any step removes the ports already
created. The physical endpoint remains visible to the future Ports pane; this
is the same shape the Share scenario uses.

### Device-in-use detection is supplied by the adapter, not the engine

The engine's serial enumeration cannot prove that another process holds a
device without trying to open it. A `device_view::in_use` flag was added to the
adapter; `EngineStudioBackend` leaves it false and tests can set it. The Attach
dialog always disables and explains virtual/com0com rows, and shows an "In use"
row with its reason when a backend reports it. This is the degradation the
engine-gap table asks for instead of probing the device.

### Watch focuses the Monitor but cannot filter it yet

WP5 creates and links the watcher through the adapter, selects the watched
port and opens, raises and focuses the Monitor dock. The Monitor itself belongs
to WP3, so there is no filter API to call. The filter request is recorded here
and must be wired when WP3's monitor model lands; the success status names the
source and watcher so the action is not silent.

### Pair success has no separate ready dialog

The copy deck defines Port ready for a single port. The pair form's result
sentence and the status-bar summary name both advertised paths and the Main
window takes over; inventing a second ready screen would add copy that the
bundle does not define. The pair's two ports and link are real adapter state
and are selected in the shared selection service.

### Setup check and Connect ports stay placeholders

The Port ready actions "Open setup check" and "Connect COM7 to something"
emit the shell's existing placeholder status. WP8 owns the setup check dialog
and WP6 owns Connect ports, so WP5 does not simulate either result.

### Rich result sentences and ErrorDialog recovery actions

`ResultSentence` gained a two-argument `set_result_text` so the visible sentence
can bold port names while screen readers receive the plain sentence. The rich
HTML is never used as the accessible name. `ErrorDialog` gained `add_action` so
Attach can offer "Try again" and "Choose another" before the existing Copy
details and Close; the destructive action order and initial focus are
unchanged.

### Add a port Advanced types are placeholders

Network endpoint and Mock fixture are other work packages. The cards appear in
Advanced with their spec descriptions, select a clear placeholder page and
disable the primary action; no backend call is made and no success is claimed.

## WP9

### The automated walk skips composite containers

`vss_gui --selftest` builds every shipped dialog against `FakeStudioBackend`
and walks the main window plus 24 dialog states (22 dialog classes; the Share
wizard is checked at each of its three steps) in both themes. `QScrollArea`,
`QTabWidget` and `QTabBar` are skipped as focusable containers: their child
controls are walked individually, and making the container itself an
additional stop would duplicate the tab stops. The Share wizard is walked on
all three steps; the second and third step are only reached when the fixture
can advance, which it does in this build.

### 200 percent is a separate process

Qt scale factors must be set before `QApplication` exists, and the design's
font sizes are explicit Theme tokens rather than inherited point sizes.
`gui_selftest_200` therefore runs the same binary with
`QT_SCALE_FACTOR=2;QT_FONT_DPI=192` instead of changing a font at runtime. The
same name, target, focus and clipping rules run in that process.

### High contrast uses the system palette

`VSS_HIGH_CONTRAST=1` drops the custom stylesheet and installs the platform
standard palette. Qt exposes no portable "high contrast is on" query, so this
is an explicit environment switch; custom-painted widgets still use the Theme
tokens for their text and shape cues. The selftest checks that the stylesheet is
actually empty and that reduced motion makes every transition zero-duration.

### Review PNGs are not committed

With `VSS_QA_SHOTS` set, the same walk writes the main window light, dark and
compact plus every dialog light and dark to that directory (52 images). They
are generated by the offscreen plugin and stay under `build/`; no pixel compare
runs. CTest uses the default (no captures) so CI does not write artifacts.

### RFC 2217 server

`rfc2217-listen://host:port` is an access server. The listen transport accepts
any number of TCP clients as before; for each one a `vss::rfc2217::session`
speaks Telnet and negotiates the COM-PORT option (RFC 2217). The session holds
no socket, so its protocol is tested on its own. Payload bytes pass through
with 0xFF doubled in both directions, and each client has a write queue,
because the protocol replies and the payload share one socket.

What the client asks for is accepted and reported, as a serial port would, but
changes no real device: baud rate, data bits, parity, stop bits and flow control
are remembered in the session and answered, and a linked physical port keeps
its own settings. DTR, RTS and break are different. They are the endpoint's
output lines, so they pass through the hub's line wiring to the linked port. In
the other direction the CTS, DSR, DCD and RI that reach the endpoint are sent
to the client as NOTIFY-MODEMSTATE, filtered by the mask the client set. Purge
is acknowledged and the line-state mask is stored, but no line state is
reported.

A client that changes DTR or RTS has no way to call the endpoint, so the
transport completes a read with no bytes, and `endpoint::on_read` treats a
zero-byte read from a transport that has control lines as "the lines changed"
and refreshes them. This is the third change to `vss_core` in the capability
work: one condition, in a branch that used to restart the read. The
`rfc2217` endpoint kind and the saved `uri` are unchanged; a saved setup
reopens the server on the same address.

### Driver ports: COM number, programs that hold a port, and release

These additions are not in the original design; the copy is written in the
same plain style and is not in the copy deck.

- **COM number (Add port, Inspector).** Add port gains one row, *COM number*, a
  spin box where 0 reads *Automatic*, shown only for *Best available* on Windows
  when `capabilities::com_numbers` is true. It is disabled and reset when *How
  many* is above one, because a number names one port. The Inspector gains a
  *COM number* row with a *Change...* button for a port the backend marks
  `driver_port`. The change runs on a worker thread, as the benchmark does, since
  it restarts the port. A refusal is said under the field with
  `com_number_refusal`, which names the number and the next step, and the dialog
  stays open with the form as it was.
- **Release port (Advanced).** A button in the Inspector, visible only in
  Advanced mode, for a driver port that reads *Program connected*. The dialog
  has two choices and one confirming button. Wording states a fact (no data for
  N seconds) and never calls a program stuck, hung or frozen; a test checks the
  dialog's text for those words. Ending a program is offered only when
  `holder_view::can_end` is true, which is the user's own programs, or any
  program when Studio is elevated.
- **Warnings (removal, New, close).** A port with `peer_state::open` is named in
  the confirmation of Remove port, in File, New and in a new *Close Studio?*
  question with *Close Studio* and *Keep Studio open*. A port whose state is
  unknown is never claimed to be in use.

### Remaining limits carried past WP9

The capability-gated feature that stays visible is the editable line matrix.
Peer-open is built for a pseudo terminal on Linux and for a Studio driver port
on Windows, and reads unknown elsewhere (see "Peer-open honesty"). Rename (see "Rename" above) is no longer one of them. Benchmark
extraction (see "Benchmark" above) and fault injection (see
"Fault injection" above) are no longer among them. The network server role is no
longer one of them: `vss_ports` listens for `tcp-listen://` and
`udp-listen://` addresses, so the network dialog's server choice works for TCP
and UDP, and RFC 2217 has a server role too (see "RFC 2217 server"
under WP9). The bundle's real
device templates are still disabled until a template device picker is
designed. Log and Topology JSON remain Advanced docks. The legacy flat GUI
sources are deleted in this package; `vss_gui` now builds only the redesigned
tree.

## Design review: what the build does

These lines record behaviour the build has and the specs left out or worded
differently. The build is right in each case, so the specs are read with these
lines applied. The numbers are the rows of the design review.

### Frame and chrome

- **Advanced dock heights (3).** Send and Monitor grow to fit their extra
  controls, to about 230 pixels, instead of staying at 162 and 196.
- **Compact layout (4).** The compact layout starts below 1100 pixels wide or
  700 pixels high, and topology nodes shrink below 500 pixels of view width.
- **Toolbar labels (10).** The toolbar uses the menu wording: "Attach real
  device", "Connect ports", "Remove port", "Mock fixture", "Share a device".
- **Readiness chip (12, 19).** A third state, "Setup not checked", is shown by
  builds that cannot run the setup check. The launcher strip has matching
  variants: "Setup needs attention. New ports may not be usable by other
  programs." and a "Setup was not checked" line.
- **Settings keys (14).** Besides the nine keys in the information
  architecture, the build stores `ui/savedSends`, `ui/recentSummary` and
  `ui/recentEntries/<sha1>/...`.
- **Help contents (15).** F1 opens a small index that opens the setup check,
  Why is nothing arriving, Keyboard shortcuts or About.
- **Export as command (16).** It reuses the Topology JSON block: the real file
  name, "Copy as command" and "Export file".

### Flows

- **Real device in Add a port (22).** The settings area shows one sentence and
  **Next** opens the Attach dialog, so there is one copy of the device state.
- **Linked pair without enough pairs (27).** The message reads "There are not
  enough free driver pairs. Open the setup check."
- **Network addresses (53, 54).** A server is `tcp-listen://host:port` or
  `udp-listen://host:port`, so the bridge command block reads
  `vss_cli bridge serial://COM3?baud=115200 tcp-listen://0.0.0.0:4001`.
  An RFC 2217 server is `rfc2217-listen://host:port`. See "RFC 2217 server".
- **Errors in wizards (68).** An error appears as an inline panel with "Copy
  details", so the wizard keeps its step and its input.

### Panes and topology

- **DROP rows (38).** The monitor shows one row when a link's dropped count
  grows, with the count and no payload, because the hub publishes no drop
  event.
- **Monitor link (43).** A monitor link is dashed and has an arrow head, since
  it is one-way.
- **Unlinked ports (44).** Ports with no link are placed in a final column.

### Support

- **Diagnostic bundle (66).** Every file is redacted for secrets, and the
  optional traffic contents go in `traffic.txt`.
- **About (71).** The Environment tab includes a Boost row, and the Licences
  tab summarises each licence instead of printing its text.
- **Keyboard shortcuts (72).** One table, generated from the action registry,
  replaces the available and proposed tables.

### Tokens and strings

- **Windows fixed font.** The design says never set a font family. Qt reports Courier New as the fixed font on Windows, a thin typewriter face. `Theme` picks Consolas, which ships with the OS, when the font database has it, and otherwise keeps the system fixed font. No font is bundled. Segoe UI Variable was tried for text and rejected: Qt 6.12 draws it semi-bold.
- **Menu colours (75).** The product stylesheet sets `color: %ink%` on menus.
  Without it menu text was dark on dark after a theme switch on Windows.
- **Launcher cards (87).** The border colour `line2` is 1.76 to 1 against the
  light surface. A card is identified by its text and icon, and the border is
  decoration.
- **Strings (84).** Strings stay inline in each dialog. There is no strings
  header per screen. Every user-visible string goes through `tr()` and the
  catalogues in `apps/vss_gui/translations/`.
- **Ordinals.** The share wizard's ordinal words ("third") are catalogue
  entries in the `ordinal` context, so each language inflects them.

### COM numbers and a port nobody has opened

- **COM numbers of driver ports.** The window no longer leaves the number to
  Windows (the lowest free one, so COM1 and COM2 on a computer with no real
  ones). It asks the Studio driver for the number above every COM port the
  computer already has, never below COM3, because COM1 and COM2 are the legacy
  on-board names that Windows, firmware and many programs treat as taken. A
  number that Windows' COM name database holds, which no list of ports shows, is
  refused by the driver with no port made, and the next number is tried. Without
  the helper service the number is left to Windows as before. The number is kept
  in a saved setup, so a program that opens a fixed name finds the port again.
  The pair form still reads "Chosen when you create the pair"; the COM number
  editor remains a follow-up.
- **Waiting for program.** The Inspector, the Port ready dialog and the
  troubleshoot row say that the program has to open exactly this port, because a
  program that was set up with another COM name opens nothing of ours. The
  Inspector adds a "Copy name" button to the waiting note.
