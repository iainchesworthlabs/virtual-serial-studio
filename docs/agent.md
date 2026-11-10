# The per-user agent

Status: **proposed**. This page is a decision record for the owner. Section 11 lists the decisions to make; nothing
outside the first slice (section 10, PR 1) is built until they are made.

Each claim below carries one of two marks:

- **[slice]** the first slice builds it and a test exercises it. Section 9 names the tests.
- **[paper]** designed here and not built.

## The problem

The engine runs inside the process that made the studio. Two rows of `docs/gui-design/docs/08-engine-gaps.md` follow
from that:

- **Ports that outlive the window.** A port lives as long as `vss_gui` or a `vss_cli` command does. Studio works
  around it by staying resident (`docs/gui-design/docs/10-running-in-the-background.md`, option A). A Studio crash
  still stops every port, and `vss_cli` has its own hub.
- **Ports another program made.** The hub is in-process, so one program cannot show, watch or use the ports of
  another.

The fix both rows name is option B of that page: a per-user program, `vss_agent`, owns the studio, and Studio and
`vss_cli` are its clients over a local pipe or socket.

## What is true today

These facts were read from the code and they constrain the design. Where a document said otherwise, the code is
followed.

| Fact | Where | What it means for the agent |
|---|---|---|
| `StudioBackend` is the one interface between the window and the engine: about 55 operations and 5 signals | `apps/vss_gui/src/backend/studio_backend.hpp` | It is the seam. A `RemoteStudioBackend` replaces `EngineStudioBackend` and no screen changes |
| Most operations are quick calls. Seven can take seconds and are already called from worker threads: `create_driver_port` and every create that reaches the driver, `set_com_number`, `set_port_settings`, `recreate_lost_ports`, `run_benchmark`, `attach_serial` and `share_serial` and `bridge` (a device or a network connection opens), `apply` | the same file; `vcom_driver_windows.cpp` waits up to 30 s for a port to appear | The protocol needs requests that answer late, with progress and cancel, and a client must never wait for one on its GUI thread |
| `ports()` and `links()` are caches. The adapter rebuilds them from the engine after each change and 50 ms after a batch of events, compares, and emits `ports_changed`, `links_changed` or `stats_changed` | `engine_studio_backend.cpp` (`rebuild`) | The remote backend keeps the same cache and the same comparison; only where the snapshot comes from changes |
| Events reach the GUI thread through `EventBridge`: a sink on the hub's dispatcher strand pushes into a queue of at most 8192 events (the oldest is dropped and counted), and a timer on the GUI thread takes them every 250 ms (50 to 5000). Nothing is merged: "coalescing" is batching by interval | `event_bridge.cpp` | The agent does the same per client, on its side of the connection |
| Large results: `traffic(limit)` (up to 8192 records with their bytes), `snapshot()` and `apply()` (a topology document), `ports()` | `studio_backend.hpp`, `hub_options::traffic_capacity` | Bytes travel outside the JSON; a frame has a size limit, so traffic history is paged |
| Some operations are composed in the GUI from several backend calls, with their own rollback: `add_program_port` (make a port, replace the link), `build_bundle`, `duplicate_ports`, the undo steps, and the search for a free COM number in `create_port_with_com_number` | `share_edit.cpp`, `canvas_edit.cpp`, `engine_studio_backend.cpp` | With more than one client these sequences can interleave, and a client that dies half-way leaves half a change. Section 5 moves them into the agent |
| Some operations do not touch the studio at all: `devices()`, `facts()`, `check_driver()`, `create_com0com_pair()` (which shows an elevation prompt) | `engine_studio_backend.cpp` | They stay in the client |
| A Windows driver port has two handles. The **lease** decides that the port exists: the control-device file object that sent `CREATE_PORT` (kernel-mode driver), or the client's pipe connection to `vsscom_helper` (user-mode driver). The **hub handle** carries the data; one is allowed at a time and any process of the same user may open it. When the process that holds the lease ends, the port is removed. While no hub handle is open the port loops back and both queues are cleared | `drivers/windows/vsscom/control.c`, `hub.c`, `vsscom_helper/main.cpp`, `vcom_driver_windows.cpp` | An agent crash removes every driver port. Handing a port from one process to another has no API, and would echo and lose bytes in between |
| `vsscom_helper` allows 16 ports and 20 connections for each user, and drops a connection that holds no port after 30 s | `vsscom_helper/main.cpp`, `protocol.h` | One agent for each user holds every lease, so these become the agent's limits |
| A pseudo terminal goes with the process. Its stable link is removed when the studio is destroyed, and is left dangling after a crash; the next port with that name replaces a link whose target is not a character device. Only `vss_cli vport` turns stable links on. The descriptors are not close-on-exec | `virtual_port_posix.cpp`, `stable_link.cpp`, `studio.cpp` | The agent turns stable links on, so a port's path survives a restart. The agent must not start other programs while this holds |
| Studio's single-instance lock is a `QLocalServer` named from a hash of the user **name**; start at sign-in is an `HKCU` `Run` value on Windows and an XDG autostart file on Linux, and nothing on macOS; the running setup is `running-setup.json`, written a second after each change and applied at start unless it says `stopped` | `single_instance.cpp`, `sign_in_start_*.cpp`, `running_setup.cpp` | The agent takes over the last two. Its own lock is described in section 3 |
| The installers hold fixed lists of programs (`vss_gui`, `vss_cli`), stop no running program and have no maintainer scripts. The Windows installer is per machine | `cmake/packaging/`, `tools/ci/check-windows-installer.ps1`, `check_linux_packages.sh`, `VssPackage-darwin-sign.cmake` | Installing `vss_agent` touches each list. The slice does not install it |
| Two loopback ports joined **both ways** pass a byte back and forth for ever: each port reads back what the hub writes to it and the link carries it across again | `loopback_transport.cpp`, `hub::route_data` | Engine behaviour, unchanged. The slice's tests join loopback ports one way |
| `08-engine-gaps.md` is out of date in places: peer-open detection has two rows (the second is right), and benchmark, fault injection, JSON positions, the diagnostic bundle and the event log are marked missing although they exist | that file | Not changed here; listed as a follow-up |

