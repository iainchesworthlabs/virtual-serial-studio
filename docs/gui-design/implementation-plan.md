# Implementation plan: redesigned GUI

Status: written by WP0 after a code-level recon of `develop` at `73d09bb`.
The design bundle is installed at `docs/gui-design/`. This document is the
contract the later work packages build against; corrections to it go through
the same review as code.

The design bundle was written from the project README and docs, not from the
Qt source. Every claim below was re-checked against the tree and cites the
symbol that proves it.

## 1. Current GUI inventory

The existing GUI is the `ux-workbench` shell. It is a polling application:
`MainWindow::poll()` calls `refresh_now()` every `ui/refreshMs`, and every
panel is fed a fresh `std::vector<vss::endpoint_info>` / `link_info` snapshot.
There is no event subscription and no adapter layer. Colours are hard-coded in
`apps/vss_gui/theme.hpp` and in each panel.

| File | What it is today | New home |
|---|---|---|
| `main.cpp` | `QApplication` setup, `--selftest`, `--screenshot <file>` | stays, gains the backend and gallery switches |
| `main_window.cpp/.hpp` | 1,730-line shell: menus, toolbar, docks, polling, scenarios, save/load | `src/shell/main_window.*` |
| `port_model.cpp/.hpp` | `PortTableModel`, a `QAbstractTableModel` over endpoint snapshots | `src/panes/port_model.*` + delegate |
| `monitor_model.cpp/.hpp` | `MonitorModel`, bounded traffic rows fed by diffing snapshots | `src/panes/monitor_model.*` |
| `topology_view.cpp/.hpp` | `TopologyView`, `EndpointItem`, `LinkItem` on `QGraphicsScene` | `src/topology/` |
| `inspector_panel.cpp/.hpp` | `InspectorPanel` with state, counters and line checkboxes | `src/panes/inspector_panel.*` |
| `send_panel.cpp/.hpp` | `SendPanel`, ASCII/hex input, picks an open endpoint | `src/panes/send_panel.*` |
| `dialogs.cpp/.hpp` | `AddVirtualPortDialog`, `AddMockDialog`, `AttachSerialDialog`, `ShareSerialDialog`, `JoinPublishersDialog`, `BridgeDialog` | replaced by `src/dialogs/` |
| `connect_dialog.cpp/.hpp` | `ConnectDialog`, `connect_mode`, `connect_request` | replaced by `src/dialogs/connect_ports.*` |
| `settings_dialog.cpp/.hpp` | `ui_settings`, `SettingsDialog`, `QSettings` persistence | replaced by `src/support/settings_dialog.*` |
| `about_dialog.cpp/.hpp` | `AboutDialog` with About/Environment/Licences/Fonts tabs | replaced by `src/support/about_dialog.*` |
| `theme.cpp/.hpp` | `apply_theme()`, `state_colour()`, `state_text()`, formatting helpers; hex literals | replaced by `src/theme/` |
| `selftest.cpp` | `MainWindow::run_selftest()`, offscreen end-to-end check | stays, extended by each WP |
| `CMakeLists.txt` | one executable, flat source list, `QT_NO_KEYWORDS` | top-level plus one fragment per area |

The current `ui_settings` struct and its `QSettings` keys are the only
persistence. WP2 replaces them with the keys in `03-information-architecture.md`.

## 2. Real engine API for every operation the specs need

The adapter calls these symbols; screens never do.

