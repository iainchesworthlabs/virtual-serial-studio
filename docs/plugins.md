# The plugin framework (design)

Status: **proposed**. This page is a design for the owner to review. Nothing here is built. Section 17 lists the
decisions to make, and nothing is implemented until they are made. It builds on the decisions in
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
| Licence files, seats, which edition may load which plugin | The licensing work | A function answers "may this plugin run now" (section 8). This page does not look inside a licence |
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

- In-process plugins with a C ABI (`editions.md` says they may follow). Decision 12 of section 17 keeps the question open.
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
| `<install>/plugins/` | Official plugins that come with Studio or with a Pro install | Administrator |
| Machine plugin directory (`%ProgramData%\VirtualSerialStudio\plugins`, `/usr/local/share/virtual-serial-studio/plugins`, `/Library/Application Support/VirtualSerialStudio/plugins`) | Plugins an administrator installs for everyone | Administrator |
| Per-user plugin directory (`%LOCALAPPDATA%\VirtualSerialStudio\plugins`, `$XDG_DATA_HOME/virtual-serial-studio/plugins`, `~/Library/Application Support/VirtualSerialStudio/plugins`) | Plugins the user installs | The user |

Rules:

- Discovery runs at agent start, when the user presses Rescan, and when a directory changes (watched at most once a
  second per root). It runs on a worker, never on the GUI thread.
- A symbolic link or junction in a plugin directory is not followed. A plugin directory must be a real directory
  owned by the user or by an administrator, with no write access for others (Windows: the ACL is read, and an entry
  that grants write to `Everyone` or `Users` on a machine or install root rejects the plugin).
- At most 64 plugins, and a manifest of at most 256 KiB. More is `rejected: too_many` or `manifest_too_large`.

### 5.3 Verify

In this order, stopping at the first failure. Each step has a reason code (section 12).

1. **Read the manifest bytes once**, into memory. Everything after works on these bytes. The file is never read again
   for the decision.
2. **Parse** with a depth limit, size limits and unknown-key rejection (section 13).
3. **Check the layout**: every listed file exists, is a regular file, has a normalised relative path with no `..`, no
   absolute path, no drive letter, no alternate data stream, no reserved device name, and no case-insensitive
   duplicate. No file exists that the manifest does not list (a signed plugin only).
4. **Check the signature** if there is one: a detached signature over the manifest bytes (section 8.2). The signing key
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

This is open decision 1 of `editions.md`. The proposal, with the reasons.

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
| Rules on a **link** (corrupt, drop or duplicate what crosses it) rather than on a port's replies | A link-level hook in the route path, or the insertion method of section 7.6 | **Perhaps.** The insertion method needs no change. A route hook in `hub` would, and is not proposed without a request |
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

Whether a plugin fixture appears in the Advanced toolbox beside the built-in ones is open decision 2 of `editions.md`
and is repeated in section 17. The API does not depend on the answer.

### 7.6 Behaviour modifiers

A modifier changes traffic on a link. It is the most dangerous extension point, since it sits in the path of bytes, so
it has the most rules.

**How it sits in the path.** Without changing the engine, by insertion (Studio already makes ports and links as
primitives): for a link from A to B the agent creates a **tap pair**, two private endpoints `Min` and `Mout`, links A to
`Min` and `Mout` to B, and the host carries bytes between `Min` and `Mout` through the plugin. These endpoints and links
belong to the instance, are marked internal, are **not** saved in the topology, are not shown as ports and are removed
with the instance. If the plugin or the instance goes away, the host applies the failure rule (below) and, when it
cannot, removes the pair and restores the original link. The modem lines and options of the original link are carried
by the host onto the pair.

This costs two extra hops and a copy. A route hook in `hub` (the proper way) would remove them and is the one change to
the engine that would help; it is **not** proposed here and needs a request (section 17, decision 5). `capabilities`
carries `route_hook` so a later hook can replace the insertion with no change to a plugin.

