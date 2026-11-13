# Testing

The repository ships unit, system, GUI, performance and fuzz suites, all driven
by CTest.

```bash
ctest --test-dir build --output-on-failure
./build/bin/vss_unit_tests --filter=hub --verbose
./build/bin/vss_unit_tests --list
```

`vss_unit_tests` and `vss_system_tests` take `--list`, `--verbose`,
`--filter=TEXT` and `--exclude=TEXT` (each may be repeated and matches part of a
test's name), `--repeat=N`, `--junit=FILE` and `--tracy`. `--list` prints the name
of every selected case.

* **Unit** — bytes, control lines, serial settings, errors, the traffic log, the
  loopback, listening and RFC 2217 transports, serial transport, endpoints and
  their reopening, topology, events, delay and latency, buses, virtual ports,
  stable links, enumeration, com0com parsing, permissions, capture, replay,
  responders, benchmarks, JSON documents, the driver set-up check, the control of
  profiling, the studio facade and the portable core of the Windows driver
  (`vsscom_core`). The cases for one operating system are in
  `tests/unit/platform/<os>/` and are built only there.
* **System** — a real serial pair (a com0com pair on Windows, or
  `VSS_LOOPBACK_A`/`VSS_LOOPBACK_B` elsewhere) carries data both ways, survives
  an integrity transfer (256 KB over com0com, 8 KB elsewhere), shares a port with
  listeners, drives the native control-line layer, rolls back a failed bridge,
  and live TCP, UDP and RFC 2217 peers exercise the network bridges. On Linux and
  macOS the `vport:` cases start `vss_cli` as a separate process. A case that
  needs the pair skips cleanly when none exists, unless `VSS_REQUIRE_LOOPBACK=1`
  is set. CI sets it on Linux and macOS, so a missing pair fails the job instead
  of quietly reducing what is tested. Those jobs create the pair with
  `tools/pty_loopback.py` (it prints the two device paths), so the physical
  backend runs there.
* **Half-duplex bus** - `test_bus_line.cpp` tests the model of the line with
  the times written in each case (the model has no clock), and
  `test_hub_half_duplex.cpp` runs it in a hub whose clock (`hub_options::clock`)
  the test moves by hand, so a chunk leaves the wire when the test says. Neither
  sleeps or depends on how fast the machine is; the two cases that use the steady
  clock check only that nothing arrives early. One case sends from several
  threads while a member is removed, for the sanitizer legs.
* **Polled bus** - one system case runs the whole bus scenario on every
  operating system, with no hardware: a master replayed from a recording, two
  devices answering from scripts (one labelled as a real serial device), and a
  device on another computer played by an RFC 2217 client over loopback, all on
  one shared bus. It checks that each reply lands inside its window with
  margin, 200 ms for two devices and 450 ms for one, and that no member hears
  itself. A run whose times miss is repeated up to three times, because another
  process taking the CPU can spoil a time; the byte checks run every time.
* **GUI** — `gui_selftest` (the offscreen self-test, also at 200 percent scale),
  one program per area of the GUI (`gui_backend`, `gui_language`, `gui_theme`,
  `gui_shell`, `gui_simple`, `gui_panes`, `gui_topology`, `gui_advanced`,
  `gui_support`, `gui_dialogs`, `gui_flows`), and the UI suite described
  [below](#the-ui-suite). They exist only when Qt was found and run on
  `QT_QPA_PLATFORM=offscreen`.
* **Fuzz replays** — `fuzz_topology`, `fuzz_endpoint_uri`, `fuzz_agent_frame` and
  `fuzz_rule_set`
  replay the corpora once per build; see [Fuzzing](fuzzing.md).
* **Tools** — `headers` (every public header compiles on its own),
  `matrix_tools` (the Python under `tools/matrix`), the `vm_*` tests (the
  PowerShell scripts of the [guest harness](vm-testing.md), checked against a
  stand-in for `vmrun`, so no virtual machine is needed; they are added only when
  PowerShell 7 is installed) and, on Windows, `gui_subsystem` (the GUI is a
  Windows-subsystem program, so it opens no console).
* **Performance** — routing, event dispatch, JSON round-trips and mock writes,
  gated against `perf/floors.json`; see [Performance](#performance).
* **Profiling** — `VSS_TRACY=ON` instruments the pipeline with Tracy from
  vcpkg; CI captures a trace of a real socket and pseudo-terminal round trip,
  exports it with `tracy-csvexport` and publishes the per-zone table. See
  [the profiling page](profiling.md).

## Coverage

Coverage uses LLVM profile instrumentation and is gated at **90% statements**
and **80% branches** of the libraries. CI runs it in the `Coverage` job, after a
merge to `develop`:

```bash
export CXX=clang++
cmake --preset linux-coverage
cmake --build --preset linux-coverage
mkdir -p build/linux-coverage/profiles
LLVM_PROFILE_FILE="$PWD/build/linux-coverage/profiles/%p-%m.profraw" \
  ./build/linux-coverage/bin/vss_unit_tests
LLVM_PROFILE_FILE="$PWD/build/linux-coverage/profiles/%p-%m.profraw" \
  ./build/linux-coverage/bin/vss_system_tests
python tools/coverage_report.py --build build/linux-coverage \
  --exclude libs/vss_testkit \
  --gate-exclude libs/vss_ports/src/vcom_driver.cpp \
  --min-lines 90 --min-branches 80
```

Measured on Windows 11, clang-cl 23, Debug: **90.15% statements**, **82.28%
branches**. The report lists every file and marks the ones outside the gated
set; `vcom_driver.cpp` is outside it, because its detection path needs an
installed Windows driver to exercise.

### GUI coverage

The GUI (`apps/vss_gui`) has its own coverage job, `coverage-gui` in CI. It
builds `linux-coverage-gui`, runs every GUI test program (the unit-level
suites, `gui_journeys`, the UI suite) and `vss_gui --selftest` instrumented,
and reports on `apps/vss_gui` without the tests and the generated moc files.
To do the same by hand (Windows, with LLVM installed; the `-B` keeps the large
build off the repository drive):

```bash
cmake --preset windows-llvm-coverage-gui -B E:/vss-cov
cmake --build E:/vss-cov
mkdir E:/vss-cov/profiles
export LLVM_PROFILE_FILE="E:/vss-cov/profiles/%p-%m.profraw"
export QT_QPA_PLATFORM=offscreen VSS_UI_BUDGET_MS=1000000
for program in E:/vss-cov/bin/vss_gui_*_tests.exe; do "$program"; done
E:/vss-cov/bin/vss_gui.exe --selftest
python tools/coverage_report.py --build E:/vss-cov --include apps/vss_gui/ \
  --exclude tests/ --exclude moc_ --exclude mocs_compilation --exclude qrc_ \
  --objects E:/vss-cov/bin/vss_gui.exe E:/vss-cov/bin/vss_gui_*_tests.exe
```

(`clang-cl` stops at `__uuidof` in `permissions_windows.cpp` under `-Werror`;
set `CCC_OVERRIDE_OPTIONS=x-Werror` for a local measurement.) The standard is
**90% of statements and 80% of branches**. Measured on Windows 11 with
clang-cl, Debug, all GUI programs together: **83.5% of statements and 68.9%
of branches**. The CI gate stands just below that (80% and 65%) so a change
cannot lose ground, and is raised as journeys are added. At that measurement
the largest gaps, by uncovered lines, were `shell_window.cpp` (the tray and
background paths, drag and drop, window-state restore, rarely used menu paths),
`topology_view.cpp` (wire-from-keyboard, loop note, rubber band, accessibility
navigation), `json_validation.cpp`, `engine_studio_backend.cpp` (driver-only
paths), `job_form.cpp` (the network and bus variants), the setup check and
permission dialogs (they act on the machine), and `elevated_step.cpp` (it needs
an elevated process).

## Performance

```bash
vss_perf --output=perf-results.json
python tools/perf_report.py perf-results.json --floors perf/floors.json
```

`vss_perf` is built by the release presets (`VSS_BUILD_PERF`). `--quick` skips the
slower benchmarks. Besides the four throughput benchmarks below it runs a
micro-benchmark per component and measures the cost of a Tracy zone; see
[Profiling](profiling.md#tests-and-benchmarks). `perf/floors.json` holds a limit
for each: a `min` is a rate that must not fall below it, a `max` a cost that must
not rise above it. The floors are conservative, because shared runners are noisy.
CI checks them in the `Performance floors` job, which runs in the merge queue when
library code changed.

Indicative results (Windows 11, MSVC 19.51, x64, Release, `vss_perf`):

| Benchmark | Result |
|---|---|
| `hub_route` | ~2.4 GB/s |
| `event_dispatch` | ~1,230,000 events/s |
| `config_roundtrip` | ~2,850 documents/s (23 KB each) |
| `mock_write` | ~359 MB/s |

## Profiling

Tracy is compiled into Debug builds by default and can be compiled into a
Release build (`VSS_TRACY=ON`, or a `*-tracy*` preset); it is dormant until the
program is told to profile. [The profiling page](profiling.md) has the options,
the zones, how to capture a trace and what CI checks. For the tests:

```bash
VSS_PROFILE=1 VSS_PROFILE_WAIT=30 ./build/linux-tracy-gui/bin/vss_unit_tests
```

Every test case runs in a zone named after it, so `VSS_PROFILE=1` on any test
program (or `--tracy` on `vss_unit_tests` and `vss_system_tests`) gives each
test's duration and the zones it called.

## Sanitizers

Every CTest suite runs under a sanitizer, the GUI suites included, at -O1 (/O1 on
MSVC), and the first finding fails the run.

| Preset | Sanitizers | Runs | Where |
|---|---|---|---|
| `linux-asan-ubsan-gui` | AddressSanitizer, LeakSanitizer, UBSan | Everything: `unit`, `system`, `headers`, every `gui_*` suite, the fuzz replays | Pull request gate (Clang); nightly (GCC) |
| `linux-tsan-gui` | ThreadSanitizer | The same list | Gate when threading code changed, and every merge-queue entry (Clang); nightly |
| `windows-msvc-asan-gui` | AddressSanitizer | The same list | Nightly |

MSVC has no UBSan, ThreadSanitizer or LeakSanitizer. The plain presets
(`linux-asan-ubsan`, `linux-tsan`, `windows-msvc-asan`) build without the GUI and are
the quick local check. A new test is picked up with no change here, as the presets run
the whole of CTest. The GUI runs on `QT_QPA_PLATFORM=offscreen`.

```bash
cmake --preset linux-asan-ubsan-gui        # or linux-tsan-gui, windows-msvc-asan-gui
cmake --build --preset linux-asan-ubsan-gui
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --preset linux-asan-ubsan-gui
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:ignore_noninstrumented_modules=1:suppressions=$PWD/tests/tsan.supp \
  ctest --preset linux-tsan-gui
```

Run the system tests with a serial pair, as CI does on Linux (`tools/pty_loopback.py`
prints two device paths; set `VSS_LOOPBACK_A` and `VSS_LOOPBACK_B` to them, and
`VSS_REQUIRE_LOOPBACK=1`). Use the pinned Qt
(`.github/toolchain-versions.json`): a distribution Qt of another version can differ in
what it compiles, and CMake may pick it up in place of the kit in `QT_ROOT_DIR` unless
you pass `-DQt6_DIR=<kit>/lib/cmake/Qt6`.

* **Timeouts.** A sanitizer build runs several times slower, and the GUI self-test takes
  about ten minutes under MSVC AddressSanitizer. `VSS_SANITIZER_TIMEOUT_FACTOR` (default 5)
  multiplies every test timeout in a sanitizer build. The timing tests (`system: a polled
  bus ...`) measure real replies, so run them on an idle machine.
* **Qt and ThreadSanitizer.** Qt comes as a prebuilt kit that is not instrumented, and its
  locks and queues use futexes and atomics that ThreadSanitizer cannot see. Without help it
  reports races between Qt's own allocations in its thread pool, and in events that a
  worker posts to the GUI thread through a queued connection, though Qt orders them
  correctly. `ignore_noninstrumented_modules=1` stops it tracking accesses made inside Qt
  and keeps checking every access made by this project's code, so a race between the hub
  or endpoint threads and the GUI thread in our own code is reported. A deliberate race in
  a small program still fails under the same setting.
* **Suppressions.** `tests/tsan.supp` (passed as `suppressions=` in the CI jobs) has one
  entry, for the construction of Qt's `QCallableObject` slot object, which a worker builds
  for a queued call and the GUI thread runs. The header template is compiled into our code
  and so is instrumented, but the queue lock that orders the two is inside Qt. The entry
  matches only that constructor, not the lambda bodies, so a race on the data a queued
  lambda touches is still reported. Add an entry only with a reason that holds for every
  report it can match.

## Static analysis

Five analysers read the source without running it. Each has one local command,
`tools/analyze.py`, that CI runs too, so a clean local run is a clean job.
They cover the libraries, the command line tool, the Qt GUI, the tests and the
workflows.

| Analyser | Looks for | Local command | Runs in CI |
|---|---|---|---|
| clang-tidy | bugprone, concurrency, performance, portability, misc and clang-analyzer defects, and the naming rules | `python tools/analyze.py clang-tidy` | `static-analysis.yml`: changed files on a pull request, everything nightly |
| cppcheck | warnings, performance and portability | `python tools/analyze.py cppcheck` | `static-analysis.yml`, as clang-tidy |
| MSVC `/analyze` (PREfast) | defects specific to the MSVC build | `python tools/analyze.py msvc` | `msvc-analysis.yml`, nightly |
| zizmor | workflow injection, unpinned actions, excess permissions | `python tools/analyze.py zizmor` | `ci.yml` gates on it; `static-analysis.yml` uploads it |
| CodeQL (C++, Python, Actions) | security and quality queries | not run locally | `codeql.yml`, nightly |

```bash
cmake --preset linux-debug-gui                  # windows-msvc-debug-gui elsewhere
cmake --build --preset linux-debug-gui          # the GUI includes generated headers
python tools/analyze.py clang-tidy cppcheck zizmor
python tools/analyze.py clang-tidy --changed    # only the files changed since origin/develop
python tools/analyze.py msvc                    # Windows; needs Visual Studio
python tools/analyze.py all                     # everything this machine can run
```

`clang-tidy` and `cppcheck` read the `compile_commands.json` of the newest tree
under `build/`, or the one named with `--build`. Build a `*-gui` preset first, so
the GUI and the tests are in it. A tool that is not installed is a SKIP in
`all`, and a failure when named. Install them with LLVM (clang-tidy),
`winget install Cppcheck.Cppcheck` or `apt install cppcheck`, and
`python -m pip install -r requirements/zizmor.txt`. On Windows the script loads
the Visual Studio environment itself.

`msvc` configures the `windows-msvc-analysis` preset, which is the Debug GUI
build with `/analyze` and the `NativeRecommendedRules` rule set, builds it, and
reads the `.sarif` file each compile leaves beside its object file. Analysis
warnings do not stop that build. CI uses Microsoft's `msvc-code-analysis-action`
with the same rule set over the same sources.

Every run writes `build/analysis/<analyser>.sarif`. CI uploads the same files
to GitHub code scanning, so a finding is an alert under Security > Code
scanning (tools `clang-tidy`, `cppcheck`, `zizmor`, `PREfast` and `CodeQL`)
and, for a pull request, an annotation on the changed line. None of these jobs
is a required check; a finding is fixed or dismissed in code scanning with a
reason. A nightly failure on `develop` also opens one `nightly-analysis` issue.

The clang-tidy check set is the `CLANG_TIDY_CHECKS` constant in
`tools/analyze.py`, and the naming rules are in `.clang-tidy`. These five checks
are left out on purpose:

* `misc-const-correctness` asks for `const` on almost every local; it is style,
  not a defect.
* `performance-enum-size` asks for narrower enum bases; the saving is a few
  bytes.
* `bugprone-unused-return-value` flags the `error_code` out-parameter calls that
  ignore a result on purpose (`non_blocking(true, ignored)`).
* `bugprone-signed-bitwise` flags the POSIX flag macros (`O_RDWR | O_NOCTTY`,
  termios bits) and byte arithmetic that C++ promotes to `int`. Neither can be
  written another way.
* `portability-avoid-pragma-once` asks for include guards. The project standard
  allows `#pragma once` and counts every guard against the ratchet in
  `tools/preprocessor-baseline.json`.

`CLANG_TIDY_CHECKS` also turns off `bugprone-easily-swappable-parameters`,
`concurrency-mt-unsafe`, `misc-include-cleaner`,
`misc-non-private-member-variables-in-classes` and `misc-no-recursion`.

CI pins a newer LLVM than most machines have (`.github/toolchain-versions.json`) and
builds on Linux, so it can report checks and platform code that a local run does
not see, such as `performance-use-std-move` or the POSIX permission code. The
nightly run and the pull request job are the reference.

Fix a finding in the code. A suppression (`// NOLINT(check)` or
`// cppcheck-suppress id`) names the check and carries the reason on the same
line.

## Fuzzing

The topology and endpoint URI parsers and the decoder of the agent's protocol are
fuzzed. Their corpora are replayed by
CTest in every build; see [Fuzzing](fuzzing.md) for the libFuzzer targets and
where CI runs them.

## The UI suite

The UI suite drives the real main window with simulated input and nothing
else: QTest mouse clicks, key presses, drags, context-menu events and drops are
sent to the widgets, and the dialogs they open are answered the same way. It
checks what a person would see (the Simple page, the Ports dock, the canvas,
the Send and Monitor docks) and what the backend holds, and then takes the
thing it made away again. It runs offscreen (`QT_QPA_PLATFORM=offscreen`) on
the in-memory backend, with a separate group on the real engine, and uses the
project's own framework (`tests/framework`); QTest supplies only the input.

```bash
ctest --preset linux-debug-gui-ui          # every UI test, one at a time
ctest --preset linux-debug-gui -R ui_edge  # one group
ctest --preset linux-debug-gui -LE ui-engine   # all but the real engine
./build/linux-debug-gui/bin/vss_gui_ui_tests --list
./build/linux-debug-gui/bin/vss_gui_ui_tests --verbose --filter="share my device"
```

`--filter` takes part of a journey's name and may be given more than once.
Presets for the other platforms are `macos-debug-gui-ui`,
`windows-msvc-debug-gui-ui` and the `-release-gui-ui` forms. The older
`gui_journeys` test (seven journeys, named `journey: ...`) runs under the same
label.

### The journeys

| Group (CTest, label) | Journeys | What they cover |
|---|---|---|
| `ui_simple`, `ui-simple` | One per job of the Simple page: give my program a serial port, connect two programs, use my device through a new port, share my device with several programs, try my device from this window, let other computers use my device, put several on one line, send several into one, let programs take turns | Each starts at the window, chooses the tile, fills the form, clicks Create, checks the ports and links in the backend, on the setup page and (through the View menu) in the Ports dock and on the canvas, and removes the setup with Remove setup and its confirmation |
| | change a setup; close one program of a shared device; see the data on a port; check this PC is ready; start from an example, then remove everything; cancelling leaves nothing behind | Change and Apply, the Add a program and Close buttons of the setup page, the data box, the setup check, the templates dialog, Remove all, and every Cancel |
| `ui_advanced`, `ui-advanced` | the toolbox offers every tile; add a port, a fixture and a loopback; add every bundle (pair, split, join, bus, switch, redirect, monitor, bench); add by dropping a tile and from the canvas menu | The toolbox, the Add sheet and its defaults, and the three ways to add |
| | link and unlink ports by dragging a wire; link selected ports from the selection menu; move nodes by dragging and with the keyboard; the right-click menu of a node and of a link | Wire chooser, loop refusal, clicking a line, Split from the selection menu, drag, arrow keys, Fit, zoom, Tidy layout, Reverse, Duplicate, Delete |
| | send once and repeat from the Send dock; the Monitor shows traffic and can pause and clear; undo and redo (menu, shortcut, after a delete) | A repeat stays on the port it started on when the selection changes; Pause, Resume, Clear |
| | save a workspace, start over and reopen it; attach a real device; put a device on the network; share a device with the wizard; connect ports with the Connect dialog; rename, change the settings of, and wire a port | File menu with Qt's file dialog, every flow dialog |
| | every Simple job can be built by hand and Simple knows it | The same outcome by hand on the canvas is recognised as that job in Simple mode |
| `ui_edge`, `ui-edge`, and `ui_edge_200` | cancelling every dialog; empty and duplicate names; a failing backend; adding many items; links between every pair of port types; rapid clicking; small and large windows; text at 200 percent; both themes; removing everything while a repeat runs; a status message does not draw over the counters | `ui_edge_200` runs the group again with `QT_SCALE_FACTOR=2` |
| `ui_actions`, `ui-actions` | every menu entry in Simple mode; every menu entry in Advanced mode; edit the setup as JSON and apply it; the JSON editor explains each kind of mistake | Walks every enabled entry of every menu, looks at each dialog it opens by trying its buttons, check boxes and lists (not those that remove, install or restart), and closes it; the JSON editor takes pasted text, validates it, formats, reverts and applies |
| `ui_setup`, `ui-setup` | the first screen names the missing piece and offers the set-up; a port that cannot be made says which piece is missing; a refusal with nothing wrong in the set-up is in words, with details; a PC that is ready is not asked to set up | One run for each state: no driver, no helper service, a stopped one, an old one, one that refuses the user. The window is started as a person starts the program, the banner or the error is read, and "Set up Virtual Serial Studio" is chosen. The dialog is read and closed; the set-up is never run, because it would show a Windows permission prompt |
| `ui_engine`, `ui-engine` | link a loopback to a fixture and keep working; bytes cross a link and are counted; a repeat keeps the window responsive; Simple mode builds and removes a pair; save, tear down and reopen a workspace | The real `vss::studio` and `EngineStudioBackend`: the hub threads, the event bridge and the GUI thread together. Only in-process ports are used, so no driver or device is needed; the Simple journey skips itself when the Studio driver is installed, because it would make real COM ports |

### Responsiveness

Every journey is also a responsiveness test. `Probe` (in `ui-tests/ui_harness`)
beats before and after each simulated input and from a timer while the event
loop runs. A gap in which the GUI thread computed for longer than the budget,
or in which it waited for ten times the budget, is a handler that blocked the
window, and the journey fails at its end, naming each slow step with the time
and the CPU time of the gap. Only computing is held to the plain budget:
on a busy machine every wait grows, and what the budget judges is the
application's own work. A long wait still fails, which is how a lock a freeze
sits in shows. A second thread ends a journey that stays in
one step for 30 seconds (`VSS_UI_HANG_MS` changes it) and prints the step, so a
freeze fails with a name and not at the CTest timeout; a crash, an uncaught exception or a failed debug
assertion prints the journey and its steps in the same way.

Each journey has a budget in `apps/vss_gui/ui-tests/perf_budgets.json`
(150 ms unless it lists one; a journey that runs several cases and names each after
a colon shares the entry of the journey), written for a release build; CMake multiplies it
by 12 for a debug build, 6 under AddressSanitizer, 25 under ThreadSanitizer
and 60 for a coverage build (`VSS_UI_BUDGET_SCALE`).
`VSS_UI_BUDGET_MS` replaces the table with one number, `VSS_UI_BUDGETS_FILE`
names another table, and `VSS_UI_TRACE=1` prints each step as it starts. The table is a ratchet: a journey that is
slower than its entry fails, and an entry is lowered when the application gets
faster, never raised to make a journey pass. `VSS_UI_PERF_REPORT=file.csv`
appends a line per journey (time, steps, worst wall and CPU time of the GUI
thread, budget), which CI prints as a table in the job summary. A step that handles many
things at once (deleting fifty ports, restyling every widget for a theme)
declares it with `allow_bulk(items)` and gets a quarter of the budget more
for each item. A step that hands the thread to the operating system on
purpose, such as Qt's file dialog listing the drives, uses `tolerate(true)`.

### When a journey fails

The output lists the steps taken, and the harness writes a PNG of the window
(and of the dialog on top, if one is open) to `ui-test-failures` in the build
directory (`VSS_UI_FAILURE_DIR` changes it). The PNGs are not committed; CI
keeps them as the `ui-test-failures` artifact for 14 days. A known failure is
marked with `expect_failure("reason", body)`, which reports it as `[XFAIL]` and
fails the journey the day it stops failing, so the marker is removed with the
fix; a test is never disabled.

### Running them

GUI tests must be run alone. CTest's `RUN_SERIAL` keeps it from starting any
other test beside a UI test, but a second `ctest` or an IDE test run next to
it is still a second process. Their `QSettings` are kept in a directory of the
process (`tests/framework/qt_settings.hpp`, used by every GUI test program), so
two programs no longer clear each other's saved mode and recent files, which
once left a window waiting for the "save changes?" prompt of another test.
Windows still shares the clipboard, the file dialogs' recent places and the
window station, so on Windows run them with nothing else going on.

CI runs the groups with `ctest --preset linux-release-gui-ui` in the
`Qt GUI (offscreen self-test)` job, and the sanitizer jobs run them too. The
groups take about two to five minutes together on a release build (more
on a busy machine and about ten times that in a debug build; the job summary
of the `gui` leg lists every journey).