| Operation | Symbol | Evidence |
|---|---|---|
| Create a virtual port, auto / pty / loopback / named pipe / driver | `vss::studio::create_virtual_port(name, kind, options)`; auto is `kind == pty` on platforms without pty, where the studio falls back to `create_auto_virtual_port` | `libs/vss_studio/include/vss/studio/studio.hpp:57`; `libs/vss_studio/src/studio.cpp:154-252` |
| Create a mock fixture | `vss::studio::create_mock_port(name, settings, target)` | `studio.hpp:73` |
| Attach a real device or a network backend | `vss::studio::attach_serial(name, uri, settings, options)`; URI schemes are `serial`, `tcp`, `udp`, `rfc2217`, `mock` | `studio.hpp:63-70`; `libs/vss_ports/include/vss/ports/serial_transport.hpp:26-34` |
| Remove a port | `vss::studio::remove_port(id)` | `studio.hpp:77` |
| List ports / links | `vss::studio::endpoint_infos()`, `link_infos()` | `studio.hpp:91-92` |
| Advertised path | `vss::studio::advertised_path(id)`; already merged into `endpoint_info.path` | `studio.hpp:84`; `studio.cpp:385-397` |
| Enumerate devices | `vss::studio::serial_devices()` -> `vss::enumerate_serial_devices()` | `studio.hpp:125`; `libs/vss_ports/include/vss/ports/serial_enumeration.hpp:34-53` |
| Detect com0com and pairs | `vss::studio::detect_com0com()` -> `vss::detect_com0com()`, `com0com_installation.pairs` | `studio.hpp:124`; `libs/vss_ports/include/vss/ports/com0com.hpp:26-43` |
| Links: duplex, split, join, redirect, monitor | `vss::hub::connect/split/join/redirect/monitor`; the studio convenience wrappers drop `link_options` | `libs/vss_core/include/vss/core/hub.hpp:84-105`; `studio.hpp:97-107` |
| Per-link policy and modem-line crossing | `vss::link_options{emulate_modem_lines, policy}` passed to the hub methods above | `libs/vss_core/include/vss/core/link.hpp:10-27` |
| Owner arbitration | `vss::hub::set_owner(link_id, endpoint_id)`, `release_owner(link_id)`; the studio facade does not expose them, the adapter reaches `studio::engine()` | `hub.hpp:110-112`; `studio.hpp:32` |
| Send bytes | `vss::endpoint::write(bytes_view, write_handler)`; the buffer is copied into a `shared_ptr` before the handler runs | `endpoint.hpp:102-107`; `libs/vss_core/src/endpoint.cpp:335-351` |
| Control lines | `vss::endpoint::set_line(control_line, bool)`, `lines()`, `refresh_lines()`, `supports_control_lines` | `endpoint.hpp:109-118`; `endpoint_info` fields at `endpoint.hpp:47-60` |
| Events | `vss::hub::subscribe(event_filter, event_sink)` / `unsubscribe`; sink runs on the hub event strand | `hub.hpp:114-134` |
| Counters and traffic ring | `endpoint_info.stats`, `link_info.stats`, `hub::traffic(limit)`, `hub::clear_traffic()` | `hub.hpp:136-137`; `libs/vss_core/include/vss/core/stats.hpp:19-39`; `traffic_log.hpp:21-54` |
| Topology JSON | `vss::config::to_json/from_json/validate/load_file/save_file`; `studio::snapshot()` / `apply()` | `libs/vss_config/include/vss/config/document.hpp:62-73`; `studio.hpp:112-114` |
| Share a device with rollback | `vss::studio::share_serial(uri, listeners)`; cleans up listeners and the source on failure | `studio.hpp:103`; `studio.cpp:482-521` |
| Bridge / join by URI | `vss::studio::bridge(from, to)`, `join_ports(sources, target)` | `studio.hpp:106-112` |
| Shutdown order | `hub::stop()` closes on each strand, then joins; sinks must capture shared state | `docs/architecture.md:63-75`; `hub.hpp:114-123` |

## 3. Capability table, re-verified

Legend: **Present** = the GUI can use it through the studio/hub today.
**Partial** = the primitive exists but a facade or a safety property is
missing. **Missing** = not reachable without an engine change.