| Message | From | Content |
|---|---|---|
| `modifier.create` | host | `instance`, `link`, `direction` (`a_to_b`, `b_to_a` or both as two instances), `params`, `budget_us`, `max_batch_bytes`, `max_batch_us`, `on_timeout` |
| `modifier.batch` | host | `instance`, `seq`, `ts_ns` of the first chunk, `chunks`: list of `{offset, length, ts_ns}`; the bytes as one blob |
| `modifier.result` | plugin | `seq`, `actions`: one for each chunk, in order, and a blob for what replaced them |
| `modifier.stop` | host | |

**Batching.** The host collects the chunks that reach `Min` into a batch, and sends it when the first of these happens:
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

**State.** Each instance's saved state is in the agent's private area: `plugins/<id>/<hash-independent id>/instance-<n>.json`.
It holds the instance, its target (port or link by *name*, not by id), parameter values, the override for
`on_timeout`, and the rule set the plugin last sent (so rules come back after a restart without the plugin).
**It is not part of the topology file and the schema of the topology does not change.** When the topology is applied
at start, the agent re-creates the instances whose targets exist and reports the others as `unbound`.

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
| Official | The signature verifies with a key that is in the **official key set** built into Studio | The Virtual Serial Studio badge and "Signed by Iain Chesworth Labs" | Yes (if the licence permits) |
| Third-party signed | The signature verifies with a key that is not official, and the user has trusted that key | The author's name from the key's record and "not verified by Virtual Serial Studio" | After the user trusts the key and the plugin |
| Third-party unsigned | No signature | "Unofficial plugin. It can do anything Studio can." | After an explicit confirmation |
| Signed, key unknown | A signature that verifies only against a key in the file | Treated as unsigned for the decision, with the key shown: "Signed by an author you have not trusted" | As unsigned, or after the key is trusted |

A signature **proves who signed it and that the files were not changed afterwards**. It does not prove that the plugin
is safe, and the interface never says "verified" about anything else.

### 8.2 Files and the signature

```
plugin.json        the manifest (the signed bytes)
plugin.sig         detached signature, a small JSON document
<other files>      listed in the manifest with their hashes
```

`plugin.sig`:

```json
{
  "sig_version": 1,
  "alg": "ed25519",
  "key_id": "<fingerprint, 16 hex>",
  "signature": "<base64 of 64 bytes>",
  "chain": []
}
```

- The signature covers **the exact bytes of `plugin.json`**, not a re-serialisation, and so there is no
  canonicalisation to get wrong. The domain separator `"vss-plugin-v1\0"` is put in front of the bytes before
  signing, so a signature of this kind cannot be reused for any other purpose (including the revocation list).
- The manifest lists every file as `{path, size, sha512}`. The verifier checks each, and that no other file is in the
  directory.
- `key_id` is the first 8 bytes of the SHA-512 of the public key, in hex. The public key itself is in a **key
  record** (below), never trusted from `plugin.sig`.
- `chain` is reserved for delegation: an official key signing an author key. The first release accepts an empty
  chain only. The field exists so that a later version can add it without a format change.

**Algorithm choice.** Ed25519 with SHA-512. It is small, has no parameters to get wrong, and gives deterministic
signatures. The project's dependencies are Boost and Qt, and Qt has no Ed25519 primitive. The options:

| Option | Cost |
|---|---|
| A small vendored implementation (Monocypher, about 3000 lines in two files, public domain or BSD-2-Clause) | A new third-party file in `THIRD_PARTY_NOTICES.md`, and the licence audit that is already decided covers it. Same code on every platform, no `#if`. **Recommended** |
| The operating system's cryptography (CNG, CryptoKit, OpenSSL) | Three code paths, and OpenSSL as a dependency on Linux. Against the rule to pick files in CMake and not to use `#if` |
| Qt's `QCryptographicHash` for hashing | SHA-512 only; no signatures |

This is decision 6.

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
  key** and the signature uses a different domain separator than plugins have. The verifier accepts a list only with a
  higher `serial` than any it has seen, and stores the highest in the trust ledger. A list that fails to verify is
  ignored and reported; a list with a lower serial is ignored silently.
- A revoked plugin or key is `rejected: revoked` at the next check. A running plugin is stopped within a second of
  the list being applied.