## 1. Goals and non-goals

Goals:

1. A port made through any client outlives that client, and every other client of the same user sees it, can use it
   and can watch its traffic. **[slice]** for loopback ports.
2. A Studio crash stops no port. **[paper]**; it follows from goal 1 once Studio is a client.
3. Studio and `vss_cli` share one set of ports. **[slice]** for `vss_cli agent`.
4. No screen and no string of the window changes. The model of `10-running-in-the-background.md` holds; only the
   process that runs the engine changes. **[paper]**

What stays true:

- **No network access.** The agent listens on a named pipe or a Unix domain socket and nowhere else. **[slice]**
- **No telemetry, no secrets in logs.** The agent logs who connected (a program name and a process number) and
  errors. It never logs the bytes of traffic. **[slice]**: the slice logs only its address and that it stopped.
- **One hub for each agent, one agent for each user.** **[slice]**
- **`vss_core` semantics are unchanged**, and so is the JSON topology schema. The agent calls `vss::studio` as the
  window does. **[slice]**
- **The strand model is unchanged.** Section 7.
- **No compatibility code.** A client and an agent of different versions do not talk (section 2).

Non-goals:

- A machine-wide service, or ports shared between users.
- Remote control of an agent from another computer.
- Seeing the ports of a program that keeps a private hub (section 6).
- Keeping driver ports across an agent crash (section 2 says why, and what would be needed).

## 2. Process model

### Who starts the agent, and when

Recommended, the same on every system: **the first client starts it on demand**, and the existing "Start when I sign
in" entry starts it at sign-in. **[paper]**

- A client that finds no agent starts the `vss_agent` that sits beside its own program file, detached, and retries
  the connection for a few seconds. Two clients that do this at once start two agents; the second finds the first
  (section 3) and exits with code 3, and both clients connect to the first. The exit code and the rule are
  **[slice]**; the starting is **[paper]**.
- The sign-in entries that exist today (`HKCU\...\Run` on Windows, `autostart/virtual-serial-studio.desktop` on
  Linux) change their command from `vss_gui --background` to `vss_agent`. The agent applies the running setup when
  it starts.
- An agent with no ports and no clients exits after a minute, so a person who opened Studio once does not keep a
  resident process. An agent with ports stays.

Rejected:

| Alternative | Why not |
|---|---|
| A Windows service | It runs in session 0 as another account. Driver leases and the helper's limits are per user SID, so the service would have to act for each user, and installing it needs an administrator. `vsscom_helper` already is the privileged piece and stays small |
| A systemd user unit as the only way on Linux | Not every session has a user manager (containers, WSL, some desktops). It is a good **addition**: `Restart=on-failure` restarts a crashed agent at once. PR 9 adds the unit file as an option |
| A launchd agent as the only way on macOS | A plist in `~/Library/LaunchAgents` works for a program that is not sandboxed, and `KeepAlive` restarts it. It needs the signing and notarising steps to cover a third program first. PR 9 |
| The agent inside `vss_gui` (a hidden Studio is the agent) | That is option A. A crash in Qt or in a screen still takes the ports |

### One agent for each user

- **Windows [slice]**: the first instance of the pipe is made with `FILE_FLAG_FIRST_PIPE_INSTANCE`, which fails when
  the name exists. The second agent then opens the pipe as a client: if the user's own process serves it, an agent is
  running (`already_running`, exit code 3); if anyone else does, the name was taken (`permission_denied`).