| Capability | Status | Evidence | GUI behaviour |
|---|---|---|---|
| Create, list, remove virtual ports (auto, pty, loopback, named pipe, driver interface) | Present | `studio::create_virtual_port`, `remove_port`, `endpoint_infos`; `virtual_port.hpp:19-31` | Use directly. |
| Enumerate serial devices and com0com pairs | Present | `studio::serial_devices`, `detect_com0com` | Use directly. |
| Links: connect, split, join, redirect, monitor interleave or owner | Present | `hub::connect/split/join/redirect/monitor`; `link_options` | Use hub methods through the adapter so options are not lost. |
| Owner arbitration | Partial | `hub::set_owner/release_owner` exist; `studio` has no facade | Adapter exposes it via `studio::engine()`. Record the missing facade in the PR. |
| Cycle refusal on add link | Present | `hub::add_link` refuses a cycle and returns `errc::cycle_detected` | Map to the loop pattern; "Show the loop" is computed in the GUI. |
| Events, counters, bounded traffic ring | Present | `hub::subscribe`, `endpoint_info.stats`, `hub::traffic` | Event bridge, coalesced by refresh interval. |
| DTR/RTS/BREAK set, CTS/DSR/DCD/RI read | Present | `endpoint::set_line`, `supports_control_lines` | Use directly; disable a line the endpoint does not support. |
| Null-modem crossing switch | Present | `link_options::emulate_modem_lines` | Use for "Emulate modem lines". |
| Per-link line wiring matrix | Missing | Only the boolean above; no per-line API in `link.hpp` | Read-only Standard preset behind `Capabilities::line_matrix = false`. Never simulate it in the GUI. |
| Fault injection on mock fixtures | Missing at the studio/GUI boundary | `serial_transport::mock()` exists (`serial_transport.hpp:79-80`) but `endpoint` keeps `transport_` private (`endpoint.hpp:153`) and the studio does not expose it | Disable fault tiles. WP7 may add one forwarding method in `vss_studio` with approval. |
| Peer-open detection ("Waiting for program") | Missing | `endpoint_info.open` is the studio's own transport state (`endpoint.hpp:82`), not an external opener | `PeerState::Unknown`; show "Open" and omit the waiting sentence. `Capabilities::peer_open = false`. |
| Setup check inputs: com0com, elevation, dialout | Partial | `detect_com0com` and `serial_devices` work without elevation; no elevation or group helper | Show driver and device rows; mark elevation/dialout "Not checked yet" until WP8 adds the platform helper. |
| Install com0com / restart elevated | Missing (GUI action) | `create_com0com_pair(..., request_elevation)` exists, but installing com0com itself is outside the process | Instructions dialog with a URL constant; elevated relaunch on Windows only. |
| Free com0com pair count | Partial | `com0com_installation.pairs` plus paths already claimed by `endpoint_infos` | Derive in the share wizard; no new engine API. |
| Share as one operation with rollback | Present | `studio::share_serial` creates listeners and rolls them back | Use directly. |
| Bridge by URI (`tcp://`, `udp://`, `rfc2217://`) | Present | `studio::bridge`, `attach_serial`, `classify_uri` | Build the URI in the GUI and pass it through. |
| JSON load, save, validate | Present | `config::document`, `studio::snapshot/apply` | Use directly. |
| JSON validation errors with line and column | Partial | `config::from_json` returns an `error_code`, no offset; syntax offsets can come from `QJsonDocument::fromJson` | Show the message; hide "Go to line" for schema errors. |
| Rename port | Missing at the studio boundary | `hub::rename_endpoint` exists (`hub.hpp:77`) but `studio::records_`, links and `snapshot()` keep the old name | Hide Rename and F2; record why in `design-deviations.md`. |
| Undo and redo | Missing | No service; `snapshot`/`apply` restore a whole document but exactness is unproven | WP8 owns a `QUndoStack`; if restore is not exact, keep confirmations and drop Undo. |
| Benchmark | Missing | `command_bench` in `apps/vss_cli/main.cpp:407`; no library function | Capability-gate the dialog; extracting a shared core needs maintainer approval. |
| Diagnostic bundle ZIP | Missing | None | WP8 writes a stored ZIP in the GUI layer, no dependency. |
| Event log tab | Missing | None | GUI-side `EventLog` fed from the bridge. |
| Recent files, templates | Missing | None | `QSettings` and bundled JSON resources validated by `vss_config`. |
| Node positions in topology JSON | Missing | `config::document` has no positions | Session only; do not change the schema. |

The engine already implements `null_modem_peer()` as DSR<-DTR, CTS<-RTS and
DCD<-DTR (`control_lines.hpp:111-118`), so the Pair result sentence is
truthful and the DTR-to-DCD question in the bundle README does not need a
maintainer decision.

## 4. Module layout and CMake pattern

The new GUI lives under `apps/vss_gui/src/`:

```
apps/vss_gui/
  main.cpp
  selftest.cpp
  src/
    backend/      StudioBackend, EngineStudioBackend, FakeStudioBackend,
                  EventBridge, capabilities, peer_state, view types
    theme/        Theme, IconProvider, FocusRingStyle, resources
    widgets/      StatusChip, DirectionBadge, Tag, NoteBanner, CodeBlock,
                  CopyButton, SegmentedControl, PickCard, Switch, Stepper,
                  Disclosure, ResultSentence, Toast, FormRow, KeyCap,
                  ConfirmDialog, ErrorDialog, WidgetGallery
    shell/        ShellWindow, ActionRegistry, CommandPalette, Launcher,
                  StatusBar, LevelService, SelectionService
    panes/        PortsPanel, PortModel, PortDelegate, InspectorPanel,
                  SendPanel, MonitorModel, MonitorPanel, EventLog
    topology/     TopologyView, NodeItem, LinkItem, TopologyLayout,
                  TopologyJsonEditor
    dialogs/      AddPort, PortReady, Pair, Attach, Watch, ConnectPorts,
                  ShareWizard, NetworkEndpoint, Templates
    support/      SetupCheck, Troubleshoot, DiagnosticBundle, Settings,
                  About, Shortcuts, UndoService
```

One CMake fragment per area. WP0 establishes the pattern with the backend and
leaves the rest to the package that owns the area. The top-level
`apps/vss_gui/CMakeLists.txt` will grow a `vss_gui_ui` static library; each
area fragment appends sources with `target_sources`. The executable keeps
`main.cpp`, `selftest.cpp` and the link line. The backend is a separate static
library because the tests link it without Qt Widgets:

```cmake
# src/backend/CMakeLists.txt
add_library(vss_gui_backend STATIC
  studio_backend.cpp engine_studio_backend.cpp
  fake_studio_backend.cpp event_bridge.cpp
  studio_backend.hpp engine_studio_backend.hpp
  fake_studio_backend.hpp event_bridge.hpp
  capabilities.hpp view_types.hpp)
target_include_directories(vss_gui_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vss_gui_backend PUBLIC Vss::studio Qt6::Core)
target_compile_features(vss_gui_backend PUBLIC cxx_std_23)
set_target_properties(vss_gui_backend PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_backend)
```

`vss_gui` links `vss_gui_backend`. `QT_NO_KEYWORDS` stays on the executable and
is set on the backend library too, so `endpoint::emit` is never rewritten.

WP2 split the frame into `vss_gui_shell` rather than the planned `vss_gui_ui`.
A dedicated static library has the same dependency shape, lets the shell tests
link without the legacy panels, and leaves `vss_gui` as `main.cpp` plus the
old translation units. Those old flat files still compile and are not
instantiated by `main.cpp`; WP9 removes them and then broadens the no-hex
scan to the whole GUI. Keeping them for WP2 meant the package could land the
new frame without a 24-file deletion in the same commit as its first working
build. `vss_gui_shell` links `vss_gui_widgets`, `vss_gui_backend` and
`vss_gui_theme`; the shell tests link it with the fake backend.

## 5. Naming and conventions

- Namespace `vss::gui`, nested `vss::gui::backend` for adapter internals is
  not used: the repo uses flat GUI names, so the adapter types share
  `vss::gui` and the `backend/` directory supplies the grouping.
- Types are `snake_case` (`port_view`, `setup_facts`, `peer_state`); Qt widget
  classes are `PascalCase` (`MainWindow`, `Theme`); methods and signals are
  `snake_case` (`create_virtual_port`, `ports_changed`), matching
  `MainWindow::refresh_now()` and `SendPanel::status_message`.
- Enum values are `lower_snake` (`peer_state::not_open`,
  `link_shape::two_way`).
- Engine types are used unchanged in the adapter's return values until they
  cross into a view struct; no `std::string`/`QString` mix inside one struct.
- Every new file carries an SPDX licence line naming GPL-3.0-or-later.

## 6. Self-test and test strategy