- **Expiry.** The manifest has `not_after` (a date; official plugins get a bounded lifetime, and the owner's signing job
  chooses it). An expired plugin is `rejected: expired`. There is a grace choice: the page offers "Run anyway for this
  session" only for **unsigned and third-party** plugins, never for an official one, since an official plugin has an
  update path and a user can always take it.
- **The clock.** The wall clock is only trusted to go forward. The ledger keeps the highest time seen (from the
  system, from a revocation list's `issued_at`, from a signature date). An expiry is checked against
  `max(system now, high-water mark)`. Setting the clock back does not revive an expired plugin. This is a
  **deterrent**, as for licences in `editions.md`: a user who deletes their own ledger defeats it, and the page says
  nothing stronger.
- The built-in **official key set** has a current key and a next key. A release can carry a new current key. A key
  that was withdrawn but not revoked keeps working for plugins signed before its end date only if their signature
  carries a date, which the first version does not have: **a rotated-out key is revoked by the list**, simply.

### 8.6 Administrator policy

A policy file `policy.json` in the machine configuration directory (`%ProgramData%\VirtualSerialStudio\`,
`/etc/virtual-serial-studio/`, `/Library/Application Support/VirtualSerialStudio/`):

```json
{
  "policy_version": 1,
  "allow": "official_only",         // official_only | signed | any (default: any)
  "allow_keys": ["<key_id>"],       // with "signed": these third-party keys are pre-trusted
  "deny_plugins": ["<plugin_id>"],
  "min_sandbox": "partial",         // none | partial | full
  "on_timeout": null,               // "fail_open" | "fail_closed" | null (do not force)
  "permissions_denied": ["net.outbound"]
}
```

- Read at start and when the file changes, on every platform with the same code.
- The file must be owned by an administrator and not writable by the user (Windows: owner `Administrators` or
  `SYSTEM` and no write for `Users`; elsewhere: owned by root, no group or other write bit). **A policy file that
  fails that test, or does not parse, is treated as `official_only`**: fail closed, with a banner: "A policy file was
  found and could not be trusted, so only official plugins will run." A missing file is the default.
- A policy narrows what the user can do. It never grants beyond what the user decides. A user cannot widen it from
  Studio, and the page says "Set by your administrator" with the reason.

### 8.7 Licence and edition

`plugin_entitlement` is an interface the licensing work implements:

```cpp
struct entitlement
{
    bool allowed;
    std::string reason_code;          // e.g. "licence_expired", "pro_required"
    std::optional<std::chrono::sys_days> grace_ends;
};
entitlement check(const plugin_identity& plugin);
```

The host calls it when verifying and when the licence changes. A plugin whose official `requires.edition` is not
met is `disabled: not_licensed`, and the page shows the reason and the renew link, as the licence page does.
Verification never looks inside a licence, and the licence code never sees a plugin's files.

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
beside the warning: "Sandbox: full" or "Sandbox: partial (no network isolation)". A policy can require a minimum
(`min_sandbox`); below it the plugin is `disabled: sandbox_insufficient`.

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

Both are listed in section 17 (decision 4). Whichever is chosen, a test checks that **no descriptor other than the
channel reaches a plugin** (section 16).

The launcher is a separate executable listed in the installers (`agent.md` says each installer holds a fixed list of
programs). It does not link Qt or the engine.

## 10. Failure modes and what the user sees

Words below are proposals. The GUI's strings are copied from `docs/gui-design/` and wrapped in `tr()`, so the
owner or the GUI work needs to add them there before they are built. Plugin-provided text is shown as plain text and
quoted, never mixed into a sentence that could be mistaken for Studio's own.

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
5. **A modifier's hop is asynchronous.** The tap-pair endpoint hands a batch to the plugin's strand and returns;
   the result comes back as a completion on the instance's strand, which writes to `Mout`. A deadline timer on the
   instance's strand enforces `budget_us`. Nothing waits on a future.
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

`manifest_invalid`, `manifest_too_large`, `too_many`, `shadowed`, `layout_invalid`, `file_missing`, `unlisted_file`,
`signature_missing`, `signature_invalid`, `key_unknown`, `changed`, `revoked`, `expired`, `policy`, `not_licensed`,
`api_incompatible`, `capability_missing`, `sandbox_insufficient`, `permission_denied`, `no_hello`, `bad_hello`,
`unresponsive`, `misbehaving` (with the limit's name), `crashed`, `exited`, `start_failed`, `quarantined`.

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
| `requires.api`, `capabilities`, `edition`, `runtime` | Section 7.2, 6.6, 8.7, 8.3 |
| `run` | One executable, a fixed argument list, the platforms it supports. No shell string. A missing platform means the plugin is not offered on this one |
| `permissions` | Section 9.1; unknown names are errors |
| `extension_points` | One or more. `kind` is one of `generator`, `fixture`, `modifier`, `adapter`. `timing` as in 6.4 (`modifier` requires `class` of `inline` or `async`; others default to `async`, `rules` allowed for `generator` and `fixture`) |
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
| `libs/vss_plugin` | The manifest parser, the verifier, the trust store, revocation, policy, the lifecycle state machine, the back-off logic, the rule-set model, the parameter model (no I/O, no processes) | `vss_core` (errors), `vss_config` (JSON helpers), Boost.JSON |
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
| T14 | The revocation list is rolled back or forged | Signed by an official key with a separate domain separator; only a higher serial accepted; the highest serial stored | A user who deletes their ledger |
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

## 17. Decisions for the owner

1. **Where the host lives**: in `vss_agent` (recommended), with the library usable in `vss_gui` and `vss_cli` until
   the agent is in use.
2. **The timing proposal** (section 6): engine rules as T0 in `vss_studio`, three tiers, rules before round trips.
   Approve the tiers and the rule language's growth (microseconds, guards, fault actions, the closed template
   functions).
3. **`fail_open` as the default for a modifier**, with per-instance override and a policy that can force either.
4. **Starting programs from the agent**: the launcher started before any port (recommended), or close-on-exec on all
   descriptors in `vss_ports` and the transports (a change to engine-side code).
5. **A route hook in `hub`** for modifiers. The first version uses insertion (two extra endpoints, no engine change).
   A hook is cleaner and faster but is a `vss_core` change, which needs a request.
6. **Signature algorithm and library**: Ed25519 with SHA-512 through a vendored Monocypher (recommended, public domain
   or BSD-2), or the operating systems' cryptography.
7. **Test keys**: confirm that throwaway keys from a fixed seed, marked TEST ONLY and used only by tests, are
   acceptable. No private key is created for the owner's job.
8. **The official key set**: the owner provides the public keys (a current and a next) as a file; the date format and
   lifetime of `not_after` for official plugins.
9. **The sandbox minimum**: whether an unsigned plugin may run with sandbox `none`, or the default `min_sandbox` is
   `partial`. Recommended: allow, with the warning, as `editions.md` says an unsigned plugin "can do anything Studio
   can".
10. **Whether a plugin fixture appears in the Advanced toolbox** (decision 2 of `editions.md`). The API is the same
    either way.
11. **Whether "run anyway" for an expired third-party plugin** is offered (this page proposes yes for non-official).
12. **In-process plugins** with a C ABI or WASM: not in the first version. Confirm it stays that way.

## 18. Order of work after review

Small vertical slices, each with its tests, each a pull request:

1. **`vss_plugin`: the manifest parser and the verification library** (parse, verify, hash, revocation, policy,
   trust store, reason codes), with unit tests and the three fuzz harnesses. No processes. This is the security
   foundation and is written by the author of this design.
2. **The supervisor**: the state machine, back-off with an injected clock, the channel and the API codec with a fake
   plugin in memory, then real processes through `vss_test_plugin` without a sandbox, then the launcher and one
   platform's sandbox at a time.
3. **The first extension point: generators.** They have the simplest contract (credit flow, one direction, no byte
   path), and they exercise the instance lifecycle, parameters, permissions, the Plugins page and the failure
   states. Fixtures with engine rules follow, then modifiers, then adapters.

Simple, well specified parts of these slices (the parameter model and its form, the `vss_test_plugin` flags, table
tests of the parser) can be handed to subagents. The verifier, the supervisor, the protocol, the concurrency and the
sandbox stay with the author of this design.