- **Linux and macOS [slice]**: an exclusive `flock` on `agent.lock` in the agent's private directory, held for the
  life of the process. The kernel drops it when the process ends, however it ends, so a socket file left by a crash
  is known to be stale and the next agent replaces it. The lock file itself stays on disk: a lock file
  cannot be removed without a race between two agents. The user's agent leaves one small file; each named instance
  of a test run leaves its own, in a directory the system clears at sign-out or restart.

### A client and an agent of different versions

No compatibility: `hello` carries one protocol number and the agent refuses any other with `version_mismatch`
**[slice]**. The window then says that another version of the agent is running and offers to restart it, which stops
the ports for a moment. **[paper]**: for that, `hello` takes a `replace` flag, and an agent that gets a `hello` with
another protocol number and `replace` saves the running setup and exits. `hello` and the error reply are therefore
the only messages whose shape may never change.

### Upgrade and uninstall **[paper]**

| Package | Upgrade | Uninstall |
|---|---|---|
| Windows (NSIS, per machine, elevated) | The program file of a running agent is locked, so the installer stops agents first: `vss_cli agent stop` for the installing user, then `taskkill` by image name for the agents of other signed-in users, whose pipes even an administrator cannot open. The ports stop; the running setup was saved and comes back at the next start. This is a new installer step, and `check-windows-installer.ps1` gains `vss_agent.exe` | The same stop, then the files. The `Run` value and `running-setup.json` are per user and are not removed today; that gap exists for `vss_gui` already |
| deb, rpm | The files are replaced under a running agent, which keeps running the old ones. The next client is newer, gets `version_mismatch` and offers the restart. No maintainer script is added | The files go; a running agent keeps going until it is stopped or the session ends. A `prerm` that stops agents would be the first maintainer script and is the owner's call |
| macOS (dmg) | As deb and rpm | As deb and rpm |

### Crashes

- **A client crashes.** Nothing happens to the ports. A request it had sent still completes; the answer is dropped.
  Its subscription is removed. **[slice]**
- **The agent crashes.** Every port goes. Pseudo terminals vanish with the process and their stable links dangle
  until the next start replaces them. **A Windows driver port is removed by the driver**, because the lease is a
  handle of the agent's process; a program that had the COM port open loses it. A client sees its connection close
  and reports it. **[slice]** for the closing and for a clean restart; the driver case follows from the driver code
  and was not run.
- **What comes back.** The agent takes over the running setup from the window: it writes `running-setup.json` a
  second after each change and applies it at start unless it says `stopped`. Driver ports come back with the COM
  numbers they asked for (the device node and its name survive, as `reopen` already relies on). **[paper]**
- **Who restarts it.** The next client that connects, or the sign-in entry. Where a supervisor is installed (the
  systemd unit, the launchd agent) it restarts at once. On Windows `RegisterApplicationRestart` asks Windows to
  restart a program that crashed after running for a minute; it costs a few lines and is recommended. **[paper]**

Can anything keep driver ports across an agent crash? Not without changing the driver or the helper, which this
design does not propose:

| Idea | What it needs | Verdict |
|---|---|---|
| Keep the agent small | Nothing. The agent links no Qt and runs no screen, so the code that can crash with the ports is the engine alone | This is the real gain over option A, and it is free |
| A second process holds duplicates of the lease handles | `DuplicateHandle` should keep the file object alive (inferred from how the framework raises cleanup; no test proves it). The restarted agent needs a way to take the leases back, and while no hub handle is open the driver loops the port back and clears its queues, so the program sees its own bytes | Needs a driver change to hold a port quiet instead of looping. Not proposed |
| `vsscom_helper` keeps the lease for a while after the client goes | A protocol change in the helper and the same driver change | Not proposed |

## 3. Transport and security

All of this section is **[slice]** unless marked.

| | Windows | Linux, macOS |
|---|---|---|
| Transport | Named pipe, byte mode, overlapped | Unix domain socket, stream |
| Address | `\\.\pipe\vss-agent-<user SID>` | `<directory>/agent.sock` |
| Directory | none | `$XDG_RUNTIME_DIR/virtual-serial-studio`, else `$TMPDIR/virtual-serial-studio-<uid>`, else `/tmp/virtual-serial-studio-<uid>` |
| Who may connect | The pipe's access list is `D:P(A;;GA;;;<user SID>)`: this user and nobody else, not administrators and not SYSTEM, with nothing inherited | The directory is made with mode 0700 and the socket with 0600. Before every use the directory must be a real directory (`lstat`, so not a link), owned by this user, with no group or other bits |
| Second check, by the agent | none needed: the access list is enforced by the kernel when the pipe is opened | The kernel is asked who is at the other end (`SO_PEERCRED`, `getpeereid`); a connection of any other user id is closed and never reaches the server |
| Check by the client | It asks which process serves the pipe (`GetNamedPipeServerProcessId`) and connects only when that process runs as the same user. It opens the pipe with `SECURITY_IDENTIFICATION`, so the server may learn who it is and may not act as it | The same directory check, then the same question to the kernel about the server |
| Other computers | `PIPE_REJECT_REMOTE_CLIENTS` | a socket file is local by nature |

