# Virtual Serial Studio

**A tool for real, virtual and networked serial ports.**

Virtual Serial Studio opens one serial port and gives it to as many applications
as you like. It splits a device across several virtual ports, merges several
publishers into one, redirects traffic, bridges a COM port to TCP, RFC 2217 or
UDP, drives the modem control lines, and records everything it sees. It is
built on [Boost.Asio](https://boost.org/libs/asio) and on
[Boost.VirtualSerial](https://github.com/iainchesworthlabs/virtual-serial), the serial stream
abstraction that lets the transport be replaced.

[![CI](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/ci.yml)
[![Fuzz](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/fuzz.yml/badge.svg?branch=develop)](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/fuzz.yml)
[![Release](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/release.yml/badge.svg?branch=develop)](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/release.yml)
[![Docs](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/docs.yml/badge.svg?branch=develop)](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/docs.yml)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)

![Virtual Serial Studio](docs/images/studio.png)

## What it can do

| Capability | How |
|---|---|
| Share one serial port with many applications | Create N virtual ports and `split` the real one across them |
| Merge several writers into one device | `join` two or more publishers into one target |
| Give an application its own serial port | Pseudo terminal on Linux/macOS; on Windows the first-party `vsscom` driver, or a com0com pair when the driver is not installed |
| Replace com0com | The Windows driver is described in [docs/windows-com-driver.md](docs/windows-com-driver.md); its kernel-mode package is not signed yet, so Windows loads it only with test signing on |
| Bridge a device to the network | `tcp://`, `udp://` and `rfc2217://` endpoints are ports like any other; `tcp-listen://`, `udp-listen://` and `rfc2217-listen://` wait for other computers to connect |
| Test RS-232 control lines | DTR/RTS/BREAK are drivable, CTS/DSR/DCD/RI are reported, a null-modem link crosses them |
| Watch traffic without disturbing it | One-way `monitor` links and a bounded traffic ring buffer |
| Put several programs on one multi-drop line | A `bus` link: each port hears every other and never itself, with a `join` to a listen-only tap |
| Record traffic with exact times | `vss_cli capture` and the monitor's **Export capture** write every byte as `.cap`, CSV or JSON Lines |
| Script a device | The serial library's mock backend is exposed as an endpoint |
| Save and reload a topology | Validated JSON documents, opened by the GUI and by `vss_cli run` |
| Measure the data path | `vss_cli bench` and a CI performance suite with enforced floors |

## Documentation

- [docs/user-guide.md](docs/user-guide.md) — how to use the application: both modes, everyday jobs, network tools and troubleshooting.
- [docs/ux.md](docs/ux.md) — who the interface is for, the simple/advanced design, and the human-factors decisions.
- [docs/architecture.md](docs/architecture.md) — how the libraries, hub and transports fit together.
- [docs/scenarios.md](docs/scenarios.md) — worked recipes for share, join, bridge and loopback.
- [docs/profiling.md](docs/profiling.md) — Tracy zones and how to capture a trace.
- [docs/posix-virtual-ports.md](docs/posix-virtual-ports.md) — stable virtual ports on Linux and macOS.
- [docs/windows-com-driver.md](docs/windows-com-driver.md) — the optional Windows COM port driver.
- [docs/platforms.md](docs/platforms.md) and [docs/vm-testing.md](docs/vm-testing.md) — what is tested on each platform, and the VMware guest matrix.
- [docs/i18n.md](docs/i18n.md) — the languages and how to add one.
- [docs/building.md](docs/building.md), [docs/testing.md](docs/testing.md), [docs/ci.md](docs/ci.md), [docs/fuzzing.md](docs/fuzzing.md), [docs/profiling.md](docs/profiling.md) and [docs/releasing.md](docs/releasing.md) — build, test, CI, fuzzing, profiling and release.
- [docs/workstreams.md](docs/workstreams.md) and [docs/migration.md](docs/migration.md) — where the platform work stands, and the move to the shared repository standard.

The same pages are published as a site by the Docs workflow (`mkdocs build --strict`).

## Building

Requirements: CMake 3.21+, a **C++23** compiler, Boost 1.74+ headers and git. [docs/building.md](docs/building.md) has every preset, the options, Qt and the language standard.

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

## Quick start

### The GUI

```bash
vss_gui
```

An empty window offers four jobs: give my program a serial port, connect two
programs, share one device, and watch traffic. `Ports -> Add port` creates a
virtual port, a linked pair or a real device and tells you the path another
application should open. `Topology` links ports and shares a device with
several programs, and the monitor shows the traffic as it flows. Advanced mode
adds network endpoints, fixtures and the expert panels to the same window.
`File -> Save` stores the topology as JSON.

The GUI can also prove itself without a display or a user:

```bash
QT_QPA_PLATFORM=offscreen vss_gui --selftest
```

### The command line

```bash
vss_cli devices                     # serial ports and com0com pairs
vss_cli virtual                     # one virtual port; prints what to open
vss_cli vport --name=ttyVSS0        # a pseudo terminal with a stable name (Linux, macOS)
vss_cli loopback                    # a virtual port plus a monitor, no hardware
vss_cli share serial://COM3 --listeners=3 --seconds=30
vss_cli bridge serial://COM3 tcp://192.168.1.50:4001 --seconds=30
vss_cli run lab.json --seconds=60
vss_cli capture lab.json --out=run.cap --port=master --seconds=60
vss_cli bench --from=serial://COM3 --to=serial://COM5 --bytes=10MB
vss_cli selftest
vss_agent &                         # keep ports after the command that made them (a first slice)
vss_cli agent create-port demo      # a loopback port in the agent; `vss_cli agent ports` lists it
```

Run `vss_cli --help` for the full list.

### A topology file

```json
{
  "version": 1,
  "name": "lab",
  "ports": [
    { "name": "device", "kind": "physical",
      "uri": "serial://COM3?baud=115200",
      "serial": { "baud_rate": 115200, "parity": "none" } },
    { "name": "terminal", "kind": "virtual", "virtual_kind": "auto" },
    { "name": "logger",   "kind": "virtual", "virtual_kind": "loopback" }
  ],
  "links": [
    { "mode": "split", "sources": ["device"], "targets": ["terminal", "logger"] }
  ]
}
```

`vss_cli run lab.json` applies it; the GUI opens the same file.

## The model

Three ideas carry the whole application.

**An endpoint** is one transport with a name: a real UART, a pseudo terminal, a
com0com end, a TCP socket, an RFC 2217 session, or a mock device. It owns its
read loop, write queue, counters and control-line state, and it is driven on a
strand so a transport is never touched by two threads at once.

**A link** connects endpoints. `duplex` links carry traffic both ways, `one_way`
links carry it one way, and `monitor` links are passive taps that do not drive
the peer's modem lines. A single link can fan out to many targets (`split`) or
accept many sources (`join`), and every link can arbitrate between publishers:
either `interleave` them or give one publisher `owner`ship. A `bus` link puts
several endpoints on one shared multi-drop line: each hears every other and
never itself, and it carries no modem lines and takes no owner.

**A hub** owns the `io_context`, the endpoints, the routing table and the event
dispatcher. The `studio` layer above it adds the bits applications need:
advertised paths, composite scenarios, JSON persistence and com0com detection.

```
vss_cli / vss_gui
      │
   vss_studio      studio: scenarios, persistence, advertised paths
      │
  ┌───┴──────────────┬─────────────────┐
vss_config       vss_ports          vss_core
 JSON documents   PTY, com0com,     hub, endpoint, links,
                  native lines,     control lines, events
                  serial transport
```

## Virtual ports by platform

| Platform | Kind | What a third-party application opens |
|---|---|---|
| Linux, macOS | `pty` | `/dev/pts/N` — a real terminal that behaves like a serial port |
| Windows | `auto` with com0com installed | One end of a com0com null-modem pair, e.g. `COM5` |
| Any | `loopback` | An in-process device: useful for demos, monitoring and tests |

`auto` picks the best available kind and reports the path it chose. A pair is
never wired into the hub twice: once an endpoint claims a com0com pair, later
listeners fall back to loopback instead of fighting over the same device.

On Windows, creating a **new** com0com pair needs administrator rights, because
`setupc.exe` carries an elevation manifest. `vss::detect_com0com()` and the pair
list work without elevation, and the GUI reports the installation state.

## Control lines

Every endpoint models DTR, RTS, CTS, DSR, DCD, RI and BREAK. The front end
drives DTR, RTS and BREAK, and reports the rest. When two endpoints are linked
with `emulate_modem_lines` (the default), the link behaves like a null-modem
cable:

```
A.DTR ──► B.DSR        A.RTS ──► B.CTS
B.DTR ──► A.DSR        B.RTS ──► A.CTS
```

That is enough to exercise software that watches carrier detect, requires
hardware flow control, or treats a dropped DTR as a hang-up. The inspector in
the GUI drives the lines directly; `endpoint::set_line` does the same from code.
On a device that cannot support a line the call reports `errc::unsupported`
rather than pretending to work.

## Testing

Unit, system, performance, fuzz and GUI journey tests all run under CTest. [docs/testing.md](docs/testing.md) describes them, the coverage gates and profiling.

## Profiling

Every layer is instrumented for [Tracy](https://github.com/wolfpld/tracy): the engine and transports, the studio service, the CLI, the GUI (events, painting, panes), the tests and `vss_perf`. In a Debug build profiling starts by itself and listens on this computer only. In a Release build the instrumentation is compiled in too whenever the vcpkg toolchain supplies Tracy (`VSS_TRACY` defaults to on there; `-DVSS_TRACY=OFF` leaves it out), but dormant, and nothing runs, listens or costs anything measurable until you ask:

```bash
vss_gui --profile            # or Settings, Performance, Profiling; or VSS_PROFILE=1
vss_cli --profile run topology.json
vss_perf --tracy
```

The listener is bound to 127.0.0.1 unless you add `--profile-remote`, it never broadcasts, and it sends nothing until you connect a Tracy profiler. [docs/profiling.md](docs/profiling.md) has the options, the zones and the measured cost.

## Continuous integration

`.github/workflows/ci.yml` is the pull request gate. [docs/ci.md](docs/ci.md) describes the jobs.

## Changelog

Every change lands as one fragment in [`changelog.d/`](changelog.d/README.md):

```bash
python tools/changelog.py --check
python tools/changelog.py --dry-run
python tools/changelog.py --release 0.2.0
```

## Repository layout

```
libs/vss_core        hub, endpoints, links, control lines, events
libs/vss_ports       serial transport, PTY, loopback, com0com, enumeration
libs/vss_config      JSON topology documents
libs/vss_license     licence files and the signed envelope (verify, judge, sign)
libs/vss_studio      the application service shared by the CLI and the GUI
libs/vss_testkit     reusable fakes and helpers for tests
libs/vss_agent_proto the wire protocol of the per-user agent
libs/vss_agent       the agent's local transport, server and client
third_party          Monocypher, vendored unchanged (Ed25519)
apps/vss_cli         headless command line
apps/vss_agent       keeps ports running without the window (a first slice)
apps/vss_gui         Qt 6 desktop application
tests/               in-tree framework, unit and system suites
perf/                performance suite and floors
cmake/               dependency, warning, coverage and sanitizer helpers
drivers/windows      the vsscom kernel-mode and user-mode COM port drivers
tools/               Boost fetch, changelog, coverage, performance and Tracy
                     reports, fuzz and analysis drivers, the PTY pair helper,
                     CI planning and the VM harness
requirements/        pinned Python tooling for the docs and workflow analysis
docs/                user guide, design notes and scenario recipes
```

## Known limits

* Windows has no user-mode COM port API, so a real `COMx` needs either com0com
  or the driver described in [docs/windows-com-driver.md](docs/windows-com-driver.md).
  The application detects the driver, prefers it over com0com when present, and
  accepts `"virtual_kind": "windows-driver"` in saved topologies. The
  kernel-mode driver package is unsigned, so it loads only with test signing on;
  the user-mode package is test-signed in CI. The driver raises no line-error
  events (`EV_ERR`, `EV_BREAK`).
* A named-pipe virtual port is not implemented.
* Two loopback virtual ports cannot be linked duplex: a loopback device echoes
  its writes into its own receive path, which is a feedback loop. The GUI's
  null-modem demo therefore uses a one-way link, and real devices do not have
  the problem.
* Creating a com0com pair needs elevation; everything else runs unprivileged.
* Linking the two ends of a real null-modem cable through the hub is a physical
  loop, exactly as it would be with any serial forwarder. Attach one end and
  let the peer be the application.

## Roadmap

1. **Windows COM driver** — the serial contract, the setup script and the guest
   tests are done. What is left is signing the kernel-mode package and an
   installer (there is no MSI yet).
2. **Named-pipe virtual ports** — a cross-process port for applications that
   accept a pipe path, useful on machines without a driver.
3. **macOS `/dev/cu.*` aliases** — an opt-in helper that creates a symlink with
   authorisation, for software that hardcodes the name.

## License

Virtual Serial Studio is free software: you can redistribute it and/or modify
it under the terms of the **GNU General Public License, version 3 or later**
(`GPL-3.0-or-later`). It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. The full text is in [LICENSE](LICENSE).

Every first-party source file carries an SPDX licence line naming
GPL-3.0-or-later.

The dependencies keep their own terms, all compatible with the GPL:

* [Boost.VirtualSerial](https://github.com/iainchesworthlabs/virtual-serial) and Boost itself are
  **BSL-1.0**; BSL-1.0 is a permissive licence that can be combined with GPLv3.
* Qt 6 is **LGPL-3.0 / GPL-3.0 / commercial**; the GUI links it dynamically.
* `com0com` is an optional, separately installed Windows driver, invoked rather
  than linked.
* Three tools in `tools/` are derived from the Boost.VirtualSerial repository
  and remain **BSL-1.0**.

[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) records the details.

If you distribute binaries, section 6 of the GPL requires you to make the
Corresponding Source available to the people you distribute to.