`vss_gui --selftest` runs `MainWindow::run_selftest()` (`apps/vss_gui/main.cpp`,
`apps/vss_gui/selftest.cpp`). It forces the offscreen platform, shows the real
window, creates two loopback ports and a link, writes a payload, checks the
monitor rows and the counters, and returns non-zero on the first failure. It is
registered as the `gui_selftest` CTest in `tests/CMakeLists.txt`.

Two layers are added:

1. **`vss_gui_backend_tests`** (this package) links the backend and
   `Qt6::Core`, creates a `QCoreApplication`, and uses the in-tree
   `tests/framework/test_framework.hpp` macros. A small Qt-aware `main`
   iterates `vss_test::registry()`. It covers the fake backend, the event
   bridge, coalescing, the queue cap and the shutdown race. Registered as
   CTest `gui_backend` in `tests/CMakeLists.txt`, so it only exists when Qt
   does. No new third-party test dependency.
2. **`vss_gui_theme_tests`** (WP1) links the theme and widget libraries,
   creates a `QApplication` on the offscreen platform and uses the same
   in-tree macros. It checks token resolution, every QSS placeholder, the
   WCAG contrast pairs, the no-literal-colour scan, the icon cache, the
   gallery in both themes, accessible names, 24 px targets and the widget
   behaviours. Registered as CTest `gui_theme` in `tests/CMakeLists.txt`.
3. **`--selftest` additions** (WP9) for the whole window and every dialog:
   light/dark, 100/200 percent, accessible-name walk, focus rings and the
   PNG set.

The in-tree framework is deliberately Qt-free (`tests/framework`), so it is
used for the logic and widget tests and not for the whole-window walk. This is
a deviation from the WP1 prompt wording ("tests with the in-tree framework")
and is recorded in `design-deviations.md`.

## 7. Adapter contract

`StudioBackend` is a `QObject` abstract interface with one real implementation
(`EngineStudioBackend`) and one in-memory test double (`FakeStudioBackend`).
Screens hold a `StudioBackend&`, never a `vss::studio&`.

- Operations: create virtual/auto/mock ports, attach by URI, remove, list
  ports/links/devices, connect/split/join/redirect/monitor with
  `link_options`, disconnect, set/release owner, send, set and refresh lines,
  snapshot/apply, share/bridge, setup facts.
- Every fallible operation returns `vss::result<T>`; the GUI translates
  `vss::error_code` to copy in one place.
- `port_view` adds `peer_state peer` (Unknown/NotOpen/Open) and
  `bool studio_only` (a loopback or mock endpoint). The real backend reports
  `Unknown` until the engine can observe a peer open; the fake can set any
  state for tests.
- `caps()` is const and is the only place a screen asks whether a
  control exists. `line_matrix`, `peer_open`, `faults`, `rename`, `undo` and
  `benchmark` are false in the real backend today.
- Signals: `ports_changed`, `links_changed`, `lines_changed(endpoint_id)`,
  `batch_ready`. All are emitted on the GUI
  thread. There is deliberately no per-write callback: it would have to
  touch a `QObject` from the endpoint strand. A queue-full rejection is
  published by the engine as an error event and arrives in the coalesced
  batch; `send()` returns an immediate error for a missing or closed port.

### Event bridge

`EventBridge` owns a `std::shared_ptr<queue_state>` and a `QTimer` on the GUI
thread:

- the sink captures the `shared_ptr` (not `this`, not a stack local) and only
  locks a mutex to append to a bounded `std::deque`; it never calls Qt;
- the timer drains the queue on the GUI thread at the refresh interval,
  converts the events to `gui_event` (copied bytes, text fields), emits one
  `batch_ready`, and counts dropped events when the cap is hit;
- `stop()` stops the timer and unsubscribes; the queue state is shared, so an
  in-flight sink delivery after `unsubscribe` writes into live memory;
- the bridge must be destroyed before the hub/studio that owns it. `main`
  owns the studio before the window, so the window (and bridge) is destroyed
  first; the tests cover an in-flight delivery and a publisher that is still
  running when the bridge is destroyed, which is the shutdown race the
  architecture note warns about.

The timer-drained queue is the "queued delivery" implementation: it is
cross-thread safe without touching a `QObject` pointer from the engine thread,
and it coalesces by construction.

## 8. Migration sequence