**The name.** The address has no secret in it and can be guessed; it does not need one, because neither side trusts
the name. On Windows any program can try to make the pipe first; the agent then fails to start and says the name is
taken, and a client refuses a pipe that another user serves. On Linux and macOS the socket is never in a directory
others can write: `$XDG_RUNTIME_DIR` is the user's own, and the fallback under `/tmp` has the user id in its name
and is refused unless this user owns it and it is closed to others. Another user who makes that directory first
keeps the agent from starting there (it says `permission_denied`); they cannot read or answer for it.

**Instance names.** The only part of the address a caller chooses is an instance name: up to 32 letters, digits,
`_` and `-`, added as `-<name>`. The tests give each agent its own name so that a test never meets the user's agent.
It cannot point an address outside the user's own pipe names or directory.

**Another user on the same computer** is refused by the access list or the directory mode before a byte is read.
**A process in another session of the same user** (a second remote desktop session, a service run as the user) is
the same user and may connect: there is one agent for each user, not for each session, because driver ports are
machine-wide and the helper counts them by user. Restricting to the logon session is possible on Windows (the logon
SID in the access list) and is decision 9.

**Elevation.** A client that runs elevated connects to the user's ordinary agent and gets nothing more than any
other client: the agent does everything with its own rights. The other direction is the dangerous one. An agent that
runs elevated would do privileged work for any ordinary process of the user, so the agent must refuse to start
elevated (Windows) or as root (elsewhere) unless it is told to. **[paper]**: the slice does not check, because the
hosted Windows runners and the package containers run the tests elevated and as root.

Abuse cases:

| Case | What happens | |
|---|---|---|
| A process of another user connects | Refused by the system; on Linux and macOS also by the peer check | [slice]; refusal across two real users was not run (section 9) |
| A malicious process of the **same** user connects | It can do what the user can do through the agent, which is what the user could do by running `vss_cli`. The agent is not a boundary inside one user account, and holds no secret | by design |
| A program takes the pipe name before the agent | The agent does not start; a client refuses the pipe | [slice] for the agent side |
| A client sends bytes that are not a frame, or a frame over 1 MiB | The length is checked when its four bytes arrive, before anything is buffered. The client gets an error and is closed | [slice], and the decoder is fuzzed |
| A client connects and says nothing | Closed after 5 s without `hello` | [slice] |
| A client opens many connections | 16 at once; the next is told `busy` and closed | [slice] |
| A client floods requests | The agent reads no more from a client that has 32 requests waiting, and handles 64 frames of one client before the others get a turn. The flood slows its sender | [slice] |
| A client asks for events and never reads them | Section 4, back-pressure. The hub is never held up | [slice] |
| A client dies in the middle of a request | The request completes; the reply is dropped | [slice] |
| A socket file is left by a crash | Replaced by the next agent, which knows it is stale because it holds the lock | [slice] |
| A client asks for an enormous allocation inside a small frame | Every list and text has a limit, and the bytes of a batch are used in order and exactly once | [slice], fuzzed |

## 4. Protocol

### Framing **[slice]**

```
u32 little-endian   size of everything after this field   6 .. 1 048 576
u32 little-endian   size of the header                    2 .. size - 4
header              one JSON object, UTF-8
blob                the rest: bytes the header describes
```

### Serialisation, and why **[slice]**

The header is JSON, read with Boost.JSON, which the project already uses for topology files. Bytes of traffic are
never put into JSON: a write carries them as the blob, and a batch of events carries the bytes of all its events as
one blob that the events use in order.

