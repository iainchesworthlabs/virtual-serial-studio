# Profiling with Tracy

Every layer of Virtual Serial Studio is instrumented for
[Tracy](https://github.com/wolfpld/tracy), pulled from
[vcpkg](https://vcpkg.io): the engine and its transports, the studio service,
the CLI, the GUI, the test runners and the performance suite. The instrumentation
is compiled in but **dormant** until the program is told to profile, so a problem
seen in a release build can be profiled without building a debug one.

Instrumentation is compiled only when `VSS_TRACY` is on; without it every macro in
[`profile.hpp`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/libs/vss_core/include/vss/core/profile.hpp)
is a no-op and nothing of Tracy is linked.

## When it is compiled in, and when it runs

| Configuration | Compiled in | Runs |
|---|---|---|
| Debug, vcpkg toolchain | yes (`VSS_TRACY` defaults to on) | **starts by itself**, listens on this computer only |
| Debug, no vcpkg toolchain | no (the package cannot be found) | never |
| Release, vcpkg toolchain (`VSS_TRACY` defaults to on) | yes | **dormant** until asked (switch in Settings, `--profile`, `VSS_PROFILE=1`) |
| Release, no vcpkg toolchain | no (the package cannot be found) | never |

Debug is the everyday case: a profiler can connect at any time. Release stays
lean by default, and a release binary built with the option on costs one relaxed
load per zone until profiling is switched on (measured below).

Dormant means: no profiler thread, no socket, no buffers. Tracy is built with its
**manual lifetime** (`TRACY_MANUAL_LIFETIME`), so nothing exists until the program
calls `vss::profiling::enable()`. The client is also built in **on-demand** mode:
after it is started, zones record only while a profiler is connected, and a
profiler can connect to a process that has been running for hours.

The client is built by the overlay port in
[`cmake/vcpkg-ports/tracy`](https://github.com/iainchesworthlabs/virtual-serial-studio/tree/develop/cmake/vcpkg-ports),
which is the upstream port plus two features the manifest asks for:
`manual-lifetime` and `no-broadcast`. `vcpkg-configuration.json` points vcpkg at
it.

## Turning it on

| Program | Command line | Environment | Other |
|---|---|---|---|
| `vss_gui` | `--profile`, `--profile=PORT`, `--profile-remote`, `--profile-wait=S`, `--no-profile` | `VSS_PROFILE=1`, `VSS_PROFILE_PORT`, `VSS_PROFILE_REMOTE`, `VSS_PROFILE_WAIT` | Settings, Performance, Profiling |
| `vss_cli` | the same, anywhere on the command line | the same | |
| `vss_perf` | `--tracy` (also runs the end-to-end phases), or the same options | the same | |
| Test runners (`vss_unit_tests`, `vss_system_tests`, the GUI test programs) | `--tracy` on the unit and system runners | `VSS_PROFILE=1` on every runner | |

`--no-profile` or `VSS_PROFILE=0` turns it off even where it is on by default.
The unattended `vss_gui --selftest` and `--screenshot` runs and the test runners
do not start profiling by themselves, not even in Debug.

In the GUI, Settings, Performance, Profiling has a switch to record a trace (it
is remembered, so Studio records again from its next start unless `--no-profile`
or `VSS_PROFILE=0` says otherwise) and one to let other computers connect. While the listener exists the status bar shows
a "Profiling :8086" chip, and About lists the state in the environment table (and
in the diagnostic bundle's `environment.txt`).

### Turning it off

Turning it off (the switch, or `vss::profiling::disable()`) stops recording at
once: every zone is back to the dormant cost, and a connected profiler sees
silence. Tracy cannot close its listening socket while threads that have used it
are alive, so the **port stays open until the program exits**. The GUI says so
("Recording is paused. 127.0.0.1:8086 stays open until Studio quits.") and the
switch can turn recording on again on the same socket. Nothing is sent while the
profiler is paused.

## What leaves the computer

`AGENTS.md` says there is no network access from the GUI and no telemetry. A
Tracy profiler has to reach the program over TCP, so the shape is:

- **Off by default** in anything that is shipped. Only a Debug build, which is
  never shipped, starts by itself.
- The listener binds **127.0.0.1** only. Other computers can connect only when the
  person who started the program chose `--profile-remote`,
  `VSS_PROFILE_REMOTE=1` or the "Let other computers connect" switch. That choice
  can be made for the first start only; once the socket is bound to this computer
  it cannot be widened without restarting.
- The client **never broadcasts**: it is built with `TRACY_NO_BROADCAST`, so it
  does not announce itself on the network and a profiler must be pointed at the
  address.
- **Nothing is sent** until a person connects a profiler. Until then the listener
  accepts a connection and does nothing else.
- The GUI says plainly when it is listening and on which address and port.

The port is 8086 or the first free one after it (up to 20 are tried); a port
asked for with `--profile=PORT` is used exactly, and the start fails if it is
taken. `vss::profiling` binds a test socket first, so the program knows the real
port before Tracy's thread starts.

## What is instrumented

Zone names follow `area.thing`. The area is the first word, and
`tools/tracy_report.py --areas` groups on it.

| Area | Where | Shows |
|---|---|---|
| `hub.*` | `libs/vss_core/src/hub.cpp` | routing, dispatch, topology changes, plots `hub.endpoints`, `hub.links`, `hub.dispatch_queue`, `hub.route_bytes` |
| `endpoint.*` | `endpoint.cpp` | reads, writes, state transitions, `endpoint.write_queue` |
| `link.*`, `core.*` | `delay_line.cpp`, `bytes.cpp`, `traffic_log.cpp` | delay lines, byte formatting, the traffic log |
| `config.*` | `libs/vss_config` | JSON documents |
| `ports.*` | `libs/vss_ports` | each transport: `ports.loopback`, `ports.serial`, `ports.listen` and `ports.tcp`/`ports.udp`, `ports.rfc2217`, `ports.virtual`, `ports.vcom` (the Windows driver and the helper service IPC), `ports.com0com`, `ports.enumerate`, `ports.permissions`, `ports.peer_open`, `ports.stable_link` |
| `studio.*` | `libs/vss_studio` | every public call, capture, replay, responder, benchmark, reconnect |
| `cli.*` | `apps/vss_cli` | one zone per command |
| `gui.*` | `apps/vss_gui` | see below |
| `test.*`, one zone per test | `tests/framework` | each test case, named after it |
| `bench.*`, `perf.*` | `perf/` | the component benchmarks and the benchmark phases |

Messages mark state changes (`hub.endpoint_added <name>`, `endpoint.open <name>`,
`studio.reconnect.lost`, `ports.permissions.probed`, `profiling.enabled`, ...),
and the threads the libraries start are named (`vss.hub.io`, `vss.reconnect`,
`vss.vcom.watch`).

### The GUI

Generic coverage, so slow work shows up without a hand-placed zone:

- **`gui.event`** wraps every event the application dispatches. The zone text is
  `ReceiverClass / EventType / objectName`, so a timeline shows who spent the
  time. A queued signal arrives as a `MetaCall` event, which is how a cross-thread
  signal-to-slot delivery is visible; a direct connection runs inside the zone of
  the event that emitted it. Hot slots carry their own `gui.*` zone.
- **`gui.slow_event`** is a message for an event that took longer than its budget
  (16 ms for input and painting, 50 ms for the rest), and `gui.event_ms` plots
  every event over 1 ms.
- **Frames.** The main frame ticks once per pass of the event loop, `gui.loop` is
  the busy time of each pass (a discontinuous frame), and `gui.paint` marks each
  top-level window paint.
- **`gui.render.*`, `gui.gpu.frame` and `gui.render_mode`**: how the canvas is drawn
  on the GPU (the widget's frame, draw, texture upload, sync, plots of item
  textures and their memory), the GPU's time for a frame, and a message with the
  renderer chosen at start-up or after a fallback. See
  `docs/gui-design/docs/11-rendering.md`. The GPU time is recorded only when
  `VSS_PROFILE_GPU=1` is set and the profiler is recording at start-up, because
  reading it slows every frame of the canvas.
- **`gui.paint.*`** zones for the custom painting: port rows, topology nodes and
  links, the canvas background. Every other widget's paint is a `gui.event` zone
  with the class name.
- **`gui.bridge.*`, `gui.backend.*`, `gui.shell.*`, `gui.status.refresh`,
  `gui.setup.*`, `gui.topology.*`, `gui.monitor.*`, `gui.fixture.tick`**: the
  event bridge draining the engine's events into slices for the window, the
  look at the engine (`gui.backend.rebuild`) and the time spent in each signal
  it raises, what the main window does for a change of ports or a batch of
  events, the operating-system probes behind the readiness chip, the canvas
  layout and link routing, and the Monitor's traffic list. `gui.bridge.drain`
  has the number of events of the batch as its text, with `gui.bridge.take`,
  `gui.bridge.convert`, `gui.bridge.release` and `gui.bridge.emit` inside it.
  The Monitor takes a batch in `gui.monitor.append_events` and puts the rows in
  the table in `gui.monitor.commit` (`gui.monitor.trim`,
  `gui.monitor.remove_rows`, `gui.monitor.insert_rows`); a painted cell is
  `gui.monitor.paint_cell` with the column as its text, and each read of the
  model by Qt is `gui.monitor.data`. `tools/tracy_phases.py` lists the cells
  by column.
- **`gui.stall_ms`** and **`gui.busy_ms`** (plots, from `support::StallProbe`,
  which runs while the profiler is recording): how late the window's event loop
  ran and how much CPU the window used between two ticks.
- Panes, dialogs, flows, layout and link routing, the theme and icon rendering,
  the bundle and zip writers, the event bridge and the backend rebuild each have
  `gui.<area>.<thing>` zones; startup and shutdown are marked with `gui.startup`,
  `gui.window_shown` and `gui.shutdown`.

### Tests and benchmarks

Each test case runs inside a zone named after it, so a profile of a test run lists
every test with its duration and the zones it called, and `test.duration_us`
plots the duration of each. `test.failed <name>` is a message.

```bash
VSS_PROFILE=1 VSS_PROFILE_WAIT=30 ./build/linux-tracy-gui/bin/vss_unit_tests
```

`vss_perf` has a micro-benchmark per component in a `bench.*` zone
(`bench.bytes_hex`, `bench.traffic_log`, `bench.serial_settings`,
`bench.config_validate`, `bench.studio_ports`, `bench.studio_snapshot`,
`bench.studio_apply`) besides the throughput benchmarks, and `perf/floors.json`
gates their results. A limit with `"min"` is a rate that
must not fall below it; one with `"max"` is a cost that must not rise above it
(`zone_dormant` is the cost of a zone in a build that is not profiling).

`vss_perf --tracy` adds two end-to-end phases: a real socket echo through the hub
(`tcp://127.0.0.1`, one connection) and, when `VSS_LOOPBACK_A` and
`VSS_LOOPBACK_B` name a pair of pseudo terminals, a write on one side read on the
other. Each phase emits a frame per chunk.

## Finding a frozen window

Studio also keeps a **stall log** without any profiler: when the GUI thread has
not run its event loop for two seconds, a line in `stalls.log` (in the
application's local data folder, for example
`%LOCALAPPDATA%\Virtual Serial Studio\Virtual Serial Studio\` on Windows)
names the receiver class and event type it was inside, and a second line says
when it recovered. It is local only and logging only. The same lines are Tracy
messages while profiling records.

The GUI thread must never be called from, or wait for, the engine's threads, so
a "Not responding" window is always work on that thread: a slot that does too
much, or too many small ones. To find it:

1. Run `vss_gui` with profiling on (a Debug build with the vcpkg toolchain has
   it on by default; a Release build with Tracy compiled in needs `--profile` or
   the switch in Settings, Performance, Profiling) and attach a profiler or
   `tracy-capture`.
2. Look at the `gui.stall_ms` plot for the spikes. The zones under a spike are
   the cause; `gui.bridge.emit` contains everything that runs for a batch of
   events and `gui.backend.emit_ports_changed` / `emit_links_changed` /
   `emit_stats_changed` contain the pane refreshes.
3. Reproduce it in a test where you can. `apps/vss_gui/src/shell/tests/
   test_responsiveness.cpp` drives the real engine and the real window with a
   busy loopback and fixture and fails if the GUI thread uses more than the
   budget of CPU time between two ticks of the probe. Set
   `VSS_GUI_STALL_BUDGET_MS=100` to hold it to the strict figure on a quiet
   computer; the default leaves room for a loaded build machine.

## Building it

Debug, with the toolchain that supplies the package:

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-debug -DVSS_BUILD_PERF=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset linux-debug
```

Release with the profiler compiled in but dormant (`windows-msvc-tracy-gui` on
Windows; `linux-tracy` and `windows-msvc-tracy` leave out the GUI, the CLI and
the tests):

```bash
cmake --preset linux-tracy-gui
cmake --build --preset linux-tracy-gui
```

A Release tree without vcpkg simply has the option off; asking for it there is a
configuration error with a message that names the missing toolchain.

## Capturing a trace

Tracy's client streams to a profiler: either the GUI, or the headless
`tracy-capture` command line tool. The capture tool is part of Tracy's `cli-tools`
feature (`vcpkg install tracy[cli-tools]`), and CI builds it from the pinned
source tag to keep the Linux job light. The client is not announced on the
network, so point the profiler at the address.

```bash
tracy-capture -a 127.0.0.1 -o trace.tracy &   # waits for a client, saves on disconnect
vss_gui --profile --profile-wait=30            # or: Settings, Performance, Profiling
wait                                           # the client exiting ends the capture
tracy-csvexport trace.tracy > trace.csv
python tools/tracy_report.py trace.csv --areas
```

A program started with `--profile-wait=S` holds its start-up until a profiler
connects (or `S` seconds pass), so a short run is captured from its first zone.
With a program that is already running, turn profiling on in Settings, connect,
reproduce the problem, and disconnect.

The report prints a Markdown table of the hottest zones:

```
| Zone | Calls | Total (ms) | Mean (us) | Max (us) | Share |
| --- | ---: | ---: | ---: | ---: | ---: |
| `perf.e2e.tcp` | 1 | 26.85 | 26851.1 | 26851.1 | 3.97% |
| `hub.route` | 8448 | 14.47 | 1.7 | 99.3 | 2.14% |
```

### Time inside Qt, and one table per phase

The aggregated export has one `gui.event` row for every event the window
delivered. `tools/tracy_phases.py` reads the unwrapped export (`tracy-csvexport
-u`) instead: it splits `gui.event` by its text, gives every zone its self time
(its time less the zones inside it) and prints one table per phase of the render
bench (`bench.repaint`, `bench.pan`, `bench.resize`, `bench.move`, `bench.idle`,
`bench.traffic`), or one for the whole capture.

```bash
tracy-csvexport -u trace.tracy > trace.u.csv
python tools/tracy_phases.py trace.u.csv --gui --phase bench.resize
```

The self time of an event is where time inside Qt shows. The self time of the
top-level window's `UpdateRequest` and `Expose` events is the compose and
present of the window; that of `gui.render.widget` is Qt's part of a canvas
frame. `--gui` keeps the GUI thread only and `--match` keeps the zones whose
name holds one of the given words.

`docs/gui-design/docs/12-render-profiling.md` is a profile of the whole
application made this way, with the hotspots ranked.

`--areas` adds the totals per area. `--require a,b,c` fails when an expected zone
is missing, and `area.*` is met by any zone of that area.
`--profile NAME` also requires everything the named profile lists in
[`perf/required-zones.json`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/perf/required-zones.json),
which is how CI notices that the instrumentation or the workload stopped covering
a layer. `--markdown` and `--json` write the same data to files.

## What CI does

The `End-to-end profiling (Tracy)` job runs after a merge to `develop` and on
request, not on a pull request. It:

1. installs vcpkg at the pinned baseline and resolves the manifest (the overlay
   port builds the client with manual lifetime and no broadcast),
2. builds `tracy-capture` and `tracy-csvexport` from Tracy v0.14.1, or restores
   them from the cache. Building them downloads CPM.cmake and sources from
   GitHub, so a build retries up to three times, and the finished tools are
   cached under a key that names the Tracy version. The tools are built with
   `NO_ISA_EXTENSIONS=ON`, because Tracy otherwise builds for the CPU it runs
   on and a cached copy stops with an illegal instruction on another runner.
   Change `TRACY_VERSION` in `ci.yml` to move to another release,
3. configures `linux-tracy-gui` (Release, profiler dormant), builds `vss_perf`,
   `vss_gui` and `vss_unit_tests`,
4. checks the dormant cost of a zone against `perf/floors.json`,
5. creates a pseudo-terminal pair so the serial phase runs too,
6. captures three runs, each started with `--profile-wait`: `vss_perf --tracy`,
   `vss_gui --selftest --profile` (the offscreen GUI scenario) and the unit tests,
   exports each, requires the zones of its profile in `perf/required-zones.json`
   and appends the per-zone and per-area tables to the job summary,
7. uploads the traces, CSVs, reports and raw benchmark output as artifacts.

## Reading the numbers

- **Share** is the fraction of the captured trace; zones nest, so a parent's
  share includes its children. Compare `total` across zones rather than adding
  them.
- **Mean** is per call. `hub.route` runs thousands of times per phase; the
  end-to-end phase zones run once and are there for the timeline.
- Tracy's overhead is real (both sides timestamp every zone), so an
  instrumented build is slower than the Release binaries the floors measure.
  Use it for shape and attribution, and `vss_perf` with `perf/floors.json` for
  the gated numbers.

## What it costs

`vss_perf` measures a zone in the three states a build can be in (nanoseconds per
zone, empty loop subtracted):

| State | Meaning |
|---|---|
| `zone_dormant` | compiled in, profiler never started: what a release build costs |
| `zone_armed` | recording switched on, no profiler attached |
| `zone_recording` | a profiler is attached |

Measured on Windows 11 with MSVC 19.51 in Release, one zone, loop cost
subtracted:

| State | ns per zone |
|---|---:|
| `VSS_TRACY` off (compiled out) | 0 |
| **dormant** (compiled in, not started) | **1.0** (2.2 inside `vss_perf`, beside other code) |
| armed (recording on, no profiler attached) | 6.5 |
| recording (a profiler is attached) | 36 to 39 |

Throughput through the hub is unchanged within the noise of the machine
(`hub_route`, `config_roundtrip` and `event_dispatch` medians of five runs are
within a few percent of each other with the instrumentation compiled out and
dormant). The `zone_dormant` limit in `perf/floors.json` keeps the dormant cost
from growing.

## Design notes

- Tracy's `StartupProfiler` and `ShutdownProfiler` are the manual lifetime. A
  restart after a shutdown is not safe in a program with other threads (each
  thread keeps a producer into the old queue), so `shutdown()` is for the end of
  `main` only, and "off" is a pause.
- Every instrumentation macro checks one relaxed atomic flag. When it is set, a
  small guard counts the thread as "inside Tracy" so `shutdown()` can wait for
  zones in flight before it lets Tracy free its queues.
- Names given to threads before profiling starts are remembered and applied the
  first time the thread records.