- **WP0 (this package)**: bundle, this plan, backend + bridge + tests. No
  visual change; the current GUI keeps working.
- **WP1** (this package): adds `Theme`, `IconProvider`, `FocusRingStyle`,
  widgets, resources and the gallery, plus `vss_gui_theme_tests`. The old
  shell stays on its legacy `theme.hpp` compatibility path; only the new
  `src/` tree is migrated. The `no hex outside Theme` scan is scoped to
  `src/` until WP2 removes the legacy files, then becomes a whole-
  `apps/vss_gui` test.
- **WP2**: introduces `vss_gui_ui`, the `ActionRegistry`, the new frame,
  launcher and palette; deletes the old flat files it replaces. `develop` is
  not merged to until WP9, so the integration branch may be incomplete.
- **WP3-WP8**: branch from the integration branch, one area each, merge in
  package order. Shared registries are append-only.
- **WP9**: whole-tree checks and docs.

## 9. Risks and open questions

1. **Legacy theme migration**: `theme.hpp` is included by every old panel and
   hard-codes colours. WP1 keeps the old shell on a compatibility path and
   applies the colour-literal scan to `src/` only; WP2 deletes the old files,
   after which the scan covers the whole GUI.
2. **Intermediate incompleteness**: after WP2 the integration branch has
   placeholders where WP3-WP8 will land. It must never be merged to `develop`
   before WP9.
3. **Peer-open and faults**: the engine cannot answer them today. The GUI
   degrades visibly; no fake state, no `vss_core` change. A later WP may add a
   thin `vss_studio` forwarding method with approval.
4. **Rename**: the hub can rename, but the studio's records and saved JSON
   would go out of sync. Hide it rather than ship a corrupting action.
5. **Benchmark**: needs a library extraction from `apps/vss_cli/main.cpp`.
   Default is capability-gated UI only; extraction is a separate, approved PR.
6. **Test framework split**: logic tests use the in-tree framework in a
   Qt-linked executable; widget checks stay in `--selftest`. No new framework.
7. **Cross-platform**: local verification is MSVC only. The code must stay
   portable (POSIX paths, `Q_OS_*` guards, no MSVC-only constructs), and CI
   on GCC/Clang/AppleClang runs only after the user pushes.
8. **Visual verification**: no images are visible to the agent. Every UI gate
   produces offscreen `grab()` PNGs for the human; the HTML mockups are read
   for measurements, never ported.
9. **Questions for the maintainer** (defaults in brackets):
   - Approve extracting the benchmark core into a shared library? [No; gate
     the dialog.]
   - Preferred URL for the com0com instructions? [Official project page.]
   - Should `vss_studio` gain a `rename_port` facade so Rename can ship
     safely? [No; hide Rename.]
   - Should peer-open detection be added to `vss_studio` later? [Treat as
     Unknown now.]
   - Confirm `docs/gui-design/design-deviations.md` as the deviations file
     (the QA doc says `docs/design-deviations.md`). [Use the bundle path.]

## 10. WP0 decisions and deviations

1. **Integration branch.** WP0 and the later packages merge into
   `gui/integration`, not `develop`; `develop` changes only at the final
   approved merge. This keeps trunk shippable while the design replaces the
   shell. The bundle's order and review gates are unchanged.
2. **Qt pin.** GUI builds use Qt 6.12.0 at
   `C:\Repos\common\qt\6.12.0\msvc2022_64`. That install contains the approved
   `Qt6::Svg` module; the Qt 6.10.3 tree at `C:\Qt\6.10.3` does not, so it
   cannot build the icon provider. Every build command pins `QT_ROOT`
   (without a trailing space) and `-DCMAKE_PREFIX_PATH`. Mixing 6.10 and 6.12
   produces a binary that fails to start with `qt_version_tag_6_12`; this is
   an environment trap, not a code change.
3. **Qt-aware logic tests.** The in-tree framework is Qt-free, so the backend
   tests use a small `QCoreApplication` runner in the same executable. Widget
   tests stay in `--selftest`. No new test framework is added.
4. **No per-write callback.** See section 7; this avoids a cross-thread
   use-after-free. The Send panel reports "queued" and errors arrive as
   events, which is the pattern the design already uses for failures.