| Alternative | Why not |
|---|---|
| A hand-written binary format for everything | About 55 operations and the port and link views would each need an encoder and a decoder written and fuzzed by hand. With JSON a new field is a line on each side, the messages can be read in a log, and the parser is already trusted with files people open. The cost of JSON is in the control path, which carries a few messages a second |
| JSON for everything, bytes as hex or base64 | Doubles the size of traffic and puts the hot path through the text parser |
| A new dependency (protobuf, flatbuffers, gRPC) | The project uses Boost and Qt only |
| Unix sockets on Windows too | Windows has them, but the pipe has the better-known security tools (an access list on the object, the server's process id, rejecting remote clients) |

The header is parsed with a depth limit of 8. Text is at most 1024 bytes and a list at most 4096 entries; the
encoder cuts longer text between characters and replaces bytes that are not UTF-8 (a path or a driver message is
not ours to trust), so that a strange name cannot cost a client its connection.

### Versioning **[slice]**

`protocol_version` is one number. Both sides must have the same one. Any change to a frame or a message changes it.

### Requests and replies **[slice]**

```
client:  {"id": 7, "op": "create_port", "name": "a", "kind": "loopback"}
agent:   {"id": 7, "reply": "port", "endpoint": 3}
agent:   {"id": 7, "reply": "error", "error": {"category": "vss", "value": 2, "message": "already exists"}}
```

The client chooses the id (never 0). The first request must be `hello`. Requests of one client run in the order they
were sent; requests of different clients run side by side. An error about the connection itself (a bad frame,
`busy`) has id 0 and the connection closes after it. A request for an operation the agent does not have gets an
error and the connection stays.

The slice has: `hello`, `list`, `create_port` (loopback), `remove_port`, `connect` (both ways or one way), `write`,
`subscribe`, `shutdown`.

### Errors **[slice]**

An error crosses as the name of its category, its value and the agent's message. On the client it becomes an error
code again: `vss` (the engine's `errc`), `vss.agent` (the protocol's own: `version_mismatch`, `not_running`,
`busy` and so on), `system` and `generic` keep category and value, because both ends run on one computer. So
`vss::result` is the same on both sides of the connection, and the window's recovery actions, which switch on those
codes (`driver_error.cpp`), keep working. A category the client does not know becomes `errc::io_error` with the
agent's message kept for display. **[paper]**: the categories of Boost.Asio and of the serial library are added to
that table by name before the window uses it.

### Events **[slice]**

`subscribe` starts the stream for a connection. The agent never sends an event by itself:

```
agent:  {"event": "batch", "revision": 12, "dropped": 0, "events": [ ... ]} + blob
```

- **Coalescing.** A sink on the hub's dispatcher strand copies each event into a queue for that client (2048 events
  and 512 KiB of bytes). A timer on the agent's client thread sends what the queue holds as one frame, every
  interval the client asked for (10 ms to 10 s, 250 ms when it does not say, as the window today). This is the
  window's `EventBridge` moved to the other side of the connection.
- **Back-pressure.** While a client has not read its last batch, no new one is sent. The queue goes on filling; when
  it is full the oldest event is dropped and counted, and the next batch says how many. A client that lets 4 MiB of
  replies and batches pile up is closed. The hub is never asked to wait, and what one slow client costs the agent is
  bounded.
- **The revision.** The agent counts changes to the set of ports and links. Every batch and every `list` carries the
  count, and a batch is sent when it changes even if there were no events. A client that sees it change asks for the
  list again. That is how the window of one client learns what another client did.

### Bulk traffic **[slice]** for the live tap

A client that subscribes with `data` gets the bytes of data events too: a **tap**. Without it the events say how many
bytes moved and carry none. A tap is bounded by the same queue and so can lose data under load, and says how much.
**[paper]**: a filter by port; `traffic(limit)` as pages of the hub's ring; and capture to a file running **inside**
the agent (`vss::capture` already exists), for a recording that must not lose bytes.

### Long operations, progress and cancel **[paper]**

The seven slow operations become requests whose reply comes late. While one runs the agent may send
`{"event": "progress", "id": <request id>, ...}`, and the client may send `{"op": "cancel", "target": <request id>}`.
A cancel takes effect at the next safe point: the benchmark checks its flag between chunks; a driver port that is
being made cannot be abandoned half-way, so it is finished and then removed. The reply says which happened. A long
request does not hold up the client's later quick requests, so it runs outside the client's order. The client
library has an asynchronous form for the window; the blocking form of the slice is for `vss_cli` and the tests.

### Capabilities **[slice]** in outline

The `hello` reply says what the agent can do; in the slice that is the list of port kinds (`loopback`).
**[paper]**: it carries the engine-side flags of `capabilities.hpp` by name (driver installed, peer-open, and so
on), computed in the agent. The remote backend merges them with the flags that are about the client's own process
(`firewall_rule`, `serial_group_step`, `com0com_pair_creation`). A flag the agent does not send is off, and the
window degrades as it does today ("Not available in this build").

## 5. `StudioBackend`, operation by operation

All of this section is **[paper]** except the rows marked. Kinds: **RR** request and reply; **late** a reply that can
take seconds, with progress and cancel; **bulk** bytes in the blob; **view** part of the snapshot the client caches;
**local** stays in the client.

