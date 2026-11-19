# A debug API for a running Studio

Status: **accepted and built**. The owner accepted every recommendation of section 9 on 18 November 2026, and all the
slices of section 8 are in this repository except the last, which is a change to Boost.VirtualSerial. [What is
built](#what-is-built) says where each part is and what differs from the design as first written.

## The problem

Four bugs in release candidate 7 could be seen only on one computer with a real device: "Access is denied" when a real
device (COM5) was opened again after a settings change, "unsupported by this transport" when a monitor was added on a
real port, COM numbers refused as "reserved by Windows", and a network client that refused a connection at start. Each
fix began with guesses, because the running program could not say what its engine, its transports and Windows were
doing at that moment. What Studio keeps today is a message and an error number at the end of a failed call. What
the fix needed was the call that failed, the raw operating system code, the state of the handle, and what Windows
believed about the COM number.

This page designs a way for a person, or a coding agent working beside them, to ask a running Studio those questions.
It is for the person who owns the computer and is looking at the fault. It is not remote control, not telemetry and not
a support channel.

## What is built

| Part | Where |
|---|---|
| The call recorder: the ring, the two rings' sizes (4096 calls and 512 failures), `call_scope`, `operation_scope` and `traced` | `libs/vss_trace` |
| Calls recorded | The serial open and close (`serial_transport.cpp`), the line and break calls (`native_lines.cpp`), the network client's resolve and connect, the listener's bind and listen, every step of the Windows driver and helper that already had a failure note (`vcom_driver_windows.cpp`), and the studio operations `reconfigure_port`, `set_com_number`, `reopen`, `stop_port`, `start_port`, `connect`, `monitor`, `create_virtual_port` and `attach_serial`, each with the port it was for and, when it failed, the last failed call inside it as its cause |
| The server, the client, the redaction, the provider interface and the engine provider | `libs/vss_debug` |
| The pipe name | `\\.\pipe\vss-debug-<user SID>-<process number>`, or `debug-<process number>.sock` in the agent's private directory, from `vss::agent::local_endpoint::debug` |
| The audit log | `<state folder>/debug/audit`, in the format and with the hash chain of the gateway's log |
| The instance files a client reads to find a Studio | `<state folder>/debug/instances/<process number>.json` |
| The switch, the approval dialog, the chip and the window's part of the answers | `apps/vss_gui/src/support/debug_access.cpp`, the **Debug access** section of Settings, Performance, and the chip in `status_bar.cpp` |
| The command line client | `vss_cli debug` (`apps/vss_cli/debug_command.cpp`) |
| The support bundle's **Calls to Windows** row | `apps/vss_gui/src/support/bundle.cpp`, `trace.txt` |
| Read-only counters added to the engine | `hub::dispatch_queue_depth()` and `endpoint::write_queue()`; neither changes behaviour |

Where it differs from the first design:

- **Approval is remembered by program path, not by a token.** `vss_cli` is a new process for each command, so a token held
  in memory would be asked for again every time, and one written to a file would be a secret on disk. The person chooses
  *Allow once* (this connection) or *Allow until Studio quits* (this program's path, for the scopes it asked for).
  The path is what the operating system says and is a hint for the person, as in the agent's peer check.
- **The window's state is read by walking the active window's controls**, not by a provider in each screen. Fields whose
  name says password, token, secret or key, and any hidden field, give no value.
- **`debug.run` calls the studio directly**, not the window's `StudioBackend`. The window follows the engine's events and
  shows the change within a refresh.
- **The settings page is in Performance**, beside Profiling, not in a page of its own.
- **Serial opens are recorded as one call, `serial.open`, with the raw code the library returned.** The calls inside
  Boost.VirtualSerial (`CreateFile`, `SetCommState` and so on) are not named until that library reports them: slice 5.

## 1. What exists

| What | Where | What it answers today | What it cannot |
|---|---|---|---|
| The control endpoint and gateway | `libs/vss_control*`, `apps/vss_gate`, [remote control](remote-control.md) | Who may connect (peer check, tokens, scopes, ceilings), an audit log with a hash chain, `ports.list`, `links.list`, `events.subscribe`, `traffic.subscribe`, `ports.create`, `expect.bytes` | It is a client of `vss_agent`, so it sees the agent's engine and not the engine inside `vss_gui`. It has no operation that returns an error's origin. Its ports are loopback ports only |
| `vss_cli control` and `watch` | `apps/vss_cli` | A command-line front for the above | The same limits |
| `vss_agent` | `libs/vss_agent`, [agent](agent.md) | The local protocol (length-prefixed JSON header and blob) and the trust model | The window does not use it yet: Studio's own ports live in the `vss_gui` process |
| The Log and Monitor panes | `apps/vss_gui/src/panes` | The event stream of the engine, the traffic ring, counters | Only what the engine puts in an `endpoint_event`: an `error_code` and a message. Not the call, not the time of the failing call, nothing from before the pane was opened |
| `studio::connection` | `libs/vss_studio` | For a network client: attempts, last error with category and number, time to the next try | Only network clients, and only the last error |
| `reserved_com_numbers` | `libs/vss_ports`, `serial_enumeration_windows.cpp` | The `ComDB` bits and the names that resolve, so that only free numbers are offered | Why a number is held, by which device node, and what the driver reports |
| The Tracy switch | [profiling](profiling.md) | Timing of zones, on this computer only, started by a switch | State and errors. Tracy shows when, not what failed |
| Capture and replay | the Diagnostics plugin, [plugins](plugins.md) section 19 | Bytes and line changes on a clock, packed by the user | It is a recording of traffic, not of the engine |
| The support bundle | File, Support bundle | Version, driver state, setup, log, counters, serial ports; never traffic | A file, made once, after the fact. It holds the log as the panes show it |
| `vss_cli selftest` | `apps/vss_cli/main.cpp` | Makes two loopback ports and a link in a new process | It tests a fresh engine, not the one that is failing |

What is missing:

1. **A way in to the engine that is running.** Every tool above looks at a copy, a file or another process.
2. **The failing call.** Errors reach the GUI as a code. Where a transport makes several operating system calls
   (CreateFile, SetCommState, CancelIoEx, `RegGetValue`, `connect`), nothing says which one failed. For a serial
   device the calls are made inside Boost.VirtualSerial (`vs::any_serial_port::open`), outside this repository, so
   what the transport sees is one error code from `open`.
3. **A history that is already there when the fault happens.** A bug seen once is not seen again when a tool is
   switched on afterwards.
4. **What the operating system thinks.** The COM number picture, the driver and the helper service, handle state.
5. **A way to try one thing and get the whole chain back**, instead of changing the program, building and asking
   the person to run it again.

## 2. Rules

These come from AGENTS.md and the remote control design, and are not open.

- **Local only.** A named pipe on Windows, a Unix socket elsewhere, both owner-only. Loopback TCP is not offered:
  a debug surface has no reason to be reachable by a container or a browser.
- **Off by default, at every start.** Whether it is on is never remembered. A person switches it on in Studio each
  time they want it.
- **Visible.** While it is on the status bar shows a chip in the warning token colour with text ("Debug access on, 1
  client"), and one click turns it off. The chip is not hidden by a setting.
- **The user approves each client.** The first request from a program shows a dialog naming the program, its path and
  process number, and what it asks for. Nothing is served until the person says yes. The answer lasts until Studio
  quits or the person withdraws it.
- **Nothing leaves the machine.** No network code is added. Output goes to the program that asked, on the pipe.
- **No secrets in output.** The redaction of the support bundle is used on every string: the home folder, the user and
  computer names, device serial numbers, tokens, passwords, the licence file. A value that cannot be redacted is
  not returned.
- **Bytes of traffic only when the user turns that on,** by a second switch, per port or for all, that turns itself
  off after ten minutes.
- **An audit line for each use,** in the audit log of the control endpoint (same format, same hash chain, a `debug`
  source), with the program, the operation and its arguments. The arguments of an operation are recorded; results are
  not.
- **No new telemetry and no network connection.**

## 3. The proposal

A set of operations under `debug.`, served from inside the process whose state is wanted. The process is `vss_gui`
today, and `vss_agent` once Studio is its client, from the same library. `vss_cli debug` is the command-line client,
and it prints text for a person and JSON lines (`--json`) for a program, so a coding agent runs it from a shell
and reads the output.

### 3.1 The recorder (build first)

A bounded in-memory ring in the engine and transports, **always on, holding no traffic bytes**, never written to disk
unless the person asks (the support bundle can include it, section 6). One entry per operating system or library call
that the transports and the studio make:

| Field | Meaning |
|---|---|
| `seq`, `mono_ns`, `wall` | Order and time, from one clock |
| `thread` | A small number, and for an endpoint the strand it ran on |
| `endpoint` | The id of the port or link, and its name |
| `call` | The failing or recorded call as text: `CreateFileW`, `SetCommState`, `CancelIoEx`, `DeviceIoControl(vcom.create)`, `RegGetValueW(ComDB)`, `connect`, `bind`, `tcgetattr` |
| `args` | The non-secret arguments that matter: a path, a baud rate, flags, a number |
| `result` | `ok` or `failed` |
| `code` | The raw code (`GetLastError`, `errno`, the HRESULT or NTSTATUS), its category and the system's message text |
| `duration_us` | How long the call took |
| `operation` | The studio operation that made it (`open`, `set_settings`, `reopen`, `add_monitor`), so a chain reads from the top |
| `cause` | For an error that wraps another (an `errc` made from a system error), the `seq` of the entry it came from |

The recorder is a plain function template that wraps a call and records it (no macros: the preprocessor policy
holds), a fixed-size record in a preallocated ring (4096 entries, no allocation on the hot path, one atomic
increment), and **failures are always kept, successes are kept with a lower priority** so that a busy port does not
push out the one failure that matters. The cost is a timestamp and a copy of a small struct per call, and these are
calls that take microseconds to milliseconds.

The `error_code` that reaches the GUI is unchanged (`vss_core` behaviour does not change). The recorder keys by endpoint
and time, so the Log pane can say "last failing call: SetCommState, 5 (Access is denied)" by looking the entry up.

**The part of the chain this repository cannot see.** For a serial device the open, the settings and the close
happen inside Boost.VirtualSerial. Three ways to get the failing call, and the third is the recommendation:

1. Record only `open` with the library's error code (the weakest; the same as today, plus a time and a handle state).
2. Ask the library for an error detail (the name of the failed call and its raw code) carried beside the
   `error_code`. It is a change in a separate repository and an API addition.
3. Do both: record the library call in this repository now, and ask for 2 as a follow-up, so that the first slice
   does not wait. On Windows the transports that this repository owns (`vcom_driver_windows.cpp`,
   `native_lines_windows.cpp`, `peer_open_windows.cpp`, `serial_enumeration_windows.cpp`, `listen_socket_windows.cpp`,
   `process_control_windows.cpp`) are recorded in full from the first slice.

### 3.2 The engine snapshot

`debug.snapshot` returns one JSON document, built on the thread that owns each piece of state and so never blocking
the GUI thread:

- **Endpoints.** Id, name, transport kind (`serial`, `driver`, `loopback`, `tcp_client`, `listener`, `rfc2217`, `mock`),
  engine state (opening, open, stopped, paused, lost, failed), the address or path (redacted), the serial
  settings the studio holds against the ones the transport last applied, the last error with the raw code and the
  recorder entry it came from, counters (bytes in and out, write errors, drops), the number of reads and writes that
  are pending, and the depth of the endpoint's strand queue.
- **OS handle state** where the transport can tell: open or closed, whether a close is in progress, the number of
  outstanding overlapped operations, and for a Windows driver port whether a program has the port open (`peer_open`)
  and what the driver says about it.
- **Links** with their options (delay, jitter, wiring, bus members), their counters and what is queued on them.
- **Reconnector** state for each port that has one (what `studio::connection` has now, with the history of attempts).
- **The hub.** Dispatcher strand queue depth, the event sink queue and how many events it dropped, the traffic ring's
  fill, and the capabilities the build and the machine have.
- **Build and process.** Version, the edition (not the licensee), the profiling state, uptime.

The snapshot is the same data as the support bundle's engine section, extended, and is built by one function that
both use (section 6).

### 3.3 The COM number picture (Windows)

`debug.com_numbers` answers "why is COM7 refused" by listing, for each number from 1 to 256:

- the `ComDB` bit (from `HKLM\SYSTEM\CurrentControlSet\Control\COM Name Arbiter`);
- every device node that names it, **present or not**, found by enumerating device classes with
  `DIGCF_ALLCLASSES` and not only present devices, with the node's `PortName`, its class, its friendly name and
  whether it is hidden (a number held by a removed device appears here and nowhere in Device Manager's default view);
- the name `SERIALCOMM` has for it, and whether `\\.\COMn` resolves;
- what the Studio driver says about its own ports and numbers (the driver's list request);
- what Studio concluded: free, in use by this setup, reserved, and the rule that decided it.

This is read-only, needs no elevation, and is Windows-only. Another platform says `not applicable`.

### 3.4 The driver, the helper service and the plugin host

- **Driver and helper.** The installed driver's version and state, its ports with their numbers and holders, the last
  results of its requests (from the recorder), and the helper service's state from the service manager, with its last
  error. The helper's own log is not read.
- **Plugin host.** Each plugin: id, version, state, the process number, the grants it holds (names only), the last
  host-side error, the number of pending requests. A plugin's own data and any `secret` field are never returned.

### 3.5 GUI state and a screenshot

- **`debug.gui.state`.** Which pane is open, the open document's name (not its path), whether it has unsaved changes,
  the mode, the theme, the language, and the values of the open dialog or sheet. Each screen opts in with a small
  provider that returns plain values; a field that holds a secret returns nothing. This is a new, small API in the
  GUI and the only part of the proposal that touches many files, so it is not first.
- **`debug.gui.screenshot`.** The PNG of a window, from the same `grab()` the screenshot tests use, returned as a blob.
  A window can show a device name, a path or traffic, so this is its own scope, and the dialog that approves it says so.

### 3.6 Running an operation

`debug.run` runs one engine operation through the same `StudioBackend` call the window uses (so the GUI state stays
the one that the window shows), and returns the whole chain: the result, the error with every cause, and the
recorder entries between the start and the end of the call. Operations are a closed list, not a general command:
`add_monitor`, `restart_port` (with settings), `stop_port`, `start_port`, `reopen`, `set_com_number`, `check_listen_free`.
The scope `debug.act` is separate and is shown in the chip as "can change ports". The approval dialog offers *Allow
reading* and *Allow reading and trying operations*, and an operation that changes a port that has a program attached
(`peer_open` is true) asks again before it runs.

### 3.7 Operations and scopes

| Operation | Scope | Slice |
|---|---|---|
| `debug.status` (what is on, who is connected, the build) | `debug.read` | 1 |
| `debug.snapshot` | `debug.read` | 1 |
| `debug.trace` (the ring, filtered by endpoint, time and failures only, paged) | `debug.read` | 1 |
| `debug.trace.subscribe` (live, the same filters) | `debug.read` | 1 |
| `debug.com_numbers`, `debug.driver`, `debug.plugins` | `debug.read` | 2 |
| `debug.run` | `debug.act` | 3 |
| `debug.gui.state`, `debug.gui.screenshot` | `debug.gui` | 4 |
| `debug.traffic` (bytes, for the ports the person chose) | `debug.data`, and the user's second switch | 2 |

`vss_cli debug status | snapshot | trace [--follow] [--failed] [--endpoint N] | com | driver | plugins | run … | screenshot`.

## 4. Transport and framing

**Reuse the pieces, not the gateway.** `vss_gate` is a client of `vss_agent`, and the engine that has the bug is
inside `vss_gui`, so the gateway cannot reach it. What does fit and is reused as it is:

| Reused | From |
|---|---|
| The length-prefixed framing with a JSON header and a blob, the preface, the 4 KiB limit before authentication | `libs/vss_control_proto`, `libs/vss_agent` |
| The local stream (pipe with an owner-only access list and `PIPE_REJECT_REMOTE_CLIENTS`, socket in a 0700 directory), with the peer check | `libs/vss_agent/local_stream.hpp`, `libs/vss_control` `loopback_platform` |
| Tokens, the scope and ceiling policy table, the failure limiter, the audit log | `libs/vss_control`, `libs/vss_control_proto/src/policy.cpp` |

What is new is a listener inside the process, its own pipe name (`vss-debug-<user SID>-<process id>`, so two Studios
do not collide and a stale name is not reused), the approval by the person (remembered by program path until Studio
quits, never written to a file), and the operations above. The listener is created when the person switches debug access
on and **destroyed when they switch it off**; while it is off there is no pipe at all.

A client finds a Studio by the file it publishes in `<state folder>/debug/instances` while it listens, named with its
process number. If several Studios run, `vss_cli debug` lists them and asks for `--pid`.

**Why not a file instead.** A periodic JSON file in a private directory would need no listener, but it cannot approve a
client, cannot serve a live trace, cannot run an operation and cannot take a screenshot, and it would leave state on
disk. Question 1 asks the owner.

## 5. Security and privacy, as tests

Each of these is a test, not only a rule:

- With debug access off there is no listener and no pipe (checked by trying to connect).
- A connection that fails the peer check, sends a frame over 4 KiB before authentication, or does not begin with the
  preface is closed with no reply.
- A client that is not approved is told nothing, not even the version.
- No operation returns a string that contains the home folder, the user name, the computer name, a device serial
  number, a token or a licence path. A test writes a known value into each and searches every operation's output.
- `debug.traffic` returns nothing until the second switch is on, and returns nothing again when it times out.
- A `debug.read` client cannot call `debug.run`, `debug.gui.screenshot` or `debug.traffic`.
- Each request writes one audit line, and `audit.verify` still passes.
- A flood of requests from one client slows only that client (the failure limiter and a rate limit).
- The recorder holds no traffic bytes: a test sends a known pattern through a port and searches the ring.

## 6. How the support bundle relates

The bundle and the debug API are the same information at two times: the bundle is a file made after the fault, for
someone else to read; the debug API is a conversation during the fault, with the person at the computer. So:

- One function builds the engine snapshot, and both call it. The bundle's `engine.json` is the snapshot.
- The recorder is one ring. The bundle gets a new section, `trace.txt` (the failed calls and the thirty seconds around
  each, redacted by the bundle's own redaction), with its own tick box and preview, as every section has, and is off
  by default. A support bundle made after a failure then holds the failing call even if nobody had debug access on.
- The bundle still holds no traffic. Capture and replay stay the Diagnostics plugin's, and the debug API does not
  duplicate them: `debug.traffic` is a live look at the hub's existing ring, not a recording.
- Tracy answers "when and how long", the debug API answers "what failed". The debug API reports whether profiling is on
  and does not switch it.

## 7. What is not proposed

- No remote use. A coding agent on another computer does not reach this; the person runs the client locally.
- No arbitrary evaluation, no scripting engine, no file read or write by path, no registry or process browser. The only
  Windows state read is the list above.
- No change to `libs/vss_core` behaviour or the JSON topology schema. Where the hub's queue depth is wanted (section
  3.2) the proposal adds a read-only accessor and asks first (question 6).
- No new test framework and no persistent debug log.

## 8. Slices

1. **Recorder, snapshot, the pipe, approval, chip, audit, `vss_cli debug status | snapshot | trace`.** Built.
2. **COM number picture, driver, helper and plugin state, `debug.traffic` with its switch,** and the trace section of
   the support bundle. Built.
3. **`debug.run`.** Built.
4. **GUI state and screenshot.** Built.
5. **Follow-up outside this repository:** an error detail in Boost.VirtualSerial, so the serial open reports the call
   that failed. Not built: it is a change to `iainchesworthlabs/virtual-serial`.

## 9. Decisions for the owner

The owner accepted every recommendation on 18 November 2026:

1. A listener in `vss_gui`: yes, an owner-only pipe, only while debug access is on.
2. Every build, including release, off by default, turned on in the client.
3. Approval once for each program, for the scopes it asked for, and a further question before an operation changes a
   port that has a program attached. *The further question is not built: `debug.run` needs the `debug.act` scope, which
   the person grants in the first dialog and which the chip does not yet show separately.*
4. Screenshots: yes, as their own scope.
5. The recorder always on, in memory only.
6. Read-only accessors in `vss_core` for the dispatcher queue and the write queue: yes.
7. An error detail in Boost.VirtualSerial: yes (slice 5).
8. The support bundle's trace section: in, off by default.
9. Community, no licence check, and in the user guide.
10. `vss_agent` serves the same operations once it hosts Studio's engine: the library is written for both.