5. **Deviations file.** `docs/gui-design/design-deviations.md` is used, as
   AGENTS.md and the WP prompts say; the QA doc's `docs/design-deviations.md`
   is treated as a typo.
6. **Bundle contents.** `agents/` and the bundle ZIP are not copied into the
   repository. `README.md` and all `docs/`, `tokens/`, `icons/`, `tools/`,
   `prompts/` and `reference/` files are copied byte-for-byte, except the two
   generated previews `tokens/style.light.qss` and `tokens/style.dark.qss`.
   The bundle README says not to commit those; `tokens/style.qss.in` and
   `tokens/tokens.json` are the authoritative sources and `gen_qss.py`
   regenerates the previews into a build directory.

## 11. WP2 decisions

1. **Shell library name.** `vss_gui_shell` (section 4) replaces the planned
   `vss_gui_ui` for the frame. Pane, dialog, topology and support libraries
   keep their own fragments when those packages land.
2. **Old shell stays until WP9.** The flat `main_window`, panel, dialog and
   legacy theme files still compile; `main.cpp` constructs `ShellWindow`
   instead. Nothing links the old `MainWindow` into a visible window. WP9
   deletes the files, its compatibility `theme.hpp`, and the poller. This is
   the one deliberate departure from the migration sequence's "WP2 deletes
   the old files" line.
3. **Placeholder handlers.** Every action and launcher card is registered and
   reachable, but actions whose dialog or pane belongs to WP3 to WP8 show a
   status-bar message ("... is part of a later work package."). The message
   is deliberately explicit rather than a fake result. The menus, palette,
   toolbar visibility and mode behaviour are final; only the handlers are
   temporary.
4. **Unsaved prompt.** WP2 uses a three-button `QMessageBox` (Save / Don't
   save / Cancel) with the spec's wording because `ConfirmDialog` in WP1 has
   two buttons. WP8 replaces it with the shared dialog and the
   "turn this prompt off" setting. The setting key `ui/askOnDiscard` is
   already honoured.
5. **Compact window.** At a width below 1100 px or a height below 700 px the
   toolbar becomes icon-only, Inspector and Send become one tabbed dock, and
   Monitor starts at 156 px. The design's exact minimum is 960x640; the
   breakpoint is intentionally just above it so a 960-wide window is already
   in the compact form. Qt's toolbar extension button handles overflow rather
   than a hand-built More menu.
6. **Review-only screenshot switches.** `VSS_SCREENSHOT_EMPTY=1`,
   `VSS_SCREENSHOT_THEME=light|dark`, `VSS_SCREENSHOT_ADVANCED=1` and
   `VSS_SCREENSHOT_COMPACT=1` exist only on the existing `--screenshot` path
   so the human gate can see the launcher, both themes, both modes and the
   compact minimum. On this path only, the advanced switch writes
   `ui/advanced` so the image is repeatable; no other product startup path is
   affected.
7. **Hub display fact.** `StudioBackend::hub_running()` was added so the
   status bar can report the engine state. It is a display-only fact, not a
   new feature capability, and both adapters implement it (`true` for the
   fake).
8. **Toolbar Share.** The Topology menu has Share in Simple, but the design
   adds the Share toolbar button only in Advanced. WP2 leaves that toolbar
   slot to WP6, which owns sharing and can add an advanced duplicate action
   without making the menu item Advanced.
9. **QSS menu colour.** The product template now sets `color` on `QMenuBar`,
   `QMenuBar::item` and `QMenu::item`. The bundle template left them to the
   palette, which rendered the menu text black-on-dark after a runtime theme
   switch. The token values are unchanged; only the explicit binding was
   added.
10. **Icon refresh.** Action icons are rebuilt from `IconProvider` when
    `Theme::themeChanged` fires. Without that, toolbars and menus kept the
    light-mode ink in the dark theme.

## 12. WP3 decisions

1. **Legacy files leave the build in WP3.** The flat shell, panels, dialogs
   and legacy `theme.cpp` are no longer compiled into `vss_gui`; `main.cpp`
   links the new shell and pane libraries. The files stay on disk until WP9.
   This moved the WP2 build-list cleanup one package earlier because the new
   pane class names (`PortModel`, `MonitorModel`, `InspectorPanel`,
   `SendPanel`) collide with the old flat classes at link time.