| Operation | Kind | Notes |
|---|---|---|
| `caps` | view | From `hello`, merged with local flags |
| `create_virtual_port`, `create_auto_port` | late | Late because the kind may be a driver port. **Hard:** the search for a free COM number moves into the agent, where the device list and the retries are in one place. [slice] for loopback |
| `create_mock_port` | RR | |
| `attach_serial` | late | A device or a network connection opens |
| `remove_port` | RR | [slice] |
| `create_driver_port` | late | Up to 30 s for the port to appear |
| `set_com_number` | late | The port restarts |
| `port_holder` | RR | The agent asks the driver and describes the process; it runs as the same user, so "own user" means the same |
| `release_port` | RR | "End program" ends the process from the agent, with the agent's rights |
| `rename_port`, `inject_fault`, `set_echo`, `set_line`, `refresh_lines`, `set_link_wiring` | RR | |
| `set_port_settings` | late | A device may reopen |
| `ports`, `links`, `port`, `link` | view | One `snapshot` request returns the full views; the agent computes what the adapter computes today (peer state, driver flags, format conflicts, echo, wiring, lost ports). The client keeps the cache and the comparison that decides which signal to emit. [slice] as `list`, with fewer fields |
| `devices`, `facts`, `check_driver`, `probe_cache_age` | local | Probes of the computer, not of the studio |
| `other_program_ports` | local | Still the driver ports nobody in the agent holds (section 6) |
| `traffic` | bulk | Pages of the hub's ring |
| `connect_ports`, `split`, `join`, `redirect`, `monitor`, `bus`, `disconnect`, `set_owner`, `release_owner` | RR | [slice] for `connect_ports` and `redirect` |
| `send` | RR, bulk | [slice] as `write`. The outcome arrives as events, as today |
| `link_wiring`, `echo`, `lost_ports`, `hub_running` | view | |
| `run_benchmark` | late | **Hard:** takes a progress callback and a cancel flag. The run happens in the agent, where the ports are; progress and cancel are the events and the request of section 4 |
| `recreate_lost_ports` | late | |
| `snapshot` | RR, bulk | The topology JSON as the blob |
| `apply` | late, bulk | Already atomic in the studio, with rollback |
| `share_serial`, `bridge` | late | Already one call in the studio |
| `create_com0com_pair` | local | It shows an elevation prompt, which only a program with a window may do |
| `refresh` | local | Asks for the snapshot again |
| `set_refresh_interval` | RR | The interval of `subscribe`. [slice] |
| `ports_changed`, `links_changed`, `stats_changed` | view | From comparing snapshots, driven by the revision and by batches |
| `lines_changed`, `batch_ready` | event | From the batches. [slice] for the batches |

**Nothing returns a live object.** The interface already deals in ids and views; `std::shared_ptr<endpoint>` stops at
the adapter. The hard ones are the two that take callbacks (benchmark) and the ones composed in the GUI.

**Compositions.** Today `add_program_port`, `build_bundle`, `duplicate_ports` and the undo steps are sequences of
backend calls with a rollback written in the GUI. Remote, each call is a round trip, another client's change can
land in the middle, and a client that dies leaves the sequence half done. They move into the agent as **one
mechanism, not one operation each**: a `batch` request holds a list of primitive operations; the agent runs them in
order with no other client's change between them, and undoes the ones already done when one fails. The GUI code
that builds these sequences stays where it is and produces a list instead of making calls. The undo history stays
in the window: it is per window, and each step is a batch. `apply`, `share_serial` and `bridge` are already single
calls. **[paper]**

**One writer.** For a batch to be atomic, every change to the topology runs on one strand in the agent; reads,
writes of bytes and taps do not. A long change (a driver port) then holds up other changes for its seconds, which is
what a person sees today in one window. The slice does not have this: each client's requests are in order, and
clients are not ordered against each other. **[paper]**

## 6. Registry and external ports

- **Ports the agent owns.** Every client gets the same list, with state, counters and paths, and may tap the
  traffic. The agent records which client made each port (program name and process number), for display.
  **[slice]** except the record of who made it.
