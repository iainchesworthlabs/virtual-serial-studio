# The plugin framework (design)

Status: **reviewed**. The owner went through this design on 13 November 2026. Section 17 records each decision
and lists what is still open. The manifest reader is the first part built. It builds on the decisions in
[Editions, licensing and plugins](editions.md) and on the protocol of [the per-user agent](agent.md), and it does not
repeat them.

## 1. Scope and what is already decided

Decided in `editions.md`, and taken as given here:

- Plugins run **out of process** and talk to Studio over the agent's framing. A plugin author chooses their own
  licence, a crash cannot take Studio down, and permissions can be limited.
- Four **extension points**: generators, fixture types, behaviour modifiers and automation adapters. Behaviour
  modifiers are in the first version.
- A plugin declares extension points, parameters and permissions in a **manifest**. Studio draws the settings from the
  declared parameters, so no plugin code runs in the GUI process.
- Three **trust levels**: official (signed by the owner), third-party signed, and unsigned with a warning. An
  administrator can allow only official plugins. Trust decisions are tied to the file's hash. Each release carries a
  revocation list and plugins carry an expiry date.
- **No network from the GUI, no telemetry.** Timing that must be exact stays in the engine.
- The signing key and the signing job are the owner's. This page designs the verifier and the file formats. It does
  not create, hold or name any key.

Not in this page, and referenced only:

| Area | Where it is decided | What this page assumes |
|---|---|---|
| Licence files, seats, which edition may load which plugin | `docs/licensing.md` (the licensing work, under review on its own branch) | `licence_service::check(feature)` answers "may this plugin run now" (section 8.7). This page does not look inside a licence |
| The signed-file envelope, the Ed25519 code and the trust list | `docs/licensing.md`, `vss::signed_file` | The plugin signature is one more purpose of that envelope, with its own key (section 8.2). There is one verifier, not two |
| The plain support bundle (version, driver state, setup, log, serial ports; never traffic) | `docs/licensing.md` and `screens/09-licence-and-support.md` | A plugin can add a section to it (section 19). Studio owns the bundle, its dialog and its preview |
| The rule model for response timing: matches, actions, clocks, determinism, replaying a recording | `docs/response-timing.md` (the response-timing work, under review on its own branch) | Section 6 is the plugin side only: which tier a plugin declares and what the host holds it to. Where the two pages differ, that page is right |
| The Plugins screens and their strings | `docs/gui-design/docs/screens/10-plugins.md` and `11-decoding-and-triggers.md`, in this pull request | Section 10 names the failures; the screen spec has the words |
| Security of the remote control endpoint (tokens, OAuth, bindings to other computers) | The remote-control work | An automation adapter gets the engine's agent operations through the proxy of section 7.7. How an adapter is exposed to the network is not designed here |
| Website and downloads | The website work | none |
| `vss_core` behaviour, the JSON topology schema | Unchanged without a request | Section 6 shows how the first version avoids both. Where it cannot, it says so |

## 2. Goals and non-goals

Goals, in order of priority:

1. **A bad plugin cannot hurt anything but itself.** Not Studio, not the engine, not the other plugins, not the user's
   files beyond what it was given. Where the system cannot enforce that, it says so on the screen.
2. **A plugin never makes Studio slower or less reliable.** The GUI thread never waits for a plugin, the hub never
   waits for a plugin, and what one plugin costs in memory, queue space and CPU is bounded.
3. **What the user is told is true.** A badge, a signer, a sandbox level, a status: each says what was checked and
   never more.
4. **Failures are visible and recoverable.** Every failure has a name, a message and an action.
5. **Test labs can rely on it.** Behaviour is deterministic where the user asks for it, and a fault is repeatable.

Non-goals for the first version:

- In-process plugins with a C ABI or WASM. Not in the first version (decision 12). They stay possible later, and
  need a licence position from the owner first.
- A plugin store, an update client or any network access. A plugin is installed by putting a directory in place.
- Plugins that draw their own windows. A plugin has parameters and a status line, nothing else, in Studio.
- Hot code loading. A plugin changes by stopping and starting.

## 3. Vocabulary

| Term | Meaning |
|---|---|
| Plugin | A directory with a manifest, a signature file (optional) and the files the manifest lists, at least one of them an executable |
| Plugin host | The `vss_agent` supervisor code that finds, checks, starts and watches plugins. The plugins are its child processes |
| Launcher | A very small program, `vss_plugin_launcher`, that the agent starts first. It applies the sandbox and then runs the plugin (section 9) |
| Instance | One use of an extension point: a generator on port 3, a modifier on link 5. A plugin has any number of instances up to its declared limit |
| Engine rule | A declarative piece of behaviour that the engine carries out without asking the plugin (section 6) |
| Plugin API | The messages between the host and a plugin (section 7). Its version is not the agent protocol's version |

## 4. Where the pieces live

```
  Studio (vss_gui)         vss_cli                      Studio never talks to a plugin process.
        \                   /                            It sees plugins through the agent.
         \  agent protocol  /
          v                v
        +------------------------------- vss_agent (one for each user) ------------------+
        |  hub + studio  <--- engine rules                                                  |
        |       ^                                                                           |
        |  plugin host: discover, verify, trust, supervise, route requests, enforce budgets |
        +-----|------------------|------------------|--------------------------------------+
              | plugin API       | plugin API       | plugin API       (one private channel for each plugin)
        +-----v----+       +-----v----+       +-----v----+
        | plugin A |       | plugin B |       | plugin C |   sandboxed child processes
        +----------+       +----------+       +----------+
```

**The host lives in the agent, not in Studio.** The reasons: plugins act on ports, and ports outlive the window
(`agent.md`, section 1); a test rig runs with no window at all; and the agent has no Qt and no GUI thread to protect.
Until `RemoteStudioBackend` exists, the same code runs inside `vss_gui` or `vss_cli` through a `plugin_host` library
that takes a `studio` and an executor. The agent is its first user, not its only one. The library is `libs/vss_plugin`
(section 14), with no Qt in it.

Studio's part is small: a **Plugins page** (list, status, trust, settings) built from views the agent sends, and the
trust prompts. It does not parse a manifest, verify a signature or read a plugin's files.

## 5. Plugin lifecycle

### 5.1 States

```
discovered -> verified -> trusted -> enabled -> starting -> ready -> stopping -> stopped
                |            |          |           |          |
                v            v          v           v          v
            rejected     untrusted   disabled    backing-off  degraded
                                                      |
                                                      v
                                                 quarantined
```

| State | Meaning | Leaves by |
|---|---|---|
| `discovered` | A directory with a manifest was found | Parse and verify |
| `rejected` | Manifest invalid, signature bad, hash mismatch, revoked, expired, or a policy forbids it. A reason code is kept | A change of files, a new revocation list or a new policy, which re-run the checks |
| `verified` | Structure and signature check out; the trust level is known | A trust decision |
| `untrusted` | Verified, and no stored decision for this hash | The user decides (section 8.4) |
| `trusted` | A decision exists for this exact hash | The user enables it |
| `disabled` | Trusted and switched off by the user, by policy, by the licence or by a quarantine | The user enables it |
| `enabled` | Should be running | The supervisor starts it |
| `starting` | Process started, no valid `hello` yet (limit: 5 s) | `ready` or a failure |
| `ready` | Handshake done, permissions granted | Normal run |
| `degraded` | Running, but a budget was exceeded, the sandbox is weaker than asked, or it reports a problem. Its instances may be in fail-open or fail-closed mode | Back to `ready` when the cause stops |
| `backing-off` | Exited or was killed; waiting before the next start | The timer, or the user pressing Start |
| `quarantined` | Too many failures; the supervisor stopped trying | The user, by Start. The count resets |
| `stopping`, `stopped` | An orderly stop (section 5.5) | |

State is kept per plugin id **and** hash. A new hash is a new plugin as far as trust goes, and an old instance does not
carry over (section 8.4).

### 5.2 Discover

Search roots, in this order, the first to hold an id wins and a second is reported as `shadowed`:

| Root | Holds | Writable by |
|---|---|---|
| `<install>/studio-plugins/` | Official plugins that come with Studio or with a Pro install. It is not `<install>/plugins/`: the Linux package keeps Qt's own plug-ins there | Administrator |
| Machine plugin directory (`%ProgramData%\VirtualSerialStudio\plugins`, `/usr/local/share/virtual-serial-studio/plugins`, `/Library/Application Support/VirtualSerialStudio/plugins`) | Plugins an administrator installs for everyone | Administrator |
| Per-user plugin directory (`%LOCALAPPDATA%\VirtualSerialStudio\plugins`, `$XDG_DATA_HOME/virtual-serial-studio/plugins`, `~/Library/Application Support/VirtualSerialStudio/plugins`) | Plugins the user installs | The user |

Rules:

- Discovery runs at agent start, when the user presses Rescan, and when a directory changes (watched at most once a
  second per root). It runs on a worker, never on the GUI thread.
- A symbolic link or junction in a plugin directory is not followed. A plugin directory must be a real directory
  owned by the user or by an administrator, with no write access for others (Windows: the ACL is read, and an entry
  that grants write to `Everyone` or `Users` on a machine or install root rejects the plugin).
- At most 64 plugins, and a manifest of at most 256 KiB. More is `rejected: too_many` or `manifest_too_large`.

**Scope.** A plugin has a scope, from the root it was found in. The host's view of a plugin carries it and the
Plugins page shows it (`screens/10-plugins.md`).

| | Came with Studio | For everyone on this computer | For this user |
|---|---|---|---|
| Root | `<install>/studio-plugins/` | The machine plugin directory | The per-user plugin directory |
| Put there by | The installer | An administrator | The user |
| Removed or replaced by | The installer | An administrator | The user |
| Listed for | Every user | Every user | That user |
| Switched on and off, and decided about | By each user, for themselves, inside the policy | The same | The same |

- **Studio never raises its own rights.** From the window a user adds a plugin for themselves: Studio copies the
  folder into the per-user root. A plugin for everyone is put in the machine root by an administrator, by hand,
  with an installer or with `vss_cli plugin install --machine` from an administrator's prompt. The window shows
  the path to use.
- **An administrator can turn the per-user root off** (`user_plugins: false` in the policy, section 8.6). It is
  then not searched, and the page says so.
- **The same id in two scopes.** The wider scope wins, in the order of the table, and the other copy is
  `shadowed`: listed under the winner with its version and scope, and never run. When the shadowed copy is the
  newer one, the page says that too, so that a user is not left wondering why their update did nothing. There is
  no way for a per-user copy to replace a copy for everyone in the first version.
- **The same id from a different signer is a different plugin.** A decision is stored for an id, a hash and,
  for a signed plugin, the key. It never carries over to a plugin with the same id that is signed by another
  key or is not signed. The page shows the two as a conflict and names both signers. Nothing is swapped in
  silently.
- **Reserved ids.** An id that starts with `com.virtualserialstudio.` is accepted only with an official
  signature. Anything else that uses it is `rejected: reserved_id`.

**Studio installed for one user.** Studio itself may be installed for everyone or for one user, depending on the
rights a person has on the computer. This was investigated on 13 November 2026 by reading the packaging in
`cmake/packaging/`, `docs/releasing.md` and `docs/agent.md`. Nothing was installed on a computer to try it; the
tests of section 16 do that when the host exists.

What the packages do today:

| | For everyone | For one user |
|---|---|---|
| Windows | The NSIS installer. It asks for an administrator once and installs into `%ProgramFiles%`. It has no mode for one user, on purpose (`VssPackage-windows.cmake` says why) | The zip, unpacked anywhere the user can write. Nothing is registered |
| Linux | The deb or rpm, into `/opt/virtual-serial-studio`, owned by root | The release archive, unpacked anywhere the user can write |
| macOS | The app in `/Applications`, when root owns it (a management tool put it there) | The app dragged from the disk image, into `~/Applications` or into `/Applications`. A person who drags it owns it and can change it, so it counts as for one user wherever it lies |

So an install for one user already exists on every platform, as an archive. **No installer gets a second mode.**
What changes is that Studio works out which kind it is and behaves accordingly.

- **How Studio tells.** By the folder, not by a registry value or a file that says so. The install is *for
  everyone* when nobody but an administrator can write to the folder that holds the programs or to any folder
  above it (Windows: only Administrators, SYSTEM and TrustedInstaller have write access; Linux and macOS: root
  owns them and no group or other has write access). Anything else is *for one user*. The check is the one
  already written for a plugin directory, run once when the agent starts. A build tree is for one user.
- **The install root** of an install for one user is checked as the per-user root is: owned by the user, not
  writable by others. Its plugins still show as "Came with Studio".
- **An official plugin loses nothing**, because its trust comes from its signature and not from its folder.
- **The machine root and the policy file** are read by both kinds when they exist, and are an administrator's.
- **A policy binds Studio, not the person.** A user who may run their own programs can run a copy of Studio that
  ignores the policy, whatever this design does. A policy holds where the administrator also controls which
  programs run (AppLocker or App Control on Windows, and the like elsewhere). The page says which kind of
  install is running, and a diagnostic bundle records it.
- **Running without isolation** (section 9.2) follows the kind: an install for one user offers the override
  unless a policy file is there and forbids it; an install for everyone offers it only where the policy allows.
- **The sandbox needs no administrator** on any of the three systems (the mechanisms of section 9.2 are all
  open to an ordinary user), so the launcher works the same in both kinds and is never installed with special
  rights.

**Both kinds on one computer.** This is possible and will happen: the installer for everyone, and a zip of a
newer version in a user's folder. There is one agent for each user (`docs/agent.md`, section 2), and a client
starts the agent that lies beside it only when none is running. So today the agent that runs is the one whose
window or command was started first, and a window from the other copy would talk to it. For plugins that is not
good enough, because the agent decides the install root, the launcher and the kind of install. Needed, as an
addition to the agent's `hello` when the host is built:

- The agent's reply to `hello` carries the folder it runs from and the kind of install.
- A client that finds an agent from another folder says so ("Another copy of Studio is running the ports:
  `<folder>`") and offers the restart that `docs/agent.md` already plans for a version mismatch. It does not
  carry on silently. Where the protocol numbers differ, the existing `version_mismatch` path already does this.
- The Plugins page shows the folder and the kind of the agent it is connected to, since that is the copy whose
  plugins run.

**The driver** is only on Windows, is per machine, and needs an administrator to set up, from the installer or
from "Set up Virtual Serial Studio". A Studio from the zip uses the driver and the helper service when they are
there and works without driver ports when they are not, as it does today. No plugin and no part of the plugin
host talks to the driver or to the helper service, and neither is started by the launcher. "Studio never raises
its own rights" above is about plugins; the driver set-up keeps the prompt it has.

**Left for the installers' own work, not this design:** adding `vss_agent` and `vss_plugin_launcher` to the
four packages and their signing lists (`docs/agent.md`, step 9), and creating the machine plugin directory with
the right owner in the installer for everyone, so that it never has to be made by a program a user runs.

### 5.3 Verify

In this order, stopping at the first failure. Each step has a reason code (section 12).

1. **Read the manifest bytes once**, into memory. Everything after works on these bytes. The file is never read again
   for the decision.
2. **Parse** with a depth limit, size limits and unknown-key rejection (section 13).
3. **Check the layout**: every listed file exists, is a regular file, has a normalised relative path with no `..`, no
   absolute path, no drive letter, no alternate data stream, no reserved device name, and no case-insensitive
   duplicate. No file exists that the manifest does not list (a signed plugin only).
4. **Check the signature** if there is one: a signed envelope that names the manifest by its hash (section 8.2). The signing key
   must be known: an official key built into Studio, or a key the user has trusted.
5. **Check every listed file's hash** against the manifest (signed plugins). An unsigned plugin has no hashes in its
   manifest, so the verifier computes a tree hash itself (section 8.3).
6. **Check revocation and expiry** (section 8.5): the key, the plugin hash and the plugin id against the list, and
   `not_after` against the clock high-water mark.
7. **Check the policy** (section 8.6) and the **entitlement** (section 8.7).
8. **Check compatibility**: the plugin API range the manifest asks for against the host's, and the engine
   capabilities it requires (`requires.capabilities`) against what the agent has. A missing capability is not
   `rejected`; it is `disabled: capability_missing`, shown as "Not available in this build", as `08-engine-gaps.md`
   requires.

Verification hashes files, so it runs on a worker and takes as long as the files are large. The result is cached by
(path, size, modification time, hash of the manifest) for display only. **A start never trusts the cache**: the check
in section 5.4 repeats.

### 5.4 Start

The point of the start sequence is that **what is checked is what runs** (no gap between check and use).

1. Open every file of the plugin for reading with write and delete denied (Windows: share mode read only; elsewhere
   the files are opened and kept, and the verifier checks the descriptor's device, inode and size again after hashing).
   The handles are kept until the process is gone.
2. Hash the files **through those handles** and compare with the verified hash. A mismatch is `rejected: changed`.
3. Create the plugin's channel (section 7.1) and its data directory (section 9.4).
4. Ask the launcher to start the plugin: executable path, arguments from the manifest (fixed strings, no shell), the
   sandbox profile derived from the **granted** permissions, resource limits, and the channel's inherited handle.
   The launcher runs the executable from the open handle where the system allows it (Linux `execveat` with
   `AT_EMPTY_PATH`, macOS and Windows by path with the directory held closed to writers).
5. Wait up to 5 s for `hello`. No `hello`, or one that is wrong, kills the process: `start_failed: no_hello`.
6. Reply with `hello_ack` (granted permissions, parameter values, the engine's capabilities, limits). The plugin is
   `ready`.

### 5.5 Supervise

| Mechanism | Rule |
|---|---|
| Heartbeat | The host sends `ping` every 5 s when the channel has been quiet. No `pong` within 5 s counts as a miss; three misses in a row kill the plugin: `unresponsive`. Any frame resets the count |
| Exit | An exit with any code, a signal or a closed channel is a failure unless the host asked for the stop. Exit code 0 on its own, with the host not asking, still counts as an exit and is restarted, since a plugin is meant to stay |
| Budgets | Memory, CPU time, open files and child processes by the sandbox (section 9.3). Frames in and out, bytes in and out and log lines per second by the host (section 7.9). Breaking one is a failure of that budget, with its name |
| Status | The plugin may send `status` (`ok`, `warning`, `error` and a short text). It is shown, and it never changes the lifecycle on its own |

**Restart with back-off.** After a failure the state is `backing-off`:

| Failure number in the window | Wait |
|---|---|
| 1st | 1 s |
| 2nd | 2 s |
| 3rd | 4 s |
| 4th | 8 s |
| 5th | `quarantined` |

A random addition of up to 20 percent is made to each wait so that several plugins that failed together do not start
together. The count is **failures in the last 10 minutes** (a window), and it resets when a run lasts 5 minutes after
being `ready`. A plugin that starts, becomes `ready` and crashes within seconds counts the same as one that never
starts. A `quarantined` plugin is not started again until the user presses Start, and then it gets a fresh count. A
plugin that fails five times, is started by the user and fails again, is `quarantined` again after five more.

The clock for waits is the monotonic clock. The host takes a clock object in its constructor so that tests step it.

**What happens to instances while a plugin is down** is set by each extension point (sections 7.4 to 7.7): a modifier
follows its fail-open or fail-closed rule, a generator stops, a fixture goes silent, an adapter's callers get
`plugin_unavailable`. Engine rules (section 6) **keep running**, because they live in the engine.

### 5.6 Stop

An orderly stop (user disables, Studio quits, the agent shuts down, policy changes):

1. Send `shutdown` with a reason. Instances are told first (`instance.stop`), and ports the plugin made through the
   proxy are not removed (they belong to the topology, not to the plugin; section 7.7).
2. Wait up to 3 s for the plugin to answer `bye` and exit.
3. Terminate: `SIGTERM` to the process group, 1 s, then `SIGKILL`; on Windows closing the job object kills the whole
   process tree.
4. Release the file handles and the channel, remove the data directory's lock.

On agent exit the host stops every plugin in parallel and waits for the slowest, at most 5 s in all.

A plugin that ignores a stop is killed, never waited for. No stop blocks any thread that serves a port.

## 6. Response timing: declared to the engine

This is open decision 1 of `editions.md`. Since this page was first written the decision has its own page,
`docs/response-timing.md`, which designs the rule model in full: what a rule matches and does, the three places a
rule set sits (`peer`, `station`, `inline`), the one clock, determinism, limits, and what happens when the plugin
dies. **That page owns the rule language.** The two agree on the answer: a plugin hands the engine a bounded,
declarative rule set and is not in the path of the bytes. This section keeps what the plugin framework has to add:
the tier a plugin declares in its manifest, what the host holds it to, and the capability flags. Sections 6.3 and
6.6 are a summary to be replaced by a reference when that page is agreed.

Two points where the pages meet:

- Its `ask` action (a rule waits for a plugin's answer, with a timeout and an `on_timeout`) is this page's inline
  tier T1, seen from the engine. The batch, budget and circuit breaker of section 7.6 are how the host carries an
  `ask` to the plugin.
- Its `inline` role puts a rule set on one direction of a link, through one optional stage on a route in `hub`.
  A behaviour modifier uses that same stage (section 7.6, decision 5). `vss_core` gets one stage, not one for
  rules and another for plugins.

### 6.1 The problem

A round trip from the engine to another process is tens to hundreds of microseconds when everything is fast, and
milliseconds when the plugin is busy, paged out or being scheduled with other work. A plugin on the path of a byte
cannot promise a latency, so any response that has to arrive at a given time cannot depend on one. The cases:

| Case | Needs |
|---|---|
| A fixed or random delay on a link or a reply | Time exact to the engine's timer, not to a process wake-up |
| "When this arrives, answer with that, after that long" | The same, plus a match on the bytes |
| A fault on cue: drop, corrupt, truncate, duplicate a reply, drop a line | A decision at the moment the bytes pass |
| A repeatable run | A seeded random source inside the engine, one clock |
| RS-485 turnaround | Already in the engine (`half_duplex`), unchanged |
| "Reply depending on the state of a simulated instrument" (a measured value, a mode) | Logic. This is the plugin's job, and its latency is the plugin's latency |

### 6.2 Proposal: three tiers, and the plugin chooses the tier by what it declares

| Tier | Where it runs | Latency | What the plugin does |
|---|---|---|---|
| **T0 Engine rules** | In the engine's process on the port's strand, from a table the plugin sent | Timer exact; no process hop | Sends a *rule set* and changes it between events. Is not on the path at all |
| **T1 Inline** | In the plugin, with the engine waiting up to a declared budget | Round trip plus the plugin's work, bounded by a timeout | Receives batches of bytes and returns a decision for each (section 7.6) |
| **T2 Async** | In the plugin, never waited for | Not bounded, not on the path | Reads copies of traffic and sends commands (generate, change a rule, inject a fault) |

**The rule: if a behaviour can be written as a rule, it must be, and the engine runs it.** A plugin that wants a reply
50 ms after a request sends a rule, not a reply at 50 ms. The plugin decides *what* (the table), and the engine decides
*when* (the timer). A plugin that needs to compute the reply from state uses T1 or T2, and its manifest says so.

### 6.3 What an engine rule is

The language is the one that already exists as `vss::responder_script` in `libs/vss_studio/include/vss/studio/responder.hpp`
(match with a mask on a byte stream, reply, delay, jitter, a seed, a poll list), made larger in a few steps and given
a name. It lives in `vss_studio`, not in `vss_core`. A rule set is data: the plugin sends JSON that is parsed by the
same parser as the script file (unknown keys are errors), and nothing in it is code.

A rule is `{trigger, guard, actions, limits}`:

| Part | Content | Status |
|---|---|---|
| Trigger | A byte pattern with a mask in a stream (exists). Also: an idle gap of at least N microseconds; a line change (DTR, RTS); "bytes received count reaches N"; a timer from a poll list (exists) | Pattern, mask and poll exist. The rest is new |
| Guard | A rule fires `count` times (once, N times, always); only in step k of a named sequence; every nth match; with probability p drawn from the seeded source | New |
| Actions, in order | `reply` bytes (exists); after a delay and jitter (exists, in milliseconds; new: microseconds and distributions: fixed, uniform, normal truncated, a list to cycle through); `drop` the trigger's bytes; `corrupt` (flip bits at positions or with a rate); `truncate` to n bytes; `duplicate`; `set_line` (DTR, RTS, BREAK) after a delay; `raise_fault` (parity, framing, overrun, break, hang-up, disconnect), which `studio::inject_fault` already provides; `emit_event` with a tag (so a plugin or a harness can be told it fired, asynchronously) | Reply and delay exist; the others are new |
| Limits | Rules per set (256), actions per rule (16), reply length (4 KiB), total pending timers (1024), delay (up to one minute, as `max_link_delay_ms`) | New |
| Seed | One seed for a set, and every random choice draws from a generator the engine owns, so the same input gives the same output | Exists for jitter |

The engine reports each firing as an event (time, rule, action), which is what a harness reads to learn what
happened. The time is the engine's clock, which is the single clock the editions page asks for.

What is **not** a rule: anything with a loop, a variable or a call. If someone needs arithmetic on the reply (a
checksum, a counter in the payload), the rule may name a **template** with a small fixed set of functions: `crc16` and
`crc32` over a range, `lrc`, `sum8`, a counter with a start and a width, and a random byte string of n bytes from the
seeded source. That covers the framing of the common instruments without a program. The set is closed; adding a
function is a change to Studio, with a version.

### 6.4 How a plugin declares it

In the manifest, each instance kind says how fast its answers must be:

```json
"timing": {
  "class": "rules",          // "rules", "inline" or "async"
  "budget_us": 2000,         // inline only: the most the engine waits
  "on_timeout": "fail_open"  // inline only: "fail_open" or "fail_closed"
}
```

The class is a promise the host holds the plugin to. An instance declared `rules` may send rule sets, patterns and
replies that the engine times (`after_us`). A plain write from it, whose timing would be the plugin's own, is refused
with `not_declared`. An `inline` instance that does not meet its own `budget_us` is flagged `degraded: timing`
(section 7.6). The Plugins page shows the class beside the plugin, so that a user knows which plugin is carried by
the engine and which is in the path.

### 6.5 Alternatives, and why not

| Alternative | Why not |
|---|---|
| Every response goes through the plugin and the plugin is told to be fast | Cannot be kept. The scheduler, a page fault, or a garbage collector in a plugin written in a managed language decides. A test that depends on 5 ms precision would be a test of the machine's load |
| A shared memory ring read by the plugin with a spinning thread | Lower latency (a few microseconds) and a lot more danger: the engine's data is mapped into a process we do not trust, the plugin burns a core, and it would be platform-specific code with its own `#if`s. Rules reach the same timing by not asking |
| Compiled filters loaded into the engine (a C ABI plugin or WASM) | Runs untrusted code in the engine's process, which is the thing this design exists to avoid. WASM would be the least bad and is a large dependency; it stays open as the in-process option of section 16 |
| A scripting language in the engine | The engine would carry an interpreter, its parser, its limits and its security. A closed rule table does most of what test labs ask, is verifiable on load, and has no way to loop |
| Only T1 with a short timeout | The timeout is a budget, not a guarantee, and it is paid on every chunk |

### 6.6 What this needs from the engine

| Need | Where | Change to `vss_core` or the JSON schema? |
|---|---|---|
| Run a rule set on an endpoint | `vss::responder` in `vss_studio` already does this for a port a program would open, using `endpoint::write` | No. The extensions (new triggers, guards, actions, microseconds) are in `vss_studio` |
| Rules and modifiers on a **link** (hold, corrupt, drop or duplicate what crosses it) | The route stage in `hub` that `docs/response-timing.md` adds (its decision 2) | **Yes, `vss_core`.** The owner asked for it (decision 5). The stage is null unless something is installed, so a link with nothing on it behaves as before |
| Delay with microsecond resolution | `delay_line` in `vss_core` is in milliseconds | Not needed for rules on a port (they use a timer in `vss_studio`). For links it is part of the same question |
| A plugin's rule set saved with the topology | The JSON schema | **No.** The rule set is part of the plugin instance's own state (section 7.8), stored beside the topology by the agent, not in it |

`capabilities` gains flags by name: `engine_rules` (T0 exists in the connected agent), `rules_link` (rules on links),
`rules_microseconds`. A plugin manifest lists the ones it needs in `requires.capabilities`. When a flag is missing the
plugin is `disabled: capability_missing` or its instance refuses with `unsupported`, and the page says "Not available
in this build". Nothing pretends to carry out a rule it cannot.

### 6.7 Why this is the right split

- **Determinism.** The same rule set and seed give the same run. A plugin on the path cannot give that.
- **Failure safety.** A crash of the plugin does not stop the rules. Test rigs survive a plugin that falls over.
- **Verifiability.** A rule set is data. The host can bound it, validate it and show it to the user. Code in a plugin
  cannot be bounded before it runs.
- **One clock.** Timestamps and firing times are from the engine, so reports agree.
- **Cost of being wrong is small.** If the rule language turns out too small, a plugin falls back to T1 for the case
  and loses only precision, not function.

## 7. The plugin API

**What is public and what is not** (decided by the owner).

- **The plugin API is public, documented and free to use.** Anyone may write a plugin against it, in any
  language, and need ask nobody. This section, the manifest of section 13 and the formats of sections 19 and 20
  are its documentation, and they are part of this repository under its licence.
- **The official plugins built on it are private.** Their source is not part of this repository. They use the
  same API as everyone else: no part of it is kept for them.
- **The agent's own protocol is private.** How Studio, `vss_cli` and the agent talk to each other
  (`libs/vss_agent_proto`) is an internal matter that changes without notice, and a plugin never speaks it. The
  plugin API borrows its framing (below) and nothing else.

### 7.1 Transport and framing

- **A private channel for each plugin**, made by the host before the plugin starts and inherited by the plugin: an
  anonymous pair of pipes (Windows, inherited by an explicit handle list, so nothing else is inherited) or a
  `socketpair` (elsewhere, with close-on-exec set on every other descriptor). There is **no name** to guess, no
  listening socket and no way for another process to connect. The plugin is told the handle number in its
  environment variable `VSS_PLUGIN_CHANNEL`.
- **Framing is the agent's** (`libs/vss_agent_proto`): `u32 size`, `u32 header size`, a JSON header, a blob, with the
  same limits (frame at most 1 MiB, JSON depth 8, text 1024 bytes, lists 4096). The same decoder, and the same fuzz
  harness, is reused. Bytes of traffic travel in the blob, never in JSON.
- A plugin's standard input is closed. Its standard output and error are read by the host and kept as log lines
  (section 7.3), at most 64 KiB per second, truncated, never trusted as markup.
- No shell, no environment other than the minimum (`VSS_PLUGIN_CHANNEL`, `VSS_PLUGIN_DATA`, `VSS_PLUGIN_ID`, a
  sanitised `PATH`, `HOME` set to the data directory, and `LANG`).

### 7.2 Versioning

Two numbers, in the `hello` of each side: `api` as `major.minor`.

- A **minor** change only adds: new optional fields, new operations, new capability names. A reader ignores unknown
  *optional* fields of a **known** message (the opposite of the manifest, section 13, where unknown keys are
  errors).
- A **major** change may break. A plugin asks for a range in its manifest (`requires.api`: `{"min": "1.0", "max": "1.x"}`);
  the host picks the highest version both support and says it in `hello_ack`. The host in the first release supports
  only 1.x. If the range excludes it, the plugin is `disabled: api_incompatible` with both numbers shown.
- This is a deliberate difference from `agent.md`: the client of the agent ships with it and gets no compatibility
  code, but plugins are shipped by other people on other schedules, so a plugin must keep working across Studio
  releases within a major version. **Two things never change at all: the messages `hello`, `hello_ack`, `shutdown`
  and the error reply, and the meaning of the frame header.**
- Operations are named and namespaced (`generator.start`). A message the host does not know is answered with
  `error: unknown_op` and the channel stays open. A plugin that sends more than 20 of them in a minute is killed as
  `misbehaving`.

### 7.3 Messages common to all plugins

Request and reply use the id scheme of the agent protocol: the sender picks a non-zero `id`, the reply repeats it.
Either side may send requests. A request that waits more than its timeout (default 10 s; the table below names the
exceptions) is failed with `error: timeout` and, for the host, counts as a miss towards `unresponsive`.

| Message | From | Content |
|---|---|---|
| `hello` | plugin | `api` (`major.minor`), `plugin_id`, `pid`, `version`. The channel is the identity; the id is checked against the manifest |
| `hello_ack` | host | The negotiated `api`, the **granted** permissions, the parameter values (already validated), the engine capability flags by name, the limits (frame rate, bytes per second, instances, queue sizes), `clock` (see below), the plugin's data directory |
| `ping`, `pong` | either | Liveness |
| `status` | plugin | `level` (`ok`, `warning`, `error`), a text of at most 200 characters, a `code` from the manifest's declared messages (optional) |
| `log` | plugin | `level` and text. Rate limited. Never shown as markup |
| `params.changed` | host | Instance id, the changed parameters with validated values. The plugin acknowledges with a reply (`ok`, or `error` with a reason that Studio shows beside the parameter) |
| `instance.create` / `instance.stop` | host | Section 7.4 and after |
| `shutdown`, `bye` | host, plugin | Section 5.6 |
| `error` | either | `{category, value, message}`. Categories: `vss.plugin` (the plugin API's own: `unknown_op`, `bad_request`, `refused`, `not_granted`, `not_declared`, `timeout`, `busy`, `unsupported`, `plugin_unavailable`), `vss` (the engine's, passed through) |

`clock`: the host gives the plugin a monotonic base: `{"epoch_ns": n}` and every timestamp in the API is nanoseconds
since that base on the **host's** clock. A plugin does not tell the host what time it is, and a timestamp it sends
(where the API accepts one) is checked against the host's clock and clamped.

**Messages a plugin must not be able to send:** none that makes the host do something the plugin was not granted.
Every operation is checked against the permissions in `hello_ack`, at the moment of the call, not at the start.

### 7.4 Generators (T2, or T0 with a pattern)

A generator produces bytes onto a port.

| Message | From | Content |
|---|---|---|
| `generator.start` | host | `instance`, `port` (an endpoint id), `params` |
| `generator.write` | plugin | `instance`, blob. Needs credit |
| `generator.credit` | host | `instance`, `bytes`: how many more bytes the plugin may send |
| `generator.pattern` | plugin | `instance`, a *declared pattern*: `{"bytes": hex, "repeat": n or 0, "period_us": n, "burst": n, "jitter": ...}`. The engine generates it (T0) |
| `generator.stop` | host | `instance` |
| `generator.done` | plugin | `instance`, a reason |

**Flow control by credit.** The host grants at most 64 KiB of credit to start and tops up as the engine takes bytes,
so a plugin can never get ahead of the port by more than that, and a plugin that writes without credit is refused with
`busy` (the first time) and killed as `misbehaving` (when it keeps going). A generator is a source of async traffic. When it must be periodic and exact, it uses
`generator.pattern`, which the engine runs.

### 7.5 Fixture types (T0 plus T2)

A fixture type simulates a device. A fixture is an endpoint the agent makes (a mock, like Studio's own fixtures), bound
to an instance. The plugin sees what the other side writes and decides how to answer.

| Message | From | Content |
|---|---|---|
| `fixture.create` | host | `instance`, `endpoint`, `params` |
| `fixture.data` | host | `instance`, `ts_ns`, blob: bytes that arrived at the fixture. Coalesced: up to 8 KiB or 1 ms of arrivals in one frame |
| `fixture.line` | host | `instance`, `ts_ns`, the new state of DTR, RTS (for a fixture these are the lines the program set) |
| `fixture.reply` | plugin | `instance`, blob, `after_us` (optional): bytes to write to the fixture. With `after_us`, the **engine** holds the bytes and writes them at the time (a one-off rule), so the delay is exact, not a delay of the plugin's |
| `fixture.line.set` | plugin | `instance`, DTR, RTS, `after_us` |
| `fixture.fault` | plugin | `instance`, a `fault_kind` |
| `rules.set` | plugin | `instance`, a rule set (section 6.3). It replaces the instance's set atomically. The reply says whether it was accepted, with the position of the first error otherwise |
| `rules.event` | host | `instance`, `ts_ns`, `rule`, `action`: a rule fired |

Most fixtures send one `rules.set` when created and again when a parameter changes. The device behaviour (what a
"Model 2000 multimeter" answers to `*IDN?`) is a rule set the plugin builds from its parameters. The plugin process
can then die and the fixture still answers. **State that changes** (the reading) is `rules.set` with a new reply, sent
whenever the value changes; a rig that needs the reading to be a function of time uses T1/T2 and accepts the latency.

**Decided by the owner:** a plugin's fixture types appear in the Advanced toolbox, only while the plugin is installed
and switched on, and are marked as provided by a plugin with the plugin's name and trust level. A type of a plugin
that the user switched on and that cannot run now (starting, failed, no licence) is shown disabled with the reason;
a type of a plugin that is off or cannot be switched on is not shown. `screens/10-plugins.md` has the rule, the
mark and the strings. For that the host's view of a plugin carries, for each fixture type, its id, name,
description and parameters, taken from the verified manifest, so the toolbox never asks a plugin process anything.

### 7.6 Behaviour modifiers

A modifier changes traffic on a link. It is the most dangerous extension point, since it sits in the path of bytes, so
it has the most rules.

**How it sits in the path.** By a stage on the route, inside the engine. This is decision 5: the owner asked for a
route hook in the first version, in place of the earlier plan to insert two hidden endpoints into the link.

`docs/response-timing.md` adds one optional stage to a route for its `inline` rule sets. It sits after the owner
policy and before the half-duplex line and the link's delay line. A modifier is a second user of **that same
stage**. The stage hands the chunks of one direction of one link to the host, the host carries them to the plugin
and gives the stage the result.

- No endpoint and no link is made for a modifier. Nothing internal has to be hidden, kept out of the saved setup
  or put back afterwards.
- When an instance goes away, its part of the stage is removed and the bytes it held are released in order,
  ahead of anything routed later, as that page says for an `inline` set that is removed.
- With nothing installed the stage is a null pointer and a link behaves as it does today.
- When a link has both a rule set and modifiers, **the rule set runs first** and the modifiers follow in the
  order of the link's bindings (section 7.8). The reasons:
  - A rule then matches the bytes the program sent, at the time the engine read them. What a rule set does on a
    run does not depend on a plugin being fast, slow or stopped, which is what `docs/response-timing.md` promises
    for rules. The other order would put a plugin's round trip and its failures in front of every rule.
  - A modifier that fails open passes bytes unchanged. With rules first that changes nothing for the rules.
  - A recording replays into the rules as it was recorded.
  - The cost: a rule's delay is no longer the last thing before the wire, because the modifier's round trip
    follows it. So a link direction that has a modifier is marked "not exactly repeatable" in a run, and the
    run's report names the modifier.
  - Weighed and not taken: refusing a rule set and a modifier on the same link direction. It is simpler, and it
    stops the common case of a scripted fault with a plugin that corrupts frames.

  The owner agreed to this order on 13 November 2026, and `docs/response-timing.md`, which owns the stage, says
  the same ("Where a rule set sits" and "Determinism"). From that page: the stage keeps a direction's bytes in
  order from one user to the next; when a modifier is removed, the bytes it had not yet answered for pass on
  unchanged first; and bytes a rule still holds go on when they are due.
- Two capability flags cover the one stage: `rules_link` for `inline` rule sets, which is that page's, and
  `route_hook` for modifiers.
- The capability `route_hook` says the engine has the stage and the host can use it for modifiers. Without it a
  modifier is `disabled: capability_missing` and the page says "Not available in this build".

So the modifier slice comes after the slice of `docs/response-timing.md` that adds the stage.

| Message | From | Content |
|---|---|---|
| `modifier.create` | host | `instance`, `link`, `direction` (`a_to_b`, `b_to_a` or both as two instances), `params`, `budget_us`, `max_batch_bytes`, `max_batch_us`, `on_timeout` |
| `modifier.batch` | host | `instance`, `seq`, `ts_ns` of the first chunk, `chunks`: list of `{offset, length, ts_ns}`; the bytes as one blob |
| `modifier.result` | plugin | `seq`, `actions`: one for each chunk, in order, and a blob for what replaced them |
| `modifier.stop` | host | |

**Batching.** The host collects the chunks the stage hands it into a batch, and sends it when the first of these happens:
`max_batch_bytes` (default 4 KiB), `max_batch_us` since the first chunk (default 200 µs), or the arrival of a byte the
plugin declared as a flush trigger (`flush_on`, optional). At most **one batch is outstanding** per instance and
direction: bytes that arrive while a batch is out wait in a bounded queue (default 64 KiB). That keeps order, caps the
memory, and makes the plugin's speed the throttle of the link instead of letting the link flood the plugin.

**Actions** for a chunk: `pass`, `drop`, `replace` (a range of the result blob), `delay_us` (the engine holds the
bytes; the delay is T0, exact), `split` (deliver in n parts with gaps, also engine timed), `insert_before` and
`insert_after` (a range of the blob), `fault` (a `fault_kind` raised at the time the chunk passes). They are a closed
set, so the host can bound the output: the result may be at most 4 times the batch (plus 1 KiB) and the sum of delays
at most the one minute limit. A result that breaks a bound is a failure of the whole batch (the timeout rule applies)
and counts toward the circuit breaker.

**Timeout and failure.** `budget_us` is what the engine waits for one result. When it passes:

| `on_timeout` | What the bytes of that batch do | What follows |
|---|---|---|
| `fail_open` | Pass unchanged, in order | Later bytes go through the plugin again; a late result for an old `seq` is discarded |
| `fail_closed` | Are dropped, and counted | Later bytes go through the plugin again; the link has lost data, as a broken wire would |

Three timeouts in a row, or five in a minute, **trip a circuit breaker**: the instance is bypassed (`fail_open`) or the
link is faulted (`fail_closed`: the host disconnects the instance's path and raises `bus-fault`-style events and a
visible state on the link) until the user resets it or the plugin shows five good answers in a row after a probe. The
user sees the state, not just a log line (section 10). The manifest sets a default; the **user may override it per
instance**, and the administrator's policy may force `fail_closed` or `fail_open` for all.

Which default? **`fail_open` is the default for a modifier.** A modifier is a test aid. A hung plugin that stops all
traffic on a link looks, to the user and to the program on the other end, like a broken cable, and the cause is
hidden. Failing open shows traffic *unmodified* and says so loudly. A test that must not run unmodified selects
`fail_closed` for itself, and gets the link marked as failed.

**What it costs.** The inline path costs the round trip: expect 50 to 300 µs on an idle machine, and the host
measures it (a 99th percentile over the last 10 000 batches) and shows it on the instance. It is a number the user can
read, not a promise. A plugin that declares `budget_us` below what the host measures for it on this machine is
`degraded: timing`.

**Async modifiers (T2)** do not hold bytes. They receive a copy of the link's traffic (`traffic.tap`, bounded queue
with a drop count) and send commands: `rules.set`, `fault`, `generator.pattern`, `inject`. A latency in them delays
only what they decide, never the bytes.

### 7.7 Automation adapters

An adapter lets another tool drive Studio. The plugin's process holds the connection to the outside tool (a test
harness, a language binding's server, a monitoring source) and translates between it and the **agent proxy**.

| Message | From | Content |
|---|---|---|
| `agent.call` | plugin | `op` and its arguments, as in the agent protocol (section 5 of `agent.md`). Only the operations in the plugin's granted set are allowed |
| `agent.subscribe` | plugin | The agent's event stream, filtered by the grant (counters only, or taps) |
| `adapter.state` | plugin | `listening` and the **exact address** it listens on, if any, and the protocol name. Shown on the page |

Rules for the proxy:

- Granted operation groups are separate permissions (section 9.1): `agent.read` (list, view), `agent.traffic` (taps and
  the event log), `agent.send` (write bytes), `agent.topology` (create, remove and connect ports and links),
  `agent.faults` (faults and rules). A group not granted returns `not_granted`.
- Every call is **attributed**: the agent logs the plugin id and the operation, and an operation that changes the
  topology is recorded as made by that plugin, in the same record that already says which client made a port. The
  audit record is local, in the agent's log, without traffic bytes.
- The proxy does **not** let a plugin do what a client cannot: it is the same entry point as a client connection, with
  a smaller set of operations, and it is no way to reach `shutdown` of the agent, a plugin's own settings, trust or
  policy. Those are operations of Studio, never of the proxy.
- An adapter that listens on a network socket asks for `net.listen` with a bind (loopback by default). The security of
  that endpoint (tokens, administrator control) is the remote-control work, which owns the rules for what an adapter
  must do before it accepts a connection. **This page requires only that the address be declared, granted, shown and
  never wider than the grant.**

### 7.8 Parameters and state

Parameters are the only way a user's settings reach a plugin. Types, all with limits in the manifest:

| Type | Constraints | Rendered as |
|---|---|---|
| `bool` | | A check box |
| `int`, `number` | `min`, `max`, `step`, `unit` | A spin box with the unit |
| `enum` | A list of `{value, label}` | A combo box |
| `string` | `max_length`, `pattern` (a restricted grammar, not a general regex, to avoid catastrophic matching) | A line edit |
| `bytes` | `max_length`, format `hex` | A hex field |
| `duration` | `min`, `max`, unit | A spin box with a unit |
| `port`, `link` | Which kinds | A choice among the existing ones |
| `path` | `file` or `directory`, `must_exist`, read or write | A browse button; the chosen path is checked against the grant |
| `secret` | `max_length` | A hidden field. The value is stored by the agent in the plugin's private data only, never in the topology, a log or a support bundle |

Labels and help texts come from the manifest, may be given in several languages (`labels.en`, `labels.de` and so on,
the best match is shown), are plain text only, and have limits. **A manifest string is data, never markup, and is
never used as a format string.** The GUI wraps its own words in `tr()`; a plugin's words are shown as they are.

The page is built from the parameters by one generic form with the accessible names the GUI rules require (the
parameter's label is its accessible name, the unit is part of it), keyboard order from the declared order, and
targets of at least 24 px. A plugin cannot change that.

Validation is done by the **agent** from the manifest, then the values are sent. A plugin is always sent valid
values, and a plugin may still refuse a combination with a reason (`params.changed` error), which the form shows
beside the field.

**State, and the setup that uses a plugin.** Two things are kept, in two places:

| What | Where | Why there |
|---|---|---|
| The **binding**: this port is a fixture of type T from plugin P, or this link has behaviour T from plugin P, with its parameter values (never a `secret`) and the `on_timeout` choice | With the setup, so it travels with the file | A setup opened on another computer, or after the plugin is removed, must still say what it was made of, and must save again unchanged |
| The **run state**: the rule set the plugin last sent, secrets, anything the plugin stores | The agent's private area, `plugins/<id>/instances/` | It is this computer's, can be large, and is rebuilt by the plugin |

Rules for the binding, whatever the plugin's state (the screens are in `screens/10-plugins.md`):

- **A setup is never changed or refused because a plugin is missing, off, blocked, unlicensed, failed or
  incompatible.** The port stays a port, as a plain fixture that says nothing, with its name, place and links. A
  link with a behaviour passes data unchanged (or is faulted, if the user chose fail-closed). A generator stops.
- The binding is read and written back **as it was read**, including a plugin id and parameters this Studio knows
  nothing about. Parameters are validated only when the plugin is present; an unknown plugin's values are kept
  as opaque JSON, with size and depth limits.
- When the plugin becomes available, its instances are made again from the bindings, without a prompt.
- Only the user removes a binding.

**Where the binding is written (decision 13): in the topology file.** The owner asked for this additive change to
the schema, so that a setup that is shared says which plugins it needs and where to get them. A port or a link
may carry a list:

```json
"plugins": [
  { "id": "com.example.modbus-fixture", "kind": "fixture", "type": "meter", "name": "Modbus fixture",
    "publisher": "Example Ltd", "key_id": "<hex>", "version": "1.2.0",
    "get_it_from": "https://example.com/plugins",
    "params": { "address": 17 }, "on_timeout": "fail_open" }
]
```

- A port has at most one fixture type or one generator, and any number of decoders. A link has any number of
  modifiers and decoders. **The order of the list is the order modifiers are applied in.**
- `kind` is what the plugin provides there: `fixture`, `generator`, `modifier` or `decoder`. A kind this
  Studio does not know is kept as it is, so a newer setup still opens.
- `id` is the plugin's manifest id, which is the identifier. There is no second identifier such as a UUID. For a
  signed plugin `key_id` says whose it was, so that a different plugin with the same id is noticed.
- `name`, `publisher`, `version` and `get_it_from` are for a person who opens the setup without the plugin.
  They are plain text with the manifest's text rules. **Studio shows `get_it_from` and never opens or fetches
  it.** It is copied from the manifest's author address when the binding is made.
- A `secret` parameter is never written.
- A reader that does not know the plugin keeps every entry and writes it back as it was read.

This is a change to the schema and to `vss_config`'s reader and writer. `vss_core` is not touched. The key is
optional and the file's `version` stays 1: the reader has always passed over keys it does not know, so an older
Studio opens a setup that has the list, and leaves the list out when it saves. The alternative that was not
taken is a file beside the setup (`<name>.plugins.json`), which needs no schema change but is lost when a
person sends only the setup. The running setup that the agent keeps (`running-setup.json`) holds the same list.

### 7.9 Limits per plugin (defaults, set in the host, never raised by a manifest above the host's maximum)

| Limit | Default |
|---|---|
| Instances | 16 (maximum 256) |
| Frames per second in | 2000 sustained, burst 4000 |
| Bytes per second in, and out | 8 MiB |
| Outstanding requests | 32 |
| Log lines | 100 per second, 64 KiB per second |
| Outstanding inline batches | 1 per instance and direction |
| Rule sets | 256 rules each, 4 sets |
| Open files, processes, memory | By the sandbox (section 9.3) |

Over a rate: the host stops reading the channel for the rest of the second (back-pressure), and a plugin that keeps
doing it for 10 seconds is killed as `misbehaving` with the limit's name.

## 8. Trust, signing and verification

### 8.1 Trust levels, with what is checked

| Level | Condition | The user sees | Loads |
|---|---|---|---|
| Official | The signature verifies with a key that the **official key list** names for plugins (section 8.9) | The Virtual Serial Studio badge and "Signed by Iain Chesworth Labs" | Yes (if the licence permits) |
| Third-party signed | The signature verifies with a key that is not official, and the user has trusted that key | The author's name from the key's record and the label "Not verified by Virtual Serial Studio" | After the user trusts the key and the plugin |
| Third-party unsigned | No signature | "Unofficial plugin. It can do anything Studio can." | After an explicit confirmation, and only in a sandbox of at least `partial` unless that is overridden (section 9.2) |
| Signed, key unknown | A signature that names a key found nowhere on this computer. It **cannot be checked**: a signature names its key by id only, and the key itself is not in the file | Treated as unsigned for the decision, with the key's id shown: "Signed by an author you have not trusted" | As unsigned, or after the key is trusted |

A signature **proves who signed it and that the files were not changed afterwards**. It does not prove that the plugin
is safe. The interface never says that a plugin is verified. The word appears in one place only, the label of a
plugin that someone else signed: "Not verified by Virtual Serial Studio" (decided by the owner).

### 8.2 Files and the signature

```
plugin.json        the manifest, readable as it is
plugin.sig         a vss::signed_file envelope, purpose "plugin"
<other files>      listed in the manifest with their hashes
```

The signature is **not a second scheme**. `docs/licensing.md` proposes one envelope for every signed file,
`vss::signed_file` (`{format, purpose, key_id, payload, signature}`, Ed25519, the signed message being
`"VSS-SIGNED-FILE-1" 0x00 <purpose> 0x00 <payload>`), with a trust list whose entries carry a key id, a purpose,
a public key and a validity window. A plugin signature is that envelope with the purpose `plugin`:

```json
payload = { "plugin_id": "com.example.modbus-fixture",
            "version": "1.2.0",
            "manifest_sha512": "<hex of the exact bytes of plugin.json>" }
```

- The payload names the manifest by the hash of **its exact bytes**, so there is no canonical form to get wrong,
  and `plugin.json` stays a plain file a person can read. The verifier hashes the manifest bytes it holds and
  compares, then checks `plugin_id` and `version` against the parsed manifest.
- The purpose is inside the signed message, so a licence signature can never pass as a plugin signature, nor a
  plugin signature as a revocation list (purpose `plugin-revocations`).
- The manifest lists every file as `{path, size, sha512}`. The verifier checks each, and that no other file is in
  the directory.
- The public key comes from the official key list (section 8.9), from the administrator's `allow_keys` or from
  the user's approved keys, never from `plugin.sig` and never from the operating system's certificate store. A key of another purpose does not verify a plugin, even if it is official.
- Delegation (an official key signing an author's key) is not in the first version.

This makes `libs/vss_plugin` depend on the `vss::signed_file` part of `libs/vss_license`, which the licensing work
builds. If that part is not merged when slice 1 starts, slice 1 begins with the manifest parser and the hashing,
and the signature check follows (section 18).

**Algorithm choice.** Ed25519, as `docs/licensing.md` proposes, and whichever implementation the owner picks there
(question 5 of that page; a vendored Monocypher is its recommendation and this page's). The reasoning, for the
record:

| Option | Cost |
|---|---|
| A small vendored implementation (Monocypher, about 3000 lines in two files, public domain or BSD-2-Clause) | A new third-party file in `THIRD_PARTY_NOTICES.md`, and the licence audit that is already decided covers it. Same code on every platform, no `#if`. **Recommended** |
| The operating system's cryptography (CNG, CryptoKit, OpenSSL) | Three code paths, and OpenSSL as a dependency on Linux. Against the rule to pick files in CMake and not to use `#if` |
| Qt's `QCryptographicHash` for hashing | SHA-512 only; no signatures |

**Decided (decision 6): the vendored Monocypher.** Signatures as X.509 certificate chains from the owner's own
certificate authority, checked by each operating system's code, were weighed in the review and not taken: Linux
has no such service of its own, three implementations would have to agree, and licence files would have had to
follow or use a second scheme. The envelope carries a `format`, so a second kind of signature can be added for
third parties later without changing how official plugins are checked.

### 8.3 The hash that a decision is tied to

`plugin_hash` is **SHA-512 over the sorted list of `path NUL size NUL sha512-of-file NL` for every file**, computed by
the verifier, for every plugin, signed or not. For a signed plugin the manifest's own list must equal it exactly.
The user sees the first 16 hex characters. A trust decision is stored for `(plugin_id, plugin_hash)`, so any change
to any file, a rename, an added file or a different manifest is a different hash and asks again.

Because the executable may itself load more files (an interpreter's libraries), the manifest says what it runs
(`runtime`), and a plugin that needs an interpreter lists the **interpreter in `requires.runtime`** by name and the
host reports it but does not hash the system's copy. A plugin should ship what it runs. This is stated on the Plugins
page ("Uses the Python on this computer. Its libraries are not covered by the signature.") because it is a real hole
in what a signature proves, and hiding it would make the badge a lie.

### 8.4 Trust decisions

The store is `trust.json` in the agent's private directory (mode 0600 / per-user ACL), written atomically (write,
flush, rename).

| Record | Content |
|---|---|
| `keys` | `key_id`, the public key, the name the user saw, when and by whom it was trusted |
| `plugins` | `plugin_id`, `plugin_hash`, trust level at the time, `enabled`, the permissions the user **granted** (a subset of the declared), the date, the sandbox level required |
| `ledger` | `revocation_serial`, the clock high-water mark (section 8.5) |

- The decision dialog lists what the plugin declares: the extension points, **every permission with a plain sentence**,
  the signer, the hash, and the sandbox level this computer can give. The user can untick permissions the plugin lists
  as optional; a plugin that was refused a required permission cannot be enabled.
- **A changed plugin asks again.** New hash, same id: the old record stays (for "last trusted version") and a new
  decision is asked. A change that only **adds a declared permission** is called out ("This version asks for more
  than the last one: network access").
- Trusting an author key is a decision about *future* plugins by the same key. It is shown separately and can be
  undone, and undoing it moves their plugins back to `untrusted`.
- No decision is ever taken by a plugin, a file in a plugin directory or the agent's protocol. Only the Studio
  window's prompt writes to the store, and `vss_cli plugin trust` after an interactive confirmation.

### 8.5 Revocation, expiry and the clock

There is no network, so:

- **The revocation list** is a signed file, `revocations.json`, shipped inside each release (and importable by the
  user from a file: Plugins > Import revocations). It holds a `serial` (increasing integer), `issued_at`, and lists of
  revoked `key_id`s, revoked `plugin_hash`es and revoked `plugin_id` + `max_version`. It is signed with an **official
  key** and the envelope has its own purpose, `plugin-revocations`. The verifier accepts a list only with a
  higher `serial` than any it has seen, and stores the highest in the trust ledger. A list that fails to verify is
  ignored and reported; a list with a lower serial is ignored silently.
- A revoked plugin or key is `rejected: revoked` at the next check. A running plugin is stopped within a second of
  the list being applied.
- **Expiry.** The manifest has `not_after` (a date; official plugins get a bounded lifetime, and the owner's signing job
  chooses it). **How long is not settled.** The owner's first thought was 28 days. This page recommends against
  a period that short: an expired official plugin does not run and has no "Run anyway", and there is no network,
  so every official plugin, the ones that come with Studio included, would stop 28 days after it was signed
  until the person fetched and installed a new copy by hand, every month, and a computer that is kept offline
  would lose them for good. The recommendation is that `not_after` runs to the signing key's `valid_to` (two
  years in the example of section 8.9), that a plugin signed with a mistake is stopped by the revocation list,
  and that anything meant to lapse monthly is the licence's period (`docs/licensing.md`), which has a grace
  period and a renewal path built for it. An expired plugin is `rejected: expired`. There is a grace choice: the page offers "Run anyway for this
  session" only for **unsigned and third-party** plugins, never for an official one, since an official plugin has an
  update path and a user can always take it.
- **The clock.** The wall clock is only trusted to go forward. The ledger keeps the highest time seen (from the
  system, from a revocation list's `issued_at`, from a signature date). An expiry is checked against
  `max(system now, high-water mark)`. Setting the clock back does not revive an expired plugin. This is a
  **deterrent**, as for licences in `editions.md`: a user who deletes their own ledger defeats it, and the page says
  nothing stronger.
- **Keys** come and go by the official key list of section 8.9. A signing key that is replaced is taken out of
  that list, which revokes it. The first version has no date inside a signature, so there is no "signed before
  the key ended" case to judge.

### 8.6 Administrator policy

A policy file `policy.json` in the machine configuration directory (`%ProgramData%\VirtualSerialStudio\`,
`/etc/virtual-serial-studio/`, `/Library/Application Support/VirtualSerialStudio/`):

```json
{
  "policy_version": 1,
  "allow": "official_only",         // official_only | signed | any (default: any)
  "allow_keys": [                   // with "signed": these third-party keys are pre-trusted
    { "key_id": "<key id>", "public_key": "<64 hex characters>", "name": "<author>" }
  ],
  "deny_plugins": ["<plugin_id>"],
  "min_sandbox": "partial",         // none | partial | full (default: partial)
  "allow_no_sandbox": false,        // may a user override the minimum for one plugin (section 9.2)
  "user_plugins": true,             // is the per-user plugin directory searched (section 5.2)
  "on_timeout": null,               // "fail_open" | "fail_closed" | null (do not force)
  "permissions_denied": ["net.outbound"]
}
```

- Read at start and when the file changes, on every platform with the same code.
- The file must be owned by an administrator and not writable by the user (Windows: owner `Administrators` or
  `SYSTEM` and no write for `Users`; elsewhere: owned by root, no group or other write bit). **A policy file that
  fails that test, or does not parse, is treated as `official_only`**: fail closed, with a banner: "A policy file was
  found and could not be trusted, so only official plugins will run." A missing file is the default.
- `allow_keys` is how an organisation adds its own signing keys: the administrator lists them here, each with
  its public key, because a signature names its key by id only and the key has to be on the computer already.
  A key id, here and everywhere, is 1 to 64 lower-case letters, digits and hyphens: what a signed file can carry.
  A user who
  controls their own computer approves a key in their own trust store (section 8.4). Neither looks in the
  operating system's certificate store, which trusts every public web authority and would let in far more than
  is meant.
- A policy narrows what the user can do. It never grants beyond what the user decides. A user cannot widen it from
  Studio, and the page says "Set by your administrator" with the reason.

### 8.7 Licence and edition

The host asks the licensing work's one question, as proposed in `docs/licensing.md`:

```
entitlement licence_service::check(feature) ->
    { granted: bool, reason: reason_code, licence_id, days_left }
```

with the feature names `plugins.official` and `plugins.<plugin id>`. The plugin framework owns the plugin ids.
The host calls it when verifying an official plugin, and again when the licence state changes. A plugin that is not
granted is `disabled: not_licensed`; the reason code is the licence service's and the screens show its sentence
unchanged. Verification never looks inside a licence, and the licence code never sees a plugin's files.

Points taken from that page, so the two stay in step:

- **An official plugin checks the licence itself as well**, with the same library, because Studio is GPL and can be
  patched. The host's check is for telling the user the truth; the plugin's own check is the enforcement.
- **A plugin that is running when the grace period ends keeps running until it is stopped** (proposed there, for
  the owner to confirm). The host does not kill on a licence change; it refuses the next start.
- **The version window** (`updates_until`) needs a release date in the signed manifest. The manifest gains an
  optional `released` date for it (section 13).
- A third-party or unsigned plugin is never asked about a licence.

### 8.8 The verifier's interface

A library with no Qt and no I/O of its own beyond reading the bytes it is given (`libs/vss_plugin`, namespace
`vss::plugin`), so that tests drive it with memory and a fake clock. The outline, not final code:

```cpp
struct public_key { std::array<std::uint8_t, 32> bytes; };
struct key_id { std::array<std::uint8_t, 8> bytes; };

enum class trust_level { official, third_party_signed, unsigned_, signed_unknown_key };

struct verify_input
{
    std::span<const std::byte> manifest_bytes;
    std::optional<std::span<const std::byte>> signature_bytes;   // plugin.sig
    std::span<const file_digest> files;                          // measured by the caller
    std::span<const public_key> official_keys;
    std::span<const public_key> trusted_keys;                    // user-trusted
    const revocation_list* revocations;                          // may be null
    std::chrono::sys_seconds now;                                // already max(system, high-water)
};

struct verify_result
{
    plugin_manifest manifest;
    trust_level level;
    plugin_hash hash;
    std::optional<key_id> signer;
};

// Pure: same input, same output. Each failure is a vss::result error with a reason
// from `plugin_errc`, listed in section 12.
vss::result<verify_result> verify(const verify_input&);

// Reading, hashing and trust-store operations:
vss::result<manifest> parse_manifest(std::span<const std::byte>);
vss::result<revocation_list> parse_revocations(std::span<const std::byte>,
                                               std::span<const public_key> official);
class trust_store;   // load, save (atomic), find(plugin_id, hash), add_key, add_decision, ledger
```

`verify` is pure so it can be fuzzed and tested on vectors. The file reading and hashing that produce
`file_digest` are a separate function, so the same function can be run on the open handles at start (section 5.4).

**Keys and the signing job.** The verifier takes public keys as input. The set of official public keys is compiled in
from a file in the repository containing **public keys only**, which the owner places. Nothing in this work creates,
stores or handles a private key, a signing token or a certificate. A signing tool (`vss_plugin_sign`) may exist as a
library function used by the owner's signing job; the repository will not contain a private key, and the tests use
**throwaway keys generated in the test process from a fixed documented test seed, marked TEST ONLY**. The owner is
asked to confirm that fixed test seeds are acceptable (decision 7), since they are test vectors, not secrets.

### 8.9 Keys: the root, the key list and the owner's procedure

Decision 8. Official signatures use plain Ed25519 keys under a key list that a root key signs. It is the shape of a
certificate authority with the certificate replaced by one line of a signed list.

| | What it is | Where it lives | When it is used |
|---|---|---|---|
| **Root key** | An Ed25519 key pair | The private half offline, with the owner, kept as the owner's root certificate authority is. The public half is compiled into Studio (a current one and a next one, so the root itself can be replaced by a release) | Only to sign a new key list |
| **Signing keys** | Ed25519 key pairs | The private half in the secret store of the owner's signing job, or on a hardware token | To sign an official plugin or a revocation list |
| **The official key list** | A `vss::signed_file` envelope with the purpose `key-list`, signed by the root | Public. In every release, and importable from a file as a revocation list is | Read by the verifier |

So there are two secrets, and neither is ever on a computer that runs Studio or in this repository.

```json
{ "key_list_version": 1, "serial": 4, "issued": "2026-11-13",
  "keys": [
    { "key_id": "plugins-2026", "public_key": "<64 hex characters>", "purpose": "plugin",
      "name": "Virtual Serial Studio plugins", "valid_from": "2026-11-01", "valid_to": "2028-11-01" },
    { "key_id": "revocations-2026", "public_key": "<64 hex characters>", "purpose": "plugin-revocations",
      "name": "Virtual Serial Studio revocations", "valid_from": "2026-11-01", "valid_to": "2028-11-01" }
  ],
  "revoked": [] }
```

- **Checking a plugin.** The verifier checks the root's signature on the list, finds the signature's `key_id` in
  it, checks that the key's purpose is `plugin`, that today is inside its dates and that it is not in `revoked`,
  and then checks the plugin's signature with that key.
- **The newest list wins.** The verifier keeps the list with the highest `serial` it has seen, in the ledger of
  section 8.4, so an old list cannot be put back to bring a revoked key to life.
- **How a new list reaches a computer.** There is no network, so: inside a Studio release, beside a plugin (a
  plugin folder may hold a newer `official-keys.json`), or as a file an administrator or a user imports.
- **Replacing a signing key:** add its line, sign a list with the next serial. **Revoking one:** move its id to
  `revoked`, sign a list with the next serial. **A lost root:** a new root needs a new Studio release, which is why
  a next root is compiled in beside the current one.
- **A key's dates cover what it signs.** The signing job refuses to sign a plugin whose `not_after` is later than
  its key's `valid_to`.
- **Licence keys are not in this list.** The owner decided on 13 November 2026 that licence files do not use
  the plugin key list. The list holds the purposes `plugin` and `plugin-revocations` only, and the verifier
  refuses a line with any other purpose. How a licence key is trusted is for `docs/licensing.md` to say, and
  nothing here changes that page. The two share only the `vss::signed_file` envelope, whose purpose string keeps
  a signature of one kind from passing as the other.

**The owner's procedure, in outline.** The owner runs these; nothing in this work creates or holds a key. OpenSSL 3
makes and uses Ed25519 keys, so no other tool is needed for the keys themselves.

```
# Once, on the offline computer: the root key.
openssl genpkey -algorithm ed25519 -out root.key
openssl pkey -in root.key -pubout -out root.pub

# For each signing key.
openssl genpkey -algorithm ed25519 -out plugins-2026.key
openssl pkey -in plugins-2026.key -pubout -out plugins-2026.pub

# The 32 bytes of a public key as hex, for the key list or for Studio's source.
openssl pkey -pubin -in plugins-2026.pub -outform DER | tail -c 32 | xxd -p -c 32

# Sign the bytes of a message (the envelope's signed message, section 8.2).
openssl pkeyutl -sign -inkey root.key -rawin -in message.bin -out signature.bin
```

A small script, written with the verifier, builds the message and the envelope around those last two steps. It is
given the path of a key and calls OpenSSL; it does not make keys and stores nothing. The root public key and the
first key list are placed in the repository by the owner, as public files.

**Other people's keys.** A third-party author makes an Ed25519 key in the same way and signs with it. No root is
involved: the key is trusted by a user in their own store, or by an administrator in `allow_keys`. A key on a
hardware token works where the token can sign with Ed25519.

**Open: how a user gets an author's public key.** `plugin.sig` carries no public key, on purpose: a key taken
from the file being checked proves nothing. So a user who wants to approve an author's key needs the key from
somewhere else, and this page does not yet say from where. The likely answer is a small public key file that the
author publishes and the user imports, with its fingerprint shown before it is approved. Until that is decided a
plugin signed with an unknown key can only be run as an unsigned one.

**What exists.** The root public keys are in `libs/vss_plugin/src/official_roots.cpp` (`root-2026` and
`root-next`). The first key list, as the text that is signed, is `libs/vss_plugin/keys/official-keys.payload.json`;
the README beside it has the owner's command to sign it. `tools/plugin_sign.py` builds the three signed files
around an OpenSSL call.

## 9. Permissions and sandboxing

### 9.1 Permissions

A manifest declares what the plugin asks for; the user grants some or all; the host grants nothing else. Each is a
name and, for some, a scope.

| Permission | Allows | Notes |
|---|---|---|
| `ports.read` | List ports, links, counters and state | Default for most |
| `traffic.tap` | Copies of traffic of the ports and links named by an instance | Bounded queue; drops are counted |
| `ports.write` | Write bytes to the ports of its own instances | |
| `topology.modify` | Create, remove and connect ports and links through the proxy | Audit recorded |
| `modifier.inline` | Hold bytes of a link in the path | Needed for T1 |
| `faults` | Inject faults, set rules | |
| `serial.devices` | Enumerate devices on the computer | |
| `fs.read` / `fs.write` | Read/write below a named directory chosen by the user (a parameter of type `path`) | Scoped; never `/` or a drive |
| `data.private` | Read and write its own data directory | Always granted |
| `bundle.section` | Add a section to a support bundle | The user sees the section, its preview and its tick box in Studio's own dialog (section 19) |
| `net.listen` | Listen on a declared address and port | Loopback unless stated; shown |
| `net.outbound` | Connect to declared hosts | Shown. Not granted by default. Absent for the GUI regardless: the GUI itself has no network access, and the policy can deny |
| `process.spawn` | Start child programs | Child processes are inside the same sandbox and job |
| `clipboard`, `ui` | **Never**. There is no such permission: a plugin has no window |

A permission is **required** or **optional** in the manifest. A plugin refused a required one cannot start; one refused
an optional one starts and is told, in `hello_ack`, what it got.

### 9.2 What each operating system can do

The launcher applies the strongest it can and reports the level it reached (`full`, `partial`, `none`) with the list of
what was **not** applied. Studio shows that, never the level asked for.

| | Windows | Linux | macOS |
|---|---|---|---|
| Process containment | **Job object**: kill on close, active process limit, memory limit, UI restrictions (no desktop, clipboard, handles to others), CPU rate | **Process group**, `PR_SET_PDEATHSIG`, `no_new_privs`, cgroup v2 limits where the user session has a delegated cgroup | **Process group**, `setrlimit` limits, `posix_spawn` with `POSIX_SPAWN_SETSID` |
| Filesystem | **AppContainer** (or a restricted token plus low integrity where AppContainer fails). The plugin directory gets a read ACE, and the data directory a write ACE, for the container's SID | **Landlock** (kernel 5.13 and later) for read-only plugin directory and a read/write data directory; **mount namespace** with an empty root where user namespaces are enabled | **Seatbelt profile** by `sandbox_init`, generated from the grant: plugin directory and data directory only |
| Network | AppContainer without the `internetClient` and `privateNetworkClientServer` capabilities: none; the capability is given only for `net.*` grants | A **network namespace** with no interfaces where user namespaces are enabled; otherwise seccomp denies `socket(AF_INET...)` and `AF_INET6` when the grant is absent | The profile denies `network*` unless granted |
| System calls | AppContainer plus the job restrictions; `ProcessSystemCallDisablePolicy` (Win32k) | **seccomp-bpf** deny-list of dangerous calls (`ptrace`, `mount`, `bpf`, `keyctl`, `kexec_*`, `perf_event_open`, `userfaultfd`), allow the rest | The profile |
| Other processes | AppContainer cannot open processes outside; the job has no access to others | A PID namespace where available; otherwise `ptrace` is denied by seccomp, and `/proc` is not readable beyond itself | The profile denies `process-info*` and `signal` to others |
| What does **not** work | Python or Java runtimes outside the AppContainer-readable area (the host adds an ACE where the user installs one, and says so); a drive under a user profile may not be readable by the container | Distributions with user namespaces disabled (some hardened ones) lose the network and mount isolation; the level is `partial`; kernels before 5.13 lose Landlock | `sandbox_init` is deprecated (but works and is what the system's own tools use); an unsigned binary on Apple Silicon must still be signed ad hoc by the author |

The sandbox is **defence in depth, not the trust boundary**. The boundary is the three trust levels and the user's
decision. The unofficial warning in `editions.md` ("It can do anything Studio can") stays as written because the
level on a given machine may be `partial`, or `none` on an old kernel. The more specific information is shown
beside the warning: "Sandbox: full" or "Sandbox: partial (no network isolation)".

**The minimum (decision 9).** `min_sandbox` is `partial` unless a policy sets it. A plugin whose level on this
computer is below the minimum is `disabled: sandbox_insufficient` and does not run.

| Plugin | Below the minimum |
|---|---|
| Official | Runs. Its trust is the owner's signature. The level is still shown |
| Third-party signed, key approved | Does not run, unless overridden as below |
| Unsigned, or signed with a key that is not approved | Does not run, unless overridden as below |

**The override** is one user's decision about one plugin: "run this one without isolation". It is asked in its
own dialog, apart from the decision to run the plugin, it is stored for that plugin's hash only, and a changed
plugin asks again. Whether it is offered depends on the install:

- Studio installed for everyone: only when the administrator's policy has `allow_no_sandbox: true`. With no
  policy file it is not offered. A plugin a user installed for themselves does not get free rein on a computer
  someone else administers.
- Studio installed for one user (section 5.2 says how Studio tells): offered, because that user already
  controls everything Studio is, unless a policy file is there and says `allow_no_sandbox: false`.

An administrator can raise the minimum to `full`, and can lower it to `none` for everyone, which is the old
behaviour and is their choice to make.

### 9.3 Resource limits

Applied by the launcher: memory (default 512 MiB), processes (default 8), open files (256), CPU time share (a job
rate on Windows; cgroup `cpu.max` or `RLIMIT_CPU` and a niceness elsewhere), core dumps disabled and no
crash dumps of a plugin written outside its data directory. Limits are in the manifest, bounded by host maximums.

### 9.4 The data directory

`<agent private dir>/plugins/<plugin_id>/data/`, created by the host, mode 0700 / ACL for the container. It is the
plugin's `HOME`. It has a quota (default 64 MiB; the sandbox is not a quota system, so the host checks the size when
the plugin exits and at start and refuses to start above it). It is **not** removed when the plugin is removed unless
the user chooses to ("Also delete its data"). Nothing there is ever read by Studio except the instance state of
section 7.8, which the agent writes itself.

### 9.5 Why a launcher

`agent.md` records that the agent's pseudo-terminal descriptors are not close-on-exec and says: *"The agent must not
start other programs while this holds."* This design needs the agent to start programs. Two ways to resolve that:

1. **A launcher started before any port exists**, like a zygote: `vss_agent` starts `vss_plugin_launcher` at the very
   start, before it makes a port, so the launcher inherits no port descriptors, and talks to it over a channel. All
   later plugin starts go through the launcher. It also is the one place where the sandbox is applied, by the one
   small, reviewable program. **Recommended.**
2. Make every descriptor the agent opens close-on-exec (a change in `virtual_port_posix.cpp` and in the transports).
   This is the better long-term fix and is needed anyway if the agent ever starts anything else, but it changes
   engine code that this design should not touch unasked.

**Decided (decision 4): the launcher.** A test checks that **no descriptor other than the channel reaches a
plugin** (section 16). The launcher is always the one from the same install as the agent that starts it, and is
never looked for in a plugin directory.

The launcher is a separate executable listed in the installers (`agent.md` says each installer holds a fixed list of
programs). It does not link Qt or the engine.

## 10. Failure modes and what the user sees

The exact strings, the Plugins dialog, the decision dialog, the toolbox rule and what a setup shows when its plugin
is not available are in `docs/gui-design/docs/screens/10-plugins.md`, which is the copy to build from. The table
below is the host's side: what it does for each failure. Where its words differ from the screen spec, the screen
spec is right. Plugin-provided text is shown as plain text and quoted, never mixed into a sentence that could be
mistaken for Studio's own.

The GUI groups the reason codes of section 12 into ten states: Available, Starting, Off, Needs your decision,
Blocked by your administrator, Needs a licence, Stopped after failing, Needs a newer Studio, Can't be used and Not
installed. Each has a reason and, where there is one, an action.

| Failure | Host action | What the user sees |
|---|---|---|
| Manifest invalid | `rejected: manifest_invalid` | The plugin is listed greyed, with "Can't be used: the manifest is not valid" and the first error with its position. A **Show details** line |
| Signature bad | `rejected: signature_invalid` | "Can't be used: the signature does not match its files. They may have been changed." No way to run anyway |
| Hash mismatch at start | `rejected: changed` | "This plugin changed since you approved it." Asks again |
| Revoked | `rejected: revoked` | "This plugin was withdrawn by Virtual Serial Studio." Stopped within a second if it was running |
| Expired | `rejected: expired` | "This plugin has expired. Look for an update." |
| Unsigned or third party, no decision | `untrusted` | The trust dialog (section 8.4). Nothing runs until answered |
| Policy forbids | `disabled: policy` | "Set by your administrator." |
| Not licensed | `disabled: not_licensed` | The reason and the renew link of the licence page |
| API or capability missing | `disabled: api_incompatible` / `capability_missing` | "This plugin needs a newer Studio" or "Not available in this build" with the missing name |
| Won't start (no `hello`, exits at once) | Back-off | Status "Starting again in 4 s (attempt 3 of 5)" and the last stderr lines, plain text |
| Crash loop | `quarantined` | "Stopped after 5 failures in 10 minutes." Start button, **Show log** |
| Not responding | Kill, back-off | "Not responding. Restarted." |
| Over a budget | Throttle, then kill | "Plugin X is sending too much (limit: frames per second)." |
| Inline modifier timeout | Fail rule, circuit breaker | On the link: an indicator "Plugin X did not answer in time: traffic is passing unchanged" (fail-open) or "…traffic is being dropped" (fail-closed), with **Reset** and **Disable plugin** |
| A fixture's plugin dead | Engine rules keep answering; anything dynamic stops | "Plugin X stopped. Fixed replies continue." |
| Sandbox weaker than needed | `degraded` or `disabled: sandbox_insufficient` | "Sandbox: partial. No network isolation on this computer." |
| Plugin refuses a parameter | The form keeps the old value | The plugin's reason beside the field |
| A rule set refused | The old set stays | "Rules refused at rule 3: reply is longer than 4096 bytes" |
| Agent starts with plugin not found | `unbound` instances | "Plugin X is not installed. Its settings are kept." |

These states also reach `vss_cli plugin list` with the same reason codes, for rigs with no window.

**An instance on a link or a port is never silent about its plugin.** The Advanced canvas shows a plugin glyph (from
`IconProvider`) on a link or a port with an active instance, with an accessible name, and the Inspector says which
plugin and its state.

## 11. Threading rules

These follow `AGENTS.md` and `architecture.md`, and apply to every line of the host.

1. **The GUI thread never calls into, or waits for, a plugin, a verification, a hash, a sandbox call or the host.**
   Studio sees plugin state as cached views from the agent, updated by batches marshalled with queued connections
   and coalesced, as `agent.md` section 7 describes. Trust prompts show data the agent already computed.
2. **The hub's strands are never blocked by a plugin.** The hub's event sink copies an event into a bounded per-plugin
   queue under a short lock and returns. It does no I/O and never waits.
3. **Sinks capture a `shared_ptr` to the queue, never a stack local.** `unsubscribe` cannot recall a running delivery,
   and a plugin that has gone away must not leave a sink writing into freed memory. Each plugin's sink owns its queue
   by `shared_ptr`, and the queue's `closed` flag makes late deliveries harmless.
4. **One strand for each plugin** carries its channel's reads and writes, its timers (heartbeat, timeouts, back-off)
   and its state. The supervisor has one strand for the set of plugins. Neither runs on a hub strand.
5. **A modifier's hop is asynchronous.** The route stage hands a batch to the plugin's strand and returns; the
   result comes back as a completion on the instance's strand, which gives the bytes back to the stage. A
   deadline timer on the instance's strand enforces `budget_us`. Nothing waits on a future, and no hub strand
   waits for a plugin.
6. **Hashing and verification run on a worker pool of 2 threads**, never on a plugin strand, a hub strand or the GUI
   thread. The result is posted back to the supervisor's strand.
7. **State of a plugin is changed only on its strand.** Other threads read an immutable snapshot (`shared_ptr<const>`),
   as the backend adapter does for ports.
8. **Shutdown is bounded** (section 5.6), and no join waits for a plugin.
9. **Clocks are injected.** The supervisor and the trust code take a clock object, so tests step time and nothing
   sleeps.

## 12. Reason codes

A plugin's state always carries one of these, as `vss::plugin_errc` (an `error_category` named `vss.plugin`),
and `vss_cli plugin list --json` prints them:

`manifest_invalid`, `manifest_too_large`, `too_many`, `shadowed`, `reserved_id`, `layout_invalid`, `file_missing`, `unlisted_file`,
`signature_missing`, `signature_invalid`, `key_unknown`, `changed`, `revoked`, `expired`, `policy`, `not_licensed`,
`api_incompatible`, `capability_missing`, `sandbox_insufficient`, `permission_denied`, `no_hello`, `bad_hello`,
`unresponsive`, `misbehaving` (with the limit's name), `crashed`, `exited`, `start_failed`, `quarantined`.
The manifest reader adds `manifest_unsupported_version`, for a manifest newer than the reader.

## 13. Manifest

### 13.1 Schema

One JSON file, `plugin.json`, UTF-8, at most 256 KiB, depth at most 8. **Unknown keys are errors** (as in the topology
parser), so a misspelling is found. Duplicate keys are errors. Strings have length limits.

```json
{
  "manifest_version": 1,
  "id": "com.example.modbus-fixture",
  "name": "Modbus RTU fixture",
  "version": "1.2.0",
  "author": { "name": "Example Ltd", "url": "https://example.com" },
  "description": "Simulates a Modbus RTU slave.",
  "license": "Apache-2.0",
  "not_after": "2027-12-31",

  "requires": {
    "api": { "min": "1.0", "max": "1.x" },
    "capabilities": ["engine_rules"],
    "edition": "pro",
    "runtime": []
  },

  "run": {
    "executable": "bin/modbus-fixture",
    "args": ["--stdio-channel"],
    "platforms": ["windows-x64", "linux-x64", "macos-arm64"]
  },

  "permissions": [
    { "name": "ports.read", "required": true,  "reason": "Needs to see its port." },
    { "name": "faults",     "required": false, "reason": "To inject CRC errors." }
  ],

  "extension_points": [
    {
      "kind": "fixture",
      "id": "modbus-slave",
      "name": "Modbus slave",
      "timing": { "class": "rules" },
      "max_instances": 8,
      "parameters": [
        { "id": "address", "type": "int", "min": 1, "max": 247, "default": 1, "label": { "en": "Slave address" } },
        { "id": "delay",   "type": "duration", "min": "0us", "max": "1s", "default": "3ms", "label": { "en": "Reply delay" } }
      ]
    }
  ],

  "files": [
    { "path": "bin/modbus-fixture", "size": 123456, "sha512": "<hex>" }
  ]
}
```

| Key | Rule |
|---|---|
| `manifest_version` | Integer. This design is 1. A higher number is `manifest_invalid: unsupported_version`, as the reader cannot know what it means. A change that only adds an optional key keeps the number; any other change raises it |
| `id` | Reverse-domain, lower case, `[a-z0-9.-]`, 3 to 128 characters. Identity for trust and state |
| `version` | Semantic version. Compared only for display and the "newer than trusted" note |
| `not_after` | A date. Optional for third party, required for official |
| `released` | A date. Optional. Used by the licence's version window (`updates_until`, section 8.7) |
| `contributes.bundle_sections` | Optional. Sections the plugin can add to a support bundle (section 19) |
| `requires.api`, `capabilities`, `edition`, `runtime` | Section 7.2, 6.6, 8.7, 8.3 |
| `run` | One executable, a fixed argument list, the platforms it supports. No shell string. A missing platform means the plugin is not offered on this one |
| `permissions` | Section 9.1; unknown names are errors |
| `extension_points` | One or more. `kind` is one of `generator`, `fixture`, `modifier`, `adapter`, and `decoder` (section 20.6; `async` only). `timing` as in 6.4 (`modifier` requires `class` of `inline` or `async`; others default to `async`, `rules` allowed for `generator` and `fixture`) |
| `parameters` | Section 7.8. Ids unique within the extension point. Defaults must satisfy the constraints |
| `files` | Required for signed plugins; ignored for unsigned (the verifier measures) |
| `labels`, `description` | Plain text; per-language maps |

### 13.2 Versioning of the manifest

`manifest_version` is for the *format*. A host reads the versions it knows and refuses the others with
`unsupported_version`; it never guesses. An older plugin with `manifest_version: 1` keeps loading in a host that also
reads 2, because each host version keeps its readers; the policy is to keep the readers for every version shipped.
The plugin API range (`requires.api`) is separate: a plugin may keep an old manifest and require a newer API.

## 14. Code layout

| Library | Content | Depends on |
|---|---|---|
| `libs/vss_plugin` | The manifest parser, the verifier, the trust store, revocation, policy, the lifecycle state machine, the back-off logic, the rule-set model, the parameter model (no I/O, no processes) | `vss_core` (errors), `vss_config` (JSON helpers), Boost.JSON, `vss::signed_file` from `libs/vss_license` |
| `libs/vss_plugin_host` | The supervisor, the channel, process start through the launcher, the budgets, the API message codec and handling, the instance types. Uses `vss_agent_proto` framing | `vss_plugin`, `vss_agent_proto`, `vss_studio` |
| `apps/vss_plugin_launcher` | The sandbox application and the exec. Platform files chosen **in CMake**, no `#if` | none |
| `apps/vss_agent` | Starts the launcher first; owns the host | |
| `apps/vss_gui` | The Plugins page, the trust dialog, the settings form from parameters, the link/port indicators | |
| `tests/…` | In the project's own framework | |

The parameter model and the rule-set model are in `vss_plugin`, so that Studio can render and validate without the
host. Everything uses `vss::result`.

## 15. Threat model

### 15.1 Assets

The user's files and secrets; the integrity of the user's account; the traffic passing through Studio (which may
contain credentials of a device); the topology and ports; the reliability of Studio and of the user's test runs; the
reputation of the owner's signing key; the truth of what Studio tells the user.

### 15.2 Actors

| Actor | Capability |
|---|---|
| Malicious third-party plugin author | Ships a plugin that does harm, possibly signed with a throwaway key |
| Careless plugin author | Ships a plugin that crashes, hangs or floods |
| Supply-chain attacker | Replaces an official plugin's files in a download, or compromises a plugin author's key |
| Malware under the user's account | Writes into the user's plugin directory or trust store |
| Other local users | Cannot reach the agent (`agent.md` section 3), but may be able to write to a world-writable plugin location |
| Network attacker | Not present on the GUI path; present only for adapters that listen |
| Hostile device or program on a port | Sends adversarial bytes, which reach plugins as traffic |
| A user who is tricked | Clicks through a prompt |

**Not defended:** a process of the *same user* that attacks the agent directly is not stopped by this design, as
`agent.md` already states, because the agent is not a boundary inside one account. Kernel and hypervisor exploits.
A compromise of the owner's signing key beyond what a revocation list and rotation can contain. A user who edits their
own trust ledger or system clock (the clock is a deterrent).

### 15.3 Trust boundaries

1. Plugin process | agent (the plugin API channel). Everything from a plugin is untrusted input.
2. Plugin files | verifier (files are untrusted until hashed and signed-checked, then pinned).
3. Agent | Studio (the existing boundary of the agent protocol).
4. Plugin | operating system (the sandbox, enforcement by the kernel).
5. Policy and trust store | user (files with ownership checked).
6. Adapters | the outside world (owned by the remote-control work).

### 15.4 Threats and mitigations

| # | Threat | Mitigation | Residual |
|---|---|---|---|
| T1 | A plugin sends malformed, oversize or deeply nested frames to crash or exhaust the host | The agent's decoder (limits checked before buffering, fuzzed), per-plugin rate and size limits, kill on repeat | A parser bug; the fuzzing and sanitiser legs are the guard |
| T2 | A plugin floods requests or traffic to starve the agent or other plugins | Per-plugin rate limits and back-pressure, bounded queues, a strand for each plugin, the hub never waits | One plugin can use its own share of CPU; the sandbox CPU limit bounds it |
| T3 | A plugin asks for more than it was granted | Every operation checked at call time against the granted set; no ambient authority; proxied operations only | A logic error in a check; every operation has a test that a missing grant is refused |
| T4 | A plugin reads or writes the user's files, or the trust store, or the agent's state | Sandbox filesystem scope; the trust store and agent state are outside any granted path; only the agent writes them | The sandbox is `partial` or `none` on some systems and says so; the user was warned |
| T5 | A plugin exfiltrates traffic or files over the network | No network unless `net.*` granted; network isolation per OS where available; the grant is shown with its hosts | `partial`: where isolation is missing the page says "No network isolation on this computer" |
| T6 | A plugin spawns children that outlive it or escape | Job object, process group, kill on stop; `process.spawn` is a permission; children in the same sandbox | Daemonising Unix children that call `setsid` and escape a process group: killed with a PID namespace or cgroup where present, else with a scan of the tree before stop |
| T7 | A plugin impersonates another (claims a different id or signer) | The id is checked against the verified manifest; the channel is the identity; one channel per process | None known |
| T8 | A plugin is changed after the user approved it | Hash-pinned decisions; files opened and held read-only during the run; a hash through the handle at start | A file the plugin itself loads from outside its directory (an interpreter): disclosed on the page |
| T9 | Time-of-check to time-of-use between verify and exec | Section 5.4: hold handles, hash through them, exec from the handle where possible | On Windows and macOS the exec is by path; the directory is held closed to writers by an ACL and a held handle; a residual window exists where an administrator or the system can change it |
| T10 | A manifest uses path tricks (`..`, drive letters, links, device names, case folding, alternate streams) | The layout rules in 5.3; paths normalised and checked; links not followed | |
| T11 | A manifest string or a log line is used for injection into the GUI (markup, format strings, control characters, bidi override to spoof a name) | All plugin text is plain text, length-limited, with control and bidi characters removed or shown escaped; never a format string; the signer's name comes from the key record, not the plugin | A long name that looks like another: the id and the hash are always shown |
| T12 | A confusing trust prompt leads the user to approve | One decision per hash, permissions in plain sentences, an increase in permissions called out, **no "trust everything"**, no default-enabled button on unofficial plugins, the warning wording as in `editions.md` | A determined click-through is the user's choice |
| T13 | A revoked key or plugin keeps working | A signed revocation list with a rising serial, applied at once; an expiry on every official plugin; rotation | Without a network a user who never updates Studio never gets a new list. Expiry bounds that, and the page says when the last list was installed |
| T14 | The revocation list is rolled back or forged | Signed by an official key under its own purpose; only a higher serial accepted; the highest serial stored | A user who deletes their ledger |
| T15 | The clock is set back to revive an expired plugin | High-water mark | A deterrent only, said so |
| T16 | A compromised or hostile plugin directory (world-writable machine root) | Ownership and ACL checks; a machine root that fails is not searched, reported | |
| T17 | A modifier hangs the link, silently | Budgets, fail-open default, circuit breaker, visible state and counters | A user who selects fail-closed gets a failed link and is told |
| T18 | Crash loop consumes resources or hides the cause | Back-off with jitter, quarantine at five, logs kept | |
| T19 | An adapter becomes a remote-control hole | Capability groups, attribution, declared address; the remote-control work owns the network side | Depends on that work |
| T20 | A plugin response is huge or amplifying (a result larger than the input; a delay beyond bounds) | Result bounds in 7.6; rule limits; delay maximum | |
| T21 | A signing-key compromise lets an attacker sign a malicious "official" plugin | The key is never in the repository or logs; a signing job the owner approves; a rotation path; the revocation list; expiry | A compromise is serious; the response is rotate, revoke, release |
| T22 | A malicious archive when installing (zip slip) | There is no installer in the first version: a plugin is a directory the user puts in place. Any future installer extracts with the layout rules of 5.3 | |
| T23 | A plugin deliberately misreports its own state to the user | The state shown is the host's. Plugin `status` text is labelled as from the plugin | |
| T24 | Privilege escalation through the agent (an elevated agent doing work for a plugin) | The agent refuses to run elevated (`agent.md`); the launcher drops privileges and never runs elevated; no setuid/AppContainer SID broadening beyond the plugin's directories | |
| T25 | A hostile device uses a plugin as a way in: crafted bytes on a port parse badly in a plugin | The plugin is in a sandbox with the user's grants; the bytes cross as opaque blobs; the host never interprets them | Plugin bugs are the author's; the sandbox limits the result |

## 16. Test strategy

Everything uses `vss_unit_tests`, the system test framework in `tests/` and the fuzz setup in `docs/fuzzing.md`. No
other framework. Waits are for a condition with a time limit, never a fixed sleep; time is injected where it matters.

### 16.1 Layers

| Layer | What | How |
|---|---|---|
| Manifest parser | Valid and invalid manifests: every key, every limit, unknown and duplicate keys, bad types, depth and size, path tricks, unicode tricks | Unit tests with a table of cases, each with the exact reason code and position; **fuzz** `fuzz_plugin_manifest` (accept or reject without a crash; accepted text writes and reads back the same) |
| Verifier | Pure function with vectors: good, wrong key, flipped bit in manifest, flipped bit in a file, extra file, missing file, expired, revoked key, revoked hash, rolled-back list, policy cases, downgraded trust level, domain separation (a plugin signature used on a revocation list and the reverse) | Unit tests with in-memory test keys (section 8.8) and a fake clock |
| Recording and bundle sections | The index and the lines of a recording, a section's file list: limits, a higher format number, path tricks, a hash that does not match | Unit tests + **fuzz** `fuzz_recording` |
| Revocation and policy | Parse, serial rules, malformed lists, policy ownership failures (a fake file-status provider), fail-closed behaviour | Unit tests + **fuzz** `fuzz_revocations`, `fuzz_policy` |
| Trust store | Atomic write (a crash between steps leaves the old file or the new one, never half), corrupt file, concurrent writers, hash change asks again | Unit tests with a failing-file-system double |
| State machine and back-off | Every transition, every failure sequence, the window count, the reset after five minutes, the user pressing Start, quarantine | Unit tests with the injected clock; a **property test** that no sequence reaches an undefined state |
| API codec | Every message there and back; every limit; unknown optional fields; unknown op; ids | Unit tests, the agent frame fuzz harness reused for framing; **fuzz** `fuzz_plugin_message` |
| Host with a fake plugin | The host and a fake plugin in one process over an in-memory channel | Unit tests: handshake and failures, permissions, credit, batches, timeouts, circuit breaker |
| Host with real processes | `vss_test_plugin`, a small executable that does what it is told by arguments | System tests, section 16.2 |
| Sandbox | A plugin that tries to read a file outside, connect to a socket, open another process, spawn a program, allocate over its limit, and write outside its data directory | System tests on each platform leg; asserts the attempt **fails**; skipped with a printed reason where the kernel lacks the facility, never silently |
| GUI | The Plugins page, the trust dialog, the form from parameters: accessible names, keyboard, both themes, 200 percent text | The GUI test suite, offscreen, against a fake backend |
| Timing | Rules fire at their times within a tolerance stated per platform, the same seed gives the same sequence, a modifier's p99 latency is measured and reported | System tests; a measurement, not a hard bound, on shared runners |

### 16.2 The hostile plugin: `vss_test_plugin`

One executable with a flag for each behaviour, so each test names the misbehaviour it checks:

`--exit-at=<stage>`, `--crash-at=<stage>` (null write, abort, stack overflow), `--hang-at=<stage>`,
`--no-hello`, `--bad-hello` (wrong id, wrong api), `--hello-twice`, `--slow-reply=<ms>`, `--never-reply`,
`--reply-out-of-order`, `--reply-wrong-id`, `--duplicate-reply`, `--reply-oversize`, `--reply-too-many-actions`,
`--frame-garbage`, `--frame-truncate`, `--frame-oversize`, `--frame-lie-size`, `--deep-json`, `--flood-frames`,
`--flood-bytes`, `--flood-log`, `--flood-stderr`, `--credit-overrun`, `--use-ungranted=<permission>`,
`--claim-other-id`, `--spawn-children`, `--fork-bomb` (against the limit), `--leak-memory`, `--busy-loop`,
`--read-file=<path>`, `--connect=<addr>`, `--write-outside=<path>`, `--ignore-shutdown`, `--ignore-sigterm`,
`--double-fork-escape`, `--list-descriptors` (to check nothing but the channel is inherited), `--stdout-markup`
(control characters and bidi in text), `--params-reject`.

Test expectations are about the **host**: it stays alive, other plugins and ports are unaffected, the right reason
code appears, the right state results, resources are released (no handle, thread or process left), and the time to
recovery is bounded.

### 16.3 Crash loops and long runs

- A crash-loop test with the fake clock: 5 failures in 10 minutes quarantine; a plugin that runs 5 minutes resets; a
  failure at the edge of the window.
- A **soak** test in the nightly leg: 100 plugin starts and kills, an inline modifier under traffic with a plugin that
  crashes every second, a count of handles, threads and memory before and after. TSan and ASan/UBSan at `-O1` (the
  sanitiser standard) run the unit and system tests.
- **Fault injection in the host**: a failing `CreateProcess`/`fork`, a channel that fails mid-frame, a full disk
  during a trust write, a clock that jumps, a hash that is slow. Each is a double behind an interface the host takes.

### 16.4 What cannot be covered, said plainly

- The behaviour of a sandbox on every kernel and Windows build. The CI legs cover the images they run; others report
  their level.
- Two-user and elevation cases (a world-writable plugin directory owned by another user): VM guest tests as in
  `docs/vm-testing.md`.
- The signing job and the real official key. The verifier is tested with test keys; the job is the owner's.
- Real timing on loaded machines.

## 17. Decisions

The owner went through these on 13 November 2026. Each line says what was decided and where it is written.

| | Decision | Outcome |
|---|---|---|
| 1 | Where the host lives | In `vss_agent`, with the library usable in `vss_gui` and `vss_cli` until the agent is in use |
| 2 | The timing proposal (section 6) | Three tiers, rules before round trips |
| 3 | The default for a modifier that does not answer | `fail_open`, with an override for each instance and a policy that can force either |
| 4 | Starting programs from the agent | The launcher, started before any port (section 9.5). **Added in review:** plugins for everyone and plugins for one user are both listed and managed, and Studio installed for one user is to be investigated (section 5.2) |
| 5 | How a modifier sits in the path | **Changed in review:** a route hook in `hub` in the first version, and no inserted endpoints. The owner asked for the change to `vss_core`. It is the one route stage that `docs/response-timing.md` adds (section 7.6) |
| 6 | Signature algorithm and library | Ed25519 with SHA-512 through a vendored Monocypher. X.509 chains were weighed and not taken (section 8.2) |
| 7 | Test keys | Throwaway keys from a fixed seed, marked TEST ONLY, used only by tests |
| 8 | The official keys | A root key kept offline signs a key list; signing keys live in the signing job (section 8.9). Decided "for now": a second kind of signature for third parties can be added later |
| 9 | The sandbox minimum | **Changed in review:** `partial` by default. Running without a sandbox is an override for one plugin, by the user, and on an install for everyone only where the administrator allows it (section 9.2) |
| 10 | Plugin fixtures in the Advanced toolbox | Yes, only while the plugin is installed and on, marked with its name and trust level. A plugin the user switched on that cannot run now keeps its tiles, disabled with the reason; every other unavailable plugin's tiles are hidden |
| 11 | "Run anyway" for an expired plugin | Offered for a plugin that is not official, until Studio closes |
| 12 | In-process plugins | Not in the first version. Left open for later; it needs a licence position from the owner first |
| 13 | Where a setup records its plugins | An optional list on a port or a link in the topology file (section 7.8). The owner asked for this schema change. It is additive and the file's version does not change. No UUID: the manifest id is the identifier |
| 14 | Recording inside the agent | Yes, with `vss::capture`, on a plugin's request, and capture gains nanosecond times |
| 15 | Bundle sections | Yes: `contributes.bundle_sections` and the `bundle.section` permission |
| 16 | Who can open a recording | Anyone can read a bundle and look at its traffic with the public reader. Replaying it is the Pro plugin |
| 17 | Decoders as a fifth kind of extension point | Yes: `kind: decoder`, always `async`, needing `traffic.tap` (section 20.6) |
| 18 | The decode stream in the agent | Yes (section 20.4), with the `bus_fault` value, link and time added to the agent protocol's event record |
| 19 | Triggers | Yes (section 20.7): the agent's own record, not a `vss_core` event kind. Engine actions on a trigger are off unless the user turns them on |
| 20 | The protocol description format | Public, in this repository, with a conformance corpus and `vss_cli protocol check` |
| 21 | Community and Pro for decoding | Community keeps bytes, text and times and gains a basic framing view. Descriptions, decoders, anomaly detection, triggers and harness delivery are Pro. The formats are public in both |

**Still open:**

1. **From the owner:** signing the first official key list with the root private key. The public keys are in
   (section 8.9); the command is in `libs/vss_plugin/keys/README.md`.
2. **How long an official plugin's `not_after` runs.** The owner's first thought was 28 days; this page
   recommends the signing key's `valid_to` and gives the reasons (section 8.5). The first key list gives the
   signing keys two years, 14 November 2026 to 14 November 2028. To be confirmed.
3. **How a user gets an unknown author's public key** in order to approve it (section 8.9).

**Closed since the review:**

- **Studio installed for one user**: investigated (section 5.2). No installer gets a second mode; Studio tells
  the kind of install from its folder; the agent's `hello` has to say which copy it is.
- **Licence files and the key list**: the owner decided that licence files do not use it (section 8.9).
- **The order of a rule set and modifiers on one route stage**: the rule set first, agreed by the owner and
  written in both pages (section 7.6). The stage itself is not built yet; it is slice 4 of
  `docs/response-timing.md`.
- **The screens for decoding and triggers**: written, `screens/11-decoding-and-triggers.md` (section 20.9).

## 18. Order of work after review

Small vertical slices, each with its tests, each a pull request:

1. **`vss_plugin`: the manifest parser and the verification library** (parse, verify, hash, revocation, policy,
   trust store, reason codes), with unit tests and the fuzz harnesses. No processes. This is the security
   foundation and is written by the author of this design.
2. **The supervisor**: the state machine, back-off with an injected clock, the channel and the API codec with a fake
   plugin in memory, then real processes through `vss_test_plugin` without a sandbox, then the launcher and one
   platform's sandbox at a time.
3. **The first extension point: generators.** They have the simplest contract (credit flow, one direction, no byte
   path), and they exercise the instance lifecycle, parameters, permissions, the Plugins page and the failure
   states. The replay half of the worked example of section 19 is a generator, so it is the first real plugin to
   run against the API. Fixtures with engine rules follow, then modifiers (on the route stage, so after the slice of
   `docs/response-timing.md` that adds it), then adapters. The decode stream,
   decoders and triggers of section 20 come after the capture in the agent, which they share a path with, and
   after the event times of `docs/response-timing.md`.

Slice 1 takes its signature check from `vss::signed_file` (section 8.2). If the licensing work has not merged that
part, slice 1 is the manifest parser, the layout checks, the hashing, the trust store and the policy, and the
signature check is its own small pull request afterwards.

Simple, well specified parts of these slices (the parameter model and its form, the `vss_test_plugin` flags, table
tests of the parser) can be handed to subagents. The verifier, the supervisor, the protocol, the concurrency and the
sandbox stay with the author of this design.

## 19. Worked example: the Diagnostics and replay plugin

The owner chose this as the first plugin to frame the API with. It is an official Pro plugin. This section says
what it does, derives what the API must offer, and lists what the earlier sections lacked. It designs the plugin
only as far as the API needs.

### 19.1 What it does

1. **Record.** A Pro user with a problem records it in their own Studio: the bytes in and out at every port and
   link they choose, every line change and fault, on one clock, with the setup and their own notes.
2. **Pack.** The recording goes into one bundle file, after the user has chosen what goes in, seen it and removed
   what they do not want to share.
3. **Replay.** Another Studio opens the bundle and plays the same traffic with the same timing, or scaled, so that
   support sees what the user saw.
4. **Test.** The same bundle is test data: a harness replays it into a setup and checks the results.

**Consent.** The user chooses what is recorded and what is packed, sees a preview of exactly what the file will
hold, can redact, and sends the file themselves. Nothing is sent by Studio or by the plugin. The plugin asks for no
network permission, so on a computer with network isolation it cannot send anything even if it tried. Support uses
only what is in the file the user sent.

### 19.2 The boundary with the support bundle

`docs/licensing.md` and `screens/09-licence-and-support.md` own the **support bundle**: one zip with a
`manifest.json` of sections (version, driver state, setup, log, counters, serial ports, licence), a checklist, a
preview built by the function that writes the file, and **never any traffic**. It is in the Community application.
That work reserved the section id `traffic` and the folder `traffic/` for this plugin.

So there is one bundle and one dialog, and the split is:

| | Support bundle (Studio, Community) | Diagnostics and replay (plugin, Pro) |
|---|---|---|
| Version, driver and engine state, setup, log, counters, real serial port list | Yes, its own sections | Not repeated. A replay reads them from the same bundle |
| Bytes on the wire, line changes and faults with times | Never | The `traffic` section |
| The user's notes | No | The `notes` section |
| The dialog, the checklist, the preview, the save | Studio's | Adds rows to it; draws nothing |
| Redaction of names, paths and secrets in Studio's files | Studio's | Not involved |
| Redaction of traffic | No traffic to redact | By port, by time range and by byte pattern, in the plugin's section settings |
| Opening a bundle to replay it | No | Yes |

A bundle made without the plugin is a plain support bundle. A bundle made with it is the same file with two more
sections, and a reader that does not know them ignores them, as the bundle format already says.

### 19.3 What stays in the engine

The plugin decides **what** to record and replay. The engine and the agent do the recording and the replaying.

| Stays in the engine or the agent | Why |
|---|---|
| **The recording itself.** The agent writes it with `vss::capture`, to a file in the plugin's data directory, on the plugin's request | A copy sent to a plugin through a queue can drop under load (`agent.md`, section 4), and a recording with holes cannot be replayed. `agent.md` already plans capture inside the agent for this reason |
| **Every timestamp**, on the run's one clock | A plugin's clock is not the engine's |
| **The replay schedule.** A recording is a timeline that the engine's rule scheduler plays (`docs/response-timing.md`, "Replaying a recording"): never early, with reported lateness, with `time_scale` | A plugin that writes bytes at recorded times would add its own scheduling error to every one |
| **Determinism**: the seed, the rule sets that were installed, the hand-moved clock for exact repeats | The same reason, and it keeps a replay comparable with its recording item by item (`source_ns`) |
| **The preview and the write of the bundle** | Studio's function writes what it showed. The plugin hands over files; it does not write the zip |
| **Limits**: recording size and length, disk space | The agent enforces them even if the plugin does not |

### 19.4 What the API must offer

| Need | API | Kind | Permission | In this design |
|---|---|---|---|---|
| Know the ports, links and their state | `agent.call` (`list`, `snapshot`) | Proxy call | `ports.read` | Section 7.7, written for adapters only. **Change:** the read operations of the proxy are open to every plugin kind that holds the permission |
| Follow events with times: opens, closes, line changes, faults, rule firings, counters | `agent.subscribe` without `data` | Proxy call | `ports.read` | The same change. Read-only: this plugin never holds `topology.modify` while recording |
| A live view of the bytes while recording, so the user sees that it works | `agent.subscribe` with `data` (a tap; it may drop, and says how much) | Proxy call | `traffic.tap` | Section 7.7 |
| **A complete recording** of chosen ports and links | `capture.start {places, limit_bytes, limit_seconds, ring}`, `capture.stop`, `capture.status`; the agent writes the file | **New** engine-side operation | `traffic.tap` | **New.** Decision 14. `ring` keeps only the last N minutes, so a user can leave it on and pack what just happened |
| The setup, the driver and engine state, the real serial ports | None: they are the support bundle's own sections | | | The plugin does not need `serial.devices` |
| Add its sections to the bundle, with a preview | `contributes.bundle_sections` in the manifest; `bundle.prepare {section, settings}` from the host, answered with a list of files in the data directory | **New** contribution | `bundle.section` | **New.** Decision 15. Section 19.5 |
| Redaction choices | The section's declared parameters, drawn by Studio's form in the bundle dialog | Parameters | none | Section 7.8, reused |
| Open a bundle the user chose | A `path` parameter, read access to that one file | Parameter | `fs.read` | Sections 7.8 and 9.1 |
| Rebuild the setup for a replay | `agent.call apply`, after Studio has asked the user | Proxy call | `topology.modify` (optional) | Section 7.7. Without it the user opens the setup themselves |
| **Replay onto a port** with original or scaled timing | Generator type "Replay a recording": `timeline.load {recording, place, time_scale, seed}`; the engine plays it | **Generator** (extension point), timing class `rules` | `ports.write` | `generator.pattern` of section 7.4 was the nearest thing. **Change:** a generator may hand the engine a timeline, as `docs/response-timing.md` defines it |
| **Stand in for a device** that is not on the replaying computer | Fixture type "Recorded device": the recorded bytes of that port as a `station` timeline | **Fixture type** (extension point), timing class `rules` | `ports.write` | Section 7.5 with the same change. It appears in the Advanced toolbox with the plugin mark |
| Repeat exactly | The seed and rule sets kept in the recording; `time_scale`; the hand-moved clock | Engine | none | `docs/response-timing.md` |
| Tell a harness what happened | Rule and data events with `when`, `source_ns` and the item's index | Proxy events | `ports.read` | For tests the harness adapter reads them; this plugin shows a summary |
| Stay inside limits | Recording limits; a data directory quota the manifest asks for and the user sees | Host | none | **Change** to section 9.4: the quota is declared (`limits.data_bytes`), shown in the decision dialog when it is over 64 MiB, and capped by the host |

**Permissions of this plugin**, as the user sees them in the decision dialog:

| Sentence | Needed | Note |
|---|---|---|
| "See your ports and links, their state and their counters." | Yes | |
| **"Read the data that passes through the ports and links you use it on."** | Yes | The read-traffic permission. An amber row, never granted silently, not even to an official plugin |
| "Add a section to a support bundle. You see it and choose whether to include it." | Yes | |
| "Read files in a folder you choose." | Yes | To open a bundle |
| "Send data from the ports you use it on." | Yes | To replay |
| "Add, remove and connect ports and links." | Optional | To rebuild the setup of a bundle |

It asks for no network permission, no `faults` and no `modifier.inline`.

`traffic.tap` is scoped: it covers the places the user picked for this recording, and the host refuses a
`capture.start` or a tap for any other. While a recording runs, Studio shows it: a "Recording" chip in the status
bar that names the plugin and opens it, so traffic is never read without a sign on the screen.

### 19.5 Bundle sections

A plugin declares the sections it can add:

```json
"contributes": {
  "bundle_sections": [
    { "id": "traffic", "title": { "en": "Recorded traffic" }, "sensitive": true,
      "parameters": [ { "id": "ports", "type": "port" }, { "id": "blank", "type": "bytes" } ] },
    { "id": "notes", "title": { "en": "Your notes" }, "sensitive": false,
      "parameters": [ { "id": "text", "type": "string", "max_length": 8000 } ] }
  ]
}
```

- Studio's bundle dialog shows each as a row, marked "Added by <plugin name>". A `sensitive` section is an amber
  row and is **off by default**, every time.
- When the user ticks a row or changes its settings, the host sends `bundle.prepare`. The plugin writes the
  section's files into a new folder of its data directory and answers with their names and sizes. Studio reads
  them, shows them in its preview and, on Save, writes exactly those bytes under `<section id>/` in the zip. What
  was previewed is what is saved, because Studio does both from the same files.
- The plugin never learns where the bundle was saved, and is not told whether it was.
- Limits: 16 files and 256 MiB for a section (the host's maximum), a `bundle.prepare` answer within 30 s with
  progress and cancel, and section ids that are the plugin's own or reserved for it (`traffic` and `notes` for
  this plugin; a third-party plugin's ids are prefixed with its plugin id).
- A plugin that fails or is stopped during `bundle.prepare` loses its rows for that bundle, with the reason shown.
  The rest of the bundle is unaffected.

The strings of these rows belong to the bundle dialog, which `screens/09-licence-and-support.md` owns; that page
already has "Added by <plugin name>".

### 19.6 The `traffic` section: an open format

Third parties must be able to read a bundle, so the format is documented in this repository and uses formats the
project already has.

```
traffic/recording.json     the index
traffic/<n>.jsonl          one file for each recorded place: JSON Lines, a capture format vss::capture already writes
notes/notes.txt            UTF-8 text
```

`recording.json`:

```json
{
  "recording_format": 1,
  "origin_utc": "2026-11-13T09:41:07Z",
  "time_unit": "ns",
  "duration_ns": 184000000000,
  "seed": 7,
  "places": [ { "file": "1.jsonl", "kind": "port", "name": "COM7", "settings": "19200 8N1",
                "bytes": 48213, "sha256": "<hex>" } ],
  "rule_sets": [],
  "redactions": [ { "place": "COM7", "from_ns": 1200000000, "to_ns": 1900000000, "how": "blanked" } ],
  "dropped": 0,
  "made_by": { "plugin": "com.virtualserialstudio.diagnostics", "version": "1.0.0" }
}
```

- **Versioned.** `recording_format` rises only for a change an old reader cannot ignore, the same rule as the
  bundle's `bundle_format`. A reader refuses a higher number and says so.
- **Times** are whole nanoseconds since the recording's origin, on the recording Studio's one clock. The capture
  formats of today hold milliseconds, so capture gains a version with nanoseconds (part of decision 14, and a
  change in `vss_studio`, not in `vss_core`).
- **`rule_sets`** holds the text and generation of each rule set that was installed, so the recording alone is
  enough to run it again.
- **Integrity.** Each file's SHA-256 is in the index, so a damaged or edited file is found. That is a check against
  accidents, not a proof of origin.
- **Optional signature.** A lab that wants to show a recording is theirs can sign the index as a
  `vss::signed_file` envelope with the purpose `recording`, with its own key. Studio shows "Signed by <name>" only
  for a key the user has approved, and never treats an unsigned recording as worse than "not signed".
- **Redaction is honest.** A redacted span keeps its length and its times and has its bytes replaced with a filler,
  so the timing still replays; the index lists every span. A place the user left out has no file and no entry.
  `dropped` says how many bytes the recording lost, which should be zero.
- **Sizes.** A recording stops at its limit and says so. Defaults: 64 MiB or 10 minutes; ring mode keeps the last
  10 minutes. The maximum is the section limit above.

A replay never trusts a bundle. It is a file from someone else: the index and every line are parsed with limits,
paths inside the zip follow the layout rules of section 5.3, and the reader has a fuzz harness (section 16).

### 19.7 What this example changed in the design

| Gap | Where it is now |
|---|---|
| The proxy's read operations were for adapters only | Open to any plugin kind with `ports.read` (19.4) |
| No way to record without loss | `capture.start` in the agent (decision 14) |
| Nothing let a plugin add to the support bundle | `contributes.bundle_sections` and `bundle.section` (decision 15) |
| A generator or fixture could only repeat a fixed pattern | It can hand the engine a timeline, defined by `docs/response-timing.md` |
| The data directory quota was fixed at 64 MiB | Declared, shown and capped |
| A setup did not say which plugin its parts came from | Section 7.8 and decision 13: a replayed setup with a "Recorded device" must open on a computer without the plugin |
| Who may open a recording | Decision 16 |

The four extension points were otherwise enough: the plugin is a generator, a fixture type and two bundle sections,
with read-only proxy calls. It needs no behaviour modifier and is never in the path of the data.

## 20. Worked example: the Protocol decoding and triggers plugin

The owner chose this as the second plugin to frame the API with. It is an official Pro plugin for people who use
Studio as automated test equipment. As in section 19, this section says what the plugin does, derives what the API
must offer, and lists what the earlier sections lacked.

### 20.1 What it does

1. **Describe.** The user writes down their serial protocol as data: how a frame starts and ends, where its length
   is, what its fields are, which message types exist and how the check value is worked out (section 20.5).
2. **Decode.** The plugin reads the traffic of a link or a port and turns the bytes into frames and fields. It only
   reads. Nothing is held, changed or delayed, so the devices talk as they always do and run their own recovery.
3. **Detect.** The plugin compares what it decoded with the description and reports what does not fit:

   | Anomaly | Found by | From what |
   |---|---|---|
   | Framing error | Plugin | A start with no end, a frame over its longest length, bytes between frames. Also a line error (parity, framing, overrun, break) that the engine reported at that time |
   | Unknown message type, unknown field value | Plugin | A type the description does not list; a value outside a field's listed values or range |
   | Check value mismatch | Plugin | The check value worked out over the described range against the one in the frame. Reported with the field, the range covered, and the expected and actual values |
   | Length mismatch | Plugin | The length field against the bytes that arrived before the end marker, the gap or the next start |
   | Timeout, missing reply | Plugin, on the engine's time | A request whose described reply did not start within its time (section 20.3) |
   | Out of sequence | Plugin | A reply with no request, a second request before a reply, a sequence counter that skipped or repeated |
   | Bus contention | **Engine** | The half-duplex bus's `bus_fault` event: `overlap`, `too_soon`, `line_held`, `transmitter_off`, `overrun`. The plugin adds the frames that were on the wire |

4. **Trigger.** Each detection becomes a trigger: one event with a time on the run's one clock and the decoded
   context. What a trigger then does is the user's choice (section 20.8): it is logged, it reaches the test
   harness, it can keep a recording of the traffic around it, and it can make the engine do something.

A check value mismatch says which field holds the check value and what it should have been. It cannot say which
data byte is wrong, because a check value does not say that. When a `bus_fault` covers the same bytes, the trigger
says so (`cause`), and that is the only case where the damaged bytes are known.

### 20.2 Community and Pro

Proposed split. The Community user is the hobbyist who is working out what a device says. The Pro user tests a
device against a protocol they already know.

| | Community (Studio itself) | Pro (this plugin) |
|---|---|---|
| Bytes as hex and as text, direction, timestamps, search | Yes, as today | The same view |
| The engine's own events in the log: line changes, line errors, `bus_fault` | Yes, as today | Also joined to the frames they touched |
| **A basic framing view**: the traffic view starts a new row at a silence the user sets, at a byte the user sets (for example `0A`), or every n bytes | Yes. It is a way of showing bytes. It marks nothing as right or wrong | The description's framing replaces it for a place that has one |
| Protocol descriptions: fields, message types, check values | No | Yes |
| Decoders for protocols a description cannot express | No | Yes; Modbus RTU, Modbus ASCII and NMEA 0183 come with the plugin |
| Anomaly detection | No | Yes |
| Triggers, delivery to a harness, a recording kept on a trigger, an engine action on a trigger | No | Yes |
| The description format, the corpus format and the trigger format | Public and documented | The same formats |

The formats are public in both editions, so that anyone can write a description, share it and build tools on it.
What is Pro is the program that decodes with them inside Studio. The basic framing view is drawn by Studio and
needs no plugin, no permission and no engine change beyond the chunk times of section 20.3.

### 20.3 What runs in the engine, and what in the plugin

The rule is: **what needs the clock is the engine's; what needs only the bytes is the plugin's.**

| Part | Where | Why |
|---|---|---|
| The time of every chunk, line change and fault, on the run's one clock | Engine | `docs/response-timing.md`, "One clock": the read time is taken once, before routing. A plugin's clock is not the engine's |
| First and last byte times of a chunk on a half-duplex bus | Engine | Only the bus has a line rate, so only it has byte times (`bus_line`) |
| Contention | Engine | The bus already decides it and publishes `bus_fault`. The plugin never works out an overlap from timestamps |
| Keeping chunks apart and in order, and saying when some were lost | Agent | Section 20.4, the decode stream |
| Splitting the bytes into frames by markers, a length field or a delimiter | Plugin | It needs only the bytes |
| **Splitting by a time gap, to observe** | Plugin, from the engine's chunk times | Each chunk arrives with its time and is never joined to another, so the silence between two chunks is a subtraction of two engine times. The plugin's own speed does not enter into it |
| **Splitting by a time gap, to act within the turn-around** | Engine, as a rule | A reaction that must come inside a frame gap cannot wait for a plugin. It is a rule with `gap_us` (`docs/response-timing.md`, "What a rule matches"), which the engine matches and times. Section 20.8 |
| Fields, message types, check values, lengths, sequence | Plugin | Bytes only, and the list of check values a protocol may need is longer than the engine's closed list |
| Deciding a timeout | Plugin, on engine time | The decode stream carries the engine's time forward when nothing happens (a `time` item at least every 10 ms while a decoder waits for a reply). The trigger's `when_ns` is the request's end plus the allowed time, which is exact. It is published up to one `time` item later |
| Stamping, limiting and storing triggers; delivering them | Agent | Section 20.7 |
| Keeping the traffic around a trigger | Agent | The capture ring of section 19 |
| Any action on the traffic | Engine, by the existing operations and rules | Section 20.8 |

Said plainly, two limits:

- **A gap the program did not make cannot be seen.** A virtual port hands over a whole write at once. Two frames
  that a program wrote in one write have no silence between them, whatever the baud rate says, and on a real UART
  they would have had none either. Gap framing on a virtual port shows the program's writes; on a half-duplex bus
  with a line rate it shows wire times.
- **Detection is after the fact.** A trigger is published after the bytes have been delivered, later by the decode
  stream's delay and the plugin's own work: milliseconds, not microseconds. Its `when_ns` is still the engine's
  time of the bytes it is about.

So no new framing code is asked of the engine for this plugin. It needs the chunk times that
`docs/response-timing.md` already plans, and the decode stream below.

### 20.4 What the API must offer

| Need | API | Kind | Permission | In this design |
|---|---|---|---|---|
| **Read the traffic of chosen places, in order, with times, without ever slowing it** | The decode stream: `decoder.data` items (below) | **New** stream from the agent | `traffic.tap` | **New.** The tap of section 7.7 may drop without saying where and joins chunks; a decoder would report anomalies that are not there |
| Line changes, line errors, `bus_fault`, rule firings, in the same order as the bytes | Items of the same stream | The same | `traffic.tap` (it already implies the events of its places) | **New** |
| The engine's time while nothing happens | `time` items | The same | none | **New** |
| A protocol description the user chose | A `path` parameter, read access to that one file | Parameter | `fs.read` | Sections 7.8 and 9.1 |
| **Decoders that are code**, from this plugin or another | Extension point kind `decoder` (section 20.6) | **New** extension point | `traffic.tap` | **New.** Decision 17 |
| Show decoded frames in the traffic view | `decoder.frame`: a summary and fields, drawn by Studio | Plugin message | none beyond the above | **New.** The plugin draws nothing |
| **Publish a trigger** | `trigger.publish` (section 20.7) | Plugin message | none beyond the above: a decoder may always report | **New.** Decision 19 |
| **Deliver triggers to a harness** | Triggers are records in the agent's event stream; `triggers.query` and `trigger_stats` for a harness that asks afterwards | Agent operations, through the proxy for an adapter | `ports.read` for the trigger, `traffic.tap` to see its bytes | **New** |
| Keep the traffic around a trigger | `capture.start {ring, snapshot_on, before_s, after_s}`: the agent cuts the ring itself when a trigger passes | Engine-side operation | `traffic.tap` | **Change** to section 19.4: `snapshot_on`. No plugin talks to another plugin |
| Act on a trigger | The existing operations: `fault`, `lines`, a write, `rules.set`, a slot | Proxy calls, rules | `faults`, `ports.write` (both optional) | Sections 7.5 and 7.7. Section 20.8 |
| React inside the turn-around | A rule set made from the description | Rules | `faults` or `ports.write` | `docs/response-timing.md`. Section 20.8 |
| Add the triggers of a run to a support bundle | A bundle section `triggers` | Bundle section | `bundle.section` | Section 19.5, reused |

**The decode stream.** It is the tap made exact. For each place the decoder was given, the agent sends items in the
order the engine saw them:

| Item | Carries |
|---|---|
| `data` | `place`, `direction`, `offset` (the count of bytes at that place and direction before this chunk), `t_ns`, `end_ns` (on a half-duplex bus: when the last byte left the wire), the bytes. One engine chunk is one item; chunks are never joined |
| `line` | `place`, `t_ns`, the lines and which changed |
| `line_error` | `place`, `t_ns`, the kind |
| `bus_fault` | `link`, the sender, `t_ns`, the `bus_fault` value, the `offset` and length of the sender's chunk |
| `rule` | A rule fired at the place: its event as `docs/response-timing.md` defines it |
| `time` | `t_ns`: the engine's time now |
| `lost` | `place`, `direction`, `from_offset`, `bytes`, `from_ns`, `to_ns`: the decoder was too slow and this much was not sent |

- **It never slows the traffic.** The agent keeps what the plugin has not yet taken in a queue of fixed size (4 MiB
  by default for an instance). When it is full, the oldest items are dropped and one `lost` item takes their place.
  There is no back-pressure on a port.
- **Loss is in the stream, at the place it happened.** A decoder that reads a `lost` item forgets the frame it was
  in, starts again at the next frame start, and publishes a `decoder_lost` trigger. It must not report a framing,
  length or timeout anomaly across a loss. A harness can therefore tell a clean run from one where detection had a
  hole, as a run whose harness died is marked incomplete.
- **Offsets tie everything together.** A frame is `place`, `direction`, `offset` and `length`, so a trigger points
  at exact bytes of a recording made by the capture of section 19, without matching by content.
- **Flow.** The plugin acknowledges items (`decoder.ack {seq}`), and the host sends at most 256 KiB that is not
  acknowledged.
- The traffic chip of `screens/10-plugins.md` ("Reading data") shows for as long as a decoder reads.

**Two gaps in what exists today**, to be closed by the agent work and not by `vss_core`:

- The agent protocol's `event_record` has the kind `bus_fault` and the engine's sentence, but not the `fault` value,
  the link or a time. The decode stream needs all three. This is an addition to `vss_agent_proto`; `vss_core`'s
  event already has them. Until the agent carries them the capability `events_bus_fault_detail` is absent, and a
  contention trigger then has the kind `contention`, the engine's sentence and no sub-kind. It says "detail not
  available in this build" and is never guessed from the bytes.
- Events have no time in the agent protocol at all. `docs/response-timing.md` ("One clock") gives every event a
  time since the run's origin; the decode stream depends on that and adds nothing of its own.

### 20.5 The protocol description: an open format

A description is one JSON file. It is data and not code, so it needs no signature and no sandbox to share. It is
still a file from someone else: it is read with limits, unknown keys are errors, and its reader has a fuzz harness
(section 16).

```json
{
  "protocol_format": 1,
  "id": "com.example.meter-bus",
  "name": "Meter bus",
  "version": "1.2.0",
  "byte_order": "big",
  "framing": {
    "start": "02", "end": "03",
    "length": { "at": 2, "size": 1, "counts": "payload", "add": 0 },
    "gap_us": 1750, "max_length": 260
  },
  "check": { "algorithm": "crc16", "poly": "8005", "init": "FFFF", "reflect": true, "xor_out": "0000",
             "covers": [1, -4], "at": -3, "byte_order": "little" },
  "header": [
    { "name": "address", "type": "u8", "range": [1, 247] },
    { "name": "length", "type": "u8" },
    { "name": "type", "type": "u8" },
    { "name": "counter", "type": "u8" }
  ],
  "type_field": "type",
  "sequence": { "field": "counter", "step": 1, "wraps_after": 255, "per": "address" },
  "messages": [
    { "name": "read request", "type": 3, "from": "controller",
      "fields": [ { "name": "register", "type": "u16" }, { "name": "count", "type": "u16", "range": [1, 125] } ],
      "reply": { "one_of": ["read reply", "error reply"], "within_us": 100000, "same": ["address", "counter"] } },
    { "name": "read reply", "type": 3, "from": "device",
      "fields": [ { "name": "size", "type": "u8" }, { "name": "values", "type": "bytes", "length": "size" } ] },
    { "name": "error reply", "type": 131, "from": "device",
      "fields": [ { "name": "code", "type": "u8",
                    "values": { "1": "bad function", "2": "bad address", "3": "bad value" }, "strict": true } ] }
  ],
  "unknown_type": "anomaly"
}
```

| Part | What it can say |
|---|---|
| `framing` | Any of: a start marker, an end marker, an escape byte for markers inside the data, a length field (where, how wide, what it counts, a constant to add), a delimiter, a fixed length, a silence (`gap_us`) before a frame or after it. `max_length` is required |
| `check` | A named value (`crc16_modbus`, `crc16_ccitt`, `sum8`, `xor8`, `lrc`: the names `docs/response-timing.md` uses, with the same results) or a CRC by its parameters (width 8, 16 or 32, polynomial, start value, reflection, final xor); the range it covers; where it is; its byte order. For a text protocol: the value as two or four hex characters |
| Fields | `u8` to `u64`, `i8` to `i64`, `f32`, `f64`, `bcd`, `ascii`, `bytes`, and `bits` (named parts of one integer). A length is a number, another field, or `rest`. A field may list `values` with names, a `range`, and `strict` (a value outside them is an anomaly) |
| `messages` | A name, how it is recognised (the type field's value, and who sends it where the protocol has a controller), its fields, and the reply it expects: which messages, within how long, and which fields must be the same |
| `sequence` | A counter field, its step and where it wraps, counted for each address or for the whole place |
| `unknown_type` | `anomaly` or `ignore` |

What it cannot say, on purpose: loops, arithmetic, conditions on field values beyond the message type, or state
beyond request and reply and one counter. The reader builds tables from it, and decoding a frame takes a number of
steps fixed by the frame's length. A protocol that needs more is a decoder in code (section 20.6). Limits: 256 KiB
of JSON, depth 16, 256 messages, 64 fields in a message, a frame of at most 64 KiB.

- **Versioned.** `protocol_format` rises only for a change an old reader cannot ignore. A reader refuses a higher
  number and says so. `version` is the author's own, for people.
- **A test corpus travels with it.** Beside `protocol.json`, a folder `corpus/` holds cases. A case is a recording
  in the format of section 19.6 and an `expected.jsonl`: the frames and the triggers a correct reader produces from
  it, one JSON object on a line, in order. `vss_cli protocol check <folder>` runs every case and prints the first
  difference. So a description can be reviewed, shared and changed with evidence, and a second implementation of
  the format can be held to the first.
- **The format has its own corpus**, in this repository: descriptions that must be refused with a given reason,
  and cases for each framing kind, each field type and each check value. It is the conformance test for any reader.
- **Decoding a recording is the same as decoding live.** A decoder is a function of its stream, and the stream of a
  recording is the recording. The corpus check, a harness that looks at a finished run, and support reading a
  bundle all use that. Nothing is opened and no port exists.

### 20.6 Decoders in code

Some protocols do not fit a description: Modbus RTU (frames told apart only by silence and by what the function
code implies), Modbus ASCII and NMEA 0183 (text with a check value in hex and optional parts), and many in-house
binary protocols. For them a plugin contributes a **decoder**, a fifth kind of extension point.

```json
{ "kind": "decoder", "id": "modbus-rtu", "name": { "en": "Modbus RTU" },
  "timing": { "class": "async" },
  "parameters": [ { "id": "silence", "type": "duration", "default": "1750us" } ] }
```

| Message | From | Content |
|---|---|---|
| `decoder.create` | host | `instance`, the places and their settings (rate, format, whether a bus), `params` |
| `decoder.data` | host | `seq` and a list of the items of section 20.4, with the bytes as one blob |
| `decoder.ack` | plugin | `seq` |
| `decoder.frame` | plugin | `place`, `direction`, `offset`, `length`, `t_start_ns`, `t_end_ns`, a message name, a summary of at most 200 characters, and up to 64 fields as `{name, offset, length, value}`. Optional, rate limited, and dropped first when the plugin is over its limits |
| `trigger.publish` | plugin | Section 20.7 |
| `decoder.stop` | host | |

Rules:

- A decoder's timing class is `async` and nothing else. It is never in the path of the data and the engine never
  waits for it.
- It needs `traffic.tap`, on the places the user gave it and no others.
- A decoder that crashes or is stopped costs the decoding and nothing else: the traffic is untouched, the traffic
  view falls back to bytes, a `decoder_lost` trigger is published by the host, and the instance shows its state.
- **What a decoder says about traffic is untrusted twice**: the plugin wrote it, and the device under test chose
  the bytes. Names and summaries are plain text with the manifest's text rules (no control characters, no direction
  marks, a length limit). Studio draws field values from their bytes where the type is known. Nothing is markup.
- The declarative reader of section 20.5 is itself a decoder of the official plugin, with one parameter: the
  description file. So descriptions and coded decoders produce the same frames and the same triggers, and the rest
  of Studio does not know which one is running.

### 20.7 The trigger

A trigger is one record. The format is public and versioned with the plugin API.

```json
{
  "id": 4127,
  "kind": "check",
  "when_ns": 18204113250,
  "place": "bus-1", "direction": "rx", "sender": "meter-17",
  "protocol": { "id": "com.example.meter-bus", "version": "1.2.0" },
  "frame": { "offset": 48213, "length": 10, "t_start_ns": 18203200000, "t_end_ns": 18204113250,
             "message": "read reply" },
  "field": { "name": "check", "offset": 7, "length": 2 },
  "expected": "3A91", "actual": "3A90",
  "covers": [1, 6],
  "cause": { "bus_fault": "overlap", "event": 9912 },
  "request": { "offset": 48205, "length": 8, "t_end_ns": 18201000000 },
  "text": "Check value is 3A90, expected 3A91.",
  "from": { "plugin": "com.virtualserialstudio.protocol", "instance": 3 }
}
```

- `kind` is from a closed list: `framing`, `unknown_type`, `unknown_value`, `check`, `length`, `timeout`,
  `sequence`, `contention`, `line_error`, `decoder_lost`, and `custom` with a `code` the manifest declared.
- **The host stamps it.** `id` (a count for the run), `from`, and the time it was published are the agent's. A
  `when_ns` later than the engine's time now, or a frame at an offset the place has not reached, is refused. A
  plugin cannot publish a trigger for a place it was not given.
- **The bytes are not in it.** A trigger names its frame by offset. A subscriber with `traffic.tap` may ask for the
  bytes (`triggers.query` with `bytes`), up to 4 KiB a frame, for as long as the agent's ring has them; a
  subscriber with only `ports.read` gets the record without `expected`, `actual` or bytes. So "which tests failed"
  can be read by a tool that may not read traffic.
- **Limits.** 200 triggers a second for an instance, as a budget that refills. Over it the agent counts them
  (`suppressed`) and publishes one `trigger_storm` record a second. The agent keeps the last 10 000 triggers of a
  run. `trigger_stats` gives the count for each kind and place at any time, exact whatever was dropped, as
  `rule_stats` does for rules.
- **They are the agent's records, not `vss_core` events.** A trigger is made outside the hub, so it travels in the
  agent protocol's event stream as its own record and `vss::event_kind` does not change.

### 20.8 What a trigger does

The user or the harness chooses, for each kind of trigger. The plugin does not choose for them.

| | What happens | Who does it | Timing |
|---|---|---|---|
| **(a) Log** | Always. The trigger is in the event log, in the run's log and in any capture that is running, at its `when_ns` | Agent | |
| **(b) Reach the harness** | A harness adapter, or a client of the control API, subscribes to triggers or asks afterwards (`triggers.query {since, kinds, places}`, `trigger_stats`). A test asserts "no triggers", "exactly one `timeout` on bus-1", or waits for one with a time limit | Agent, through the proxy of section 7.7. How the control API is reached from outside is the remote-control work | Batched like every event; the times inside are exact |
| **(c) Keep the traffic** | A capture ring is running (`capture.start` with `ring`, section 19.4). With `snapshot_on: {kinds, places}`, `before_s` and `after_s`, the agent cuts that span out of the ring into a kept recording when a matching trigger passes, and names the trigger in the recording's index. That recording is what the Diagnostics and replay plugin packs and replays | Agent. The two plugins never talk to each other; either may start the ring | The span is exact because the ring was already recording |
| **(d) An engine action** | Off unless the user turns it on. Two forms, below | Engine | Below |

**(d) in two forms**, which is the boundary with `docs/response-timing.md`. That page owns the rules, their clock
and their guarantees, and nothing here changes it.

1. **After the fact.** On a trigger the plugin calls an existing operation: raise a fault, set a line, write
   bytes, fill a slot, switch a rule on or off (`update_rules`). This is tier T2 of section 6: the action comes
   after the trigger was decided, milliseconds after the bytes, and that delay is the plugin's and is not promised.
   It suits "after the first bad check value, drop the device's carrier" and does not suit "answer this frame".
2. **Armed.** For a reaction that must be exact, the plugin does not react; it prepares. It installs a rule set
   made from the description (the fixed start of a message as `bytes`, the frame's silence as `gap_us`, the action
   with its delay), switched off. A trigger switches it on, and the **next** matching frame is matched and
   answered by the engine, with the engine's timing and its lateness reported. This is "staying one step ahead"
   of that page, used for faults. What the description can express and a fixed-length pattern cannot (a length
   inside the frame) stays detection only, until that page's rule language has bounded variable-length patterns,
   which is its decision 6.

Either form needs `faults` or `ports.write`, which this plugin asks for as optional. Without them it is read-only
and (d) is not offered.

**A trigger must not feed itself.** An action that damages traffic causes anomalies, which cause triggers. So
actions from triggers have a budget of their own (10 a second for an instance; over it the actions stop, the
detection goes on and the instance says so), and a trigger whose frame overlaps a fault the same instance asked
for carries `cause: {"own_action": id}`, so that a rule of the user's can leave it out.

**Permissions of this plugin**, as the user sees them in the decision dialog:

| Sentence | Needed | Note |
|---|---|---|
| "See your ports and links, their state and their counters." | Yes | |
| **"Read the data that passes through the ports and links you use it on."** | Yes | The read-traffic permission. An amber row, never granted silently |
| "Read files in a folder you choose." | Yes | The description file and its corpus |
| "Add a section to a support bundle. You see it and choose whether to include it." | Optional | The `triggers` section |
| "Raise faults on the ports you use it on." | Optional | For (d) |
| "Send data from the ports you use it on." | Optional | For (d) |

It asks for no network permission and no `modifier.inline`. It is never in the path of the data.

### 20.9 What this example changed in the design

| Gap | Where it is now |
|---|---|
| The tap could drop without saying where, and joined chunks | The decode stream: one item for each chunk, engine times, offsets, `lost` in place (20.4). Decision 18 |
| Nothing let a plugin interpret traffic for Studio | The `decoder` extension point (20.6). Decision 17 |
| A plugin could report only a status and a log line | The trigger record, with limits, storage and counters in the agent (20.7). Decision 19 |
| A recording could not be kept because of something that happened | `snapshot_on` in `capture.start` (20.8) |
| The agent's `bus_fault` event had no value, link or time | An addition to `vss_agent_proto`, behind `events_bus_fault_detail` (20.4) |
| A plugin had no time source but its own clock | `time` items on the engine's clock (20.3) |
| No shared way to write a protocol down | The description format and its corpus (20.5). Decision 20 |
| What Community gets | The basic framing view, in Studio (20.2). Decision 21 |

The screens are in `docs/gui-design/docs/screens/11-decoding-and-triggers.md`, which is the copy to build from:
the basic framing view as "Rows" in the Monitor, the dialog that starts a decoding, frames and their fields in the
Monitor, the Triggers tab with the name and sentence of every kind, and the choices of section 20.8.