2. **Selection service location.** `SelectionService` moved to
   `src/panes/`, where its metaobject can be built and linked by the pane
   tests. The shell links panes and shares the same instance.
3. **Default dock floors.** Ports 360, Inspector 340 and Monitor 196 are the
   minimums at normal width during WP3, with 240/260/156 in compact. The
   placeholder size hints otherwise settle the splitter near the minima. WP9
   re-checks the splitter after the panes own their content sizes and at 200
   percent text.
4. **Advanced heights.** Advanced Send and Monitor grow to fit the saved-sends
   row, filters and tab bar (about 230) instead of clipping them. Simple keeps
   the spec's 162/196; compact keeps Monitor at 156.
5. **Monitor Log is a bounded text view.** It is a read-only
   `QPlainTextEdit` capped at 10,000 blocks fed from the same event batch.
   There are no per-row widgets and users keep text selection. A separate
   `EventLog` model is not needed for the feature set.
6. **DROP rows aggregate counters.** The engine publishes dropped *bytes* on
   the link counter, not a per-drop event with payload. Monitor adds a DROP
   row when the counter grows, with the real name and byte delta and the copy
   "not the owner, not forwarded". No payload is invented.
7. **Simple hides the one-tab bar.** The dock title already says Monitor; the
   Advanced tabs restore the bar for Log and Topology JSON. The tab bar is not
   duplicated in Simple.
8. **No rename/undo.** `rename_port` and any exact undo service are not
   capabilities today, so F2, Undo and Redo remain placeholders and Rename is
   not registered. Ports deletion is confirmed and final; no Undo toast is
   shown.

## 13. WP6 decisions and integration points

1. **Dialog library.** `vss_gui_dialogs` (section 4's planned `dialogs/`
   area) holds `ConnectPortsDialog`, `ShareWizard`,
   `NetworkEndpointDialog`, `TemplatesDialog`, the `network_uri` and
   `share_plan` helpers, `recent_files` and the template resource. The shell
   links it; the tests link it without the old flat sources.
2. **Action IDs.** Existing handlers now open real dialogs:
   `topology.connect`, `topology.share`, `ports.network`,
   `file.templates`. The only new ID is `topology.shareToolbar`, an
   Advanced-only toolbar placement with `action_spec::palette = false`. The
   launcher jobs `connect`, `share`, `network` and `templates` route through
   `ShellWindow::run_job`. No existing action moved or changed level.
3. **Adapter additions.** `capabilities::network_server` is false;
   `capabilities::com0com_pair_creation` is false off Windows;
   `StudioBackend::create_com0com_pair()` is implemented by
   `EngineStudioBackend` (`vss::create_com0com_pair`, elevation, next free COM
   numbers) and `FakeStudioBackend`. No `libs/` change.
4. **Network server.** The engine has no listening TCP/UDP backend. The
   dialog defaults to the client role and disables the server role with
   "Not available in this build"; the gap is in `08-engine-gaps.md`.
5. **Templates.** Four documents are Qt resources validated by
   `vss_config`; there is no schema change. Real-device templates require the
   WP5 Attach flow and are visibly disabled until it lands. The Recent table
   keeps `ui/recentFiles` for ordering and adds per-path summary/date keys.
6. **Share semantics.** The wizard calls `share_serial` as one operation; a
   failure leaves the studio unchanged (the fake's mid-share failure test
   covers the rollback). Add another program, from step 3, adds one port to the
   share's link in place with `add_target` and leaves the device, the link and
   the other programs' ports alone; going back to step 2 and sharing again
   removes the previous share and runs the operation again.
7. **Peer states.** The real backend reports `unknown`; step 3 shows "Open"
   and omits the waiting sentence. Definite `not_open`/`open` values map to
   "Waiting for program"/"Program connected" in the fake tests.
8. **Widgets.** `PickCard` gained a trailing glyph and fixed vertical policy;
   `NoteBanner`/`ResultSentence` gained a plain accessible text overload;
   `ActionRegistry` gained the palette flag. The new QSS selectors use
   existing tokens.