- **Ports the agent does not own.** A driver port made by a program with its own hub (a `vss_cli run` kept private
  on purpose, another user's Studio, any other tool) is as invisible to the agent as it is to Studio today: the
  enumeration gives its COM name and nothing else, and opening it would take it from its program.
  `other_program_ports()` keeps saying so, and `external_ports` stays off.
- **So the "ports another program made" row is closed for programs that are clients of the agent, and not for
  programs that are not.** After `vss_cli` uses the agent by choice (section 8) the first group is what a person
  meets. A registry for the whole computer needs a second observer on the hub side of the driver, as the row says,
  and is not proposed.
- **A read-only tap** is `subscribe` with `data`: copies of the bytes the hub publishes for each port, in both
  directions, at the cost described in section 4. It cannot disturb the port. "Read-only" is what the client does,
  not something the agent enforces between programs of one user; a `read_only` flag in `hello`, after which the
  agent refuses that connection's changes, is cheap and is decision 10. **[paper]**

## 7. Threading

**[slice]**, and the TSan leg runs it.

| Thread | What runs on it |
|---|---|
| The hub's threads | The engine, as today: each endpoint on its strand, events on the dispatcher strand. The only agent code they run is the sink that copies one event into a client's queue under a short lock. It never does I/O and never waits for a client |
| The agent's client thread | Every read from and write to a client, the timers, and all the state of a connection |
| The request pool (2 threads) | The requests that call the studio. One client's requests run in order on a strand; different clients run side by side. A request that takes seconds holds up neither the client thread nor the hub |

The sink captures only a `shared_ptr` to its queue, which is the rule for sinks: `unsubscribe` cannot recall a
delivery that is running, and that delivery then writes into a queue nobody reads.

In the window nothing changes. The remote backend owns a thread for its connection; replies and batches are handed
to the GUI thread with queued connections, at most one batch for each interval, and the GUI thread never waits for
the agent. **[paper]**

## 8. `vss_cli`

| Command | With the agent |
|---|---|
| `agent status`, `ports`, `create-port`, `remove-port`, `connect`, `write`, `watch`, `stop` | Clients. **[slice]** |
| `devices`, `permissions`, `selftest` | No hub involved, or a private one for a moment. Unchanged |
| `bench` | Private hub on purpose: it measures a path it builds itself and should not disturb, or be disturbed by, the user's ports |
| `loopback` | A demonstration; private |
| `virtual`, `vport`, `share`, `join`, `bus`, `bridge`, `run` | They build something and keep the process alive until Ctrl-C. **They keep a private hub by default**: scripts and CI rely on "the ports go when the command does", and the system tests check it. Each gains `--agent`, which builds the same thing in the agent and returns at once **[paper]** |
| `capture`, `play`, `respond` | With a topology file, private as today. With `--agent` and a port name, `capture` is a tap and `play` writes; `respond` runs inside the agent, because a reply timed in milliseconds should not cross a connection twice **[paper]** |

**`vss_cli run`.** `run file.json` stays the private, CI form, with `--seconds`. `vss_cli agent apply file.json`
replaces the running setup in the agent and returns; `vss_cli agent stop` ends it. Whether `run` should one day use
the agent by default is decision 6.

## 9. Testing

| Layer | How, without a desktop session | |
|---|---|---|
| Frames and messages | Unit tests: every message there and back, every limit, each kind of bad header (`tests/unit/test_agent_proto.cpp`) | [slice] |
| The decoder | A fuzz harness, `fuzz_agent_frame`: bytes arrive in pieces, every frame is decoded as a request and as a reply, and what decodes must encode and decode to the same value. Its corpus is replayed in every build | [slice] |
| Transport | The server's tests run over the real pipe or socket. Platform tests read back the pipe's access list, and the modes of the directory and the socket | [slice] |
| Server | The server and clients in one process (`tests/unit/test_agent_server.cpp`): handshake and its failures, `busy`, a silent client, a flood, a client that never reads, a client that goes away, events in batches, the revision. A stress case runs several clients against traffic and stops the server under them; the TSan leg runs it at -O1 | [slice] |
| Two processes | `tests/system/test_system_agent.cpp` starts `vss_agent` and `vss_cli` as real processes: client A makes two ports and a link and exits; client B lists them, writes, and reads the bytes on the far port. Then: kill a client; kill the agent; a second agent; a clean stop | [slice] |
| Waiting | Tests wait for a condition with a time limit, never for a fixed time | [slice] |
| The window on the agent | `FakeStudioBackend` stays the double for screens. The remote backend is tested against a real agent in one process, offscreen | [paper] |

What these cannot cover:

- **Two real users.** Refusing another user needs a second account. The tests check the mechanism (the access list,
  the modes, the peer question); the refusal itself is for a VM guest (`docs/vm-testing.md`) with two accounts.
- **Windows driver ports in the agent**: a port outliving Studio, what a program sees when the agent is killed, COM
  numbers after a restart. VM guest with the driver installed.
- **Sign-in start, sign-out, the installers** stopping an agent. VM guest.
- **Real serial hardware** through the agent. A hardware check, as for the engine.

## 10. The pull requests

Each is reviewable alone and green alone. Sizes are lines of code and tests together, to within a factor of two.
The slice came out at a little more than one line of test for each line of code, and the estimates assume the same.

| PR | What | Size |
|---|---|---|
| 1 (this slice) | This page; `libs/vss_agent_proto`; `libs/vss_agent` (transport, server, client); `vss_agent`; `vss_cli agent`; the tests of section 9. Loopback ports only, not installed | 10 700 measured: 4 800 of code, 5 400 of tests and the fuzz harness, 500 of documents |
| 2 | The whole snapshot (full port and link views, capabilities by name), the rest of the quick operations, the error table for Asio and serial categories | 4 000 |
| 3 | Late replies: progress, cancel, the asynchronous client; driver ports, COM numbers, `attach_serial`, `share`, `bridge`, `apply`, the benchmark | 5 000 |
| 4 | `batch` with rollback and the single writer; the free-COM-number search in the agent | 2 500 |
| 5 | Traffic: the filtered tap, pages of history, capture in the agent | 2 500 |
| 6 | The running setup and its restore move into the agent; start on demand; exit when idle; refuse to run elevated; `RegisterApplicationRestart` | 2 500 |
| 7 | `RemoteStudioBackend`, behind a switch, with the window's tests run against it | 6 000 |
| 8 | The window uses the agent by default: what the tray, the single-instance lock and the sign-in entry become (decision 4); the restart-the-agent prompt | 2 500 |
| 9 | Installers: `vss_agent` in all four, stopping agents on Windows, the signing lists, the optional systemd unit and launchd agent | 1 000 |
| 10 | `--agent` for the `vss_cli` commands of section 8, `agent apply`, `respond` in the agent | 2 500 |
| 11 | VM guest runs: two users, driver ports, sign-in, upgrade | scripts |

PR 7 is the large one and cannot be cut smaller without a backend that does half of the interface. PRs 2 to 6 can
land in any order after 1.

## 11. Decisions for the owner

| # | Question | Options | Recommended | Cost of being wrong |
|---|---|---|---|---|
| 1 | Process model on each system | (a) started on demand by the first client, plus the existing sign-in entry; (b) a supervisor only (systemd user unit, launchd agent, a scheduled task); (c) a Windows service | (a) everywhere, with (b) as an option on Linux and macOS in PR 9 | Low. The start path is a few hundred lines and can change without touching the protocol |
| 2 | Protocol format | (a) JSON header and a blob for bytes; (b) hand-written binary; (c) a new dependency | (a) | Medium, and falling: the format can be replaced at any time because there is no compatibility, but the cost of doing so grows with each PR that adds messages |
| 3 | Does the window keep an in-process mode | (a) both backends stay, chosen at start; (b) the agent only | (a) until PR 8 has been in use for a release, then decide again. `EngineStudioBackend` is also what the agent's own logic is tested against | Low for (a): two backends to keep working. High for (b) too early: every agent bug is a window bug with no way round |
| 4 | The tray and "run in the background" | (a) keep as it is, with the agent behind it; (b) reduce: the tray icon stays as the place to see and stop the ports, closing the window closes Studio, and the agent keeps the ports; (c) remove the tray | (b). The promise of `10-running-in-the-background.md` is unchanged and the resident process becomes the small one | Medium: it is what people see. The strings of that page were written to hold in both models, so (a) to (b) changes behaviour, not words |
| 5 | Does a mismatched version need the exact program version too | (a) the protocol number only; (b) the program version as well | (a). (b) refuses a rebuilt client during development for no gain | Low |
| 6 | `vss_cli` commands that keep ports: private hub or agent by default | (a) private by default, `--agent` to choose; (b) the agent by default | (a). Scripts and CI rely on ports going with the command | Medium for (b): it changes what existing scripts do |
| 7 | Stable links for pseudo terminals in the agent | (a) always on, named after the port; (b) as today, off | (a): a path that survives a restart is the point of the agent | Low |
| 8 | An agent with no ports and no clients | (a) exits after a minute; (b) stays | (a) | Low |
| 9 | Another session of the same user | (a) may connect: one agent for each user; (b) one agent for each logon session | (a). Driver ports are machine-wide and counted by user | Low on a personal computer; on a shared server with one account, (a) means its sessions share ports, which is also what they share in the driver |
| 10 | A `read_only` flag for a connection | (a) add it; (b) leave "read-only" to the client | (a), in PR 5 | Low |
| 11 | Windows installer stops other users' agents by ending them | (a) yes, the running setup brings the ports back; (b) refuse to upgrade while any agent runs | (a) | Medium: (a) interrupts another signed-in user's ports during an upgrade; (b) can block an upgrade on a shared computer |
| 12 | The library layout of the slice | (a) `vss_agent_proto` (wire format, no I/O) and `vss_agent` (transport, server and client together); (b) a separate client library that does not link the studio | (a) now; split when a client appears that should not link the studio | Low |

## What the slice proves

| Claim | Proven by | Still paper |
|---|---|---|
| A port made through a client outlives that client, and a second client sees and uses it | The two-process test: real `vss_agent`, real `vss_cli` processes, bytes read on the far port by a third client | For loopback ports. Pseudo terminals and driver ports are PR 3 and the VM runs |
| Only this user can connect | The access list and the modes are read back; the peer question is asked on every connection | Refusal of a second real user was not run |
| One agent for each user | A second agent exits with code 3; a stale socket is replaced | |
| The protocol refuses what it should | Unit tests and the fuzz harness | Only the operations of the slice exist |
| A client cannot hold up the hub or the other clients | The tests of a flood, a silent client and a client that never reads, and the stress case under TSan | Long operations and the single writer do not exist yet |
| A killed client changes nothing; a killed agent is noticed, and a new one starts clean | The two-process tests | What a program holding a driver port sees |
| The window can use the agent unchanged | | All of it: section 5 is a mapping, not code |
| Ports come back after a crash or a restart | | All of it: PR 6 |
