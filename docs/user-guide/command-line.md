# Command line equivalents

| Task | Command |
|---|---|
| Create one virtual port | `vss_cli virtual --kind=auto` |
| Create a Windows port with the COM number you choose | `vss_cli virtual --kind=windows-driver --com=3` |
| Create a port with a stable name (Linux, macOS) | `vss_cli vport --name=ttyVSS0` (see [Virtual ports on Linux and macOS](../posix-virtual-ports.md)) |
| Create and hold a topology | `vss_cli run topology.json --seconds=60` (add `--reconnect` to open `tcp://` and `rfc2217://` clients again when the connection drops; see [Setups](setups.md)) |
| Share a device | `vss_cli share serial://COM3 --listeners=3 --seconds=60` |
| Join publishers | `vss_cli join tcp://a:1 tcp://b:2 --target=serial://COM4 --seconds=60` |
| Put ports on a shared bus, with a listen-only tap | `vss_cli bus --virtual=3 --tap=virtual` |
| Bridge two transports | `vss_cli bridge serial://COM3 tcp-listen://0.0.0.0:4001 --seconds=60` |
| Loopback demo | `vss_cli loopback --seconds=30` |
| List devices | `vss_cli devices` (add `--json` for a script) |
| Record traffic to a file | `vss_cli capture topology.json --out bus.cap --port master` |
| Record a port without a topology | `vss_cli capture --uri=serial://COM5 --out run.jsonl --seconds=60` |
| Play a capture into a port | `vss_cli play bus.cap --uri=serial://COM6 --speed=2` |
| Make a port answer like a device | `vss_cli respond device.json --uri=serial://COM7` |
| Measure throughput | `vss_cli bench --from=… --to=…` |
| Self-test the wiring | `vss_cli selftest` |
| Check what the system may block | `vss_cli permissions check` |
| List the installed plugins | [`vss_cli plugin list`](plugins.md#listing-plugins-with-vss_cli-plugin-list) (add `--json` for a script) |
| Check a plugin folder | [`vss_cli plugin verify <folder>`](plugins.md#checking-a-plugin-with-vss_cli-plugin-verify) |
| Install or remove a plugin | [`vss_cli plugin install <folder>`, `vss_cli plugin remove <id>`](plugins.md#installing-a-plugin-with-vss_cli-plugin-install) |
| Run one installed plugin | [`vss_cli plugin run <id>`](plugins.md#running-a-plugin-with-vss_cli-plugin-run) (add `--json` for a script) |
| Show the edition and the licence files | [`vss_cli licence`](licences.md#from-the-command-line), `vss_cli licence import <file>` |

[Other commands](#other-commands), below, covers `permissions`, `debug`,
`control` and `agent`.

`virtual`, `vport`, `bus`, `capture`, `run`, `share`, `join`, `bridge` and
`loopback` keep running until `Ctrl-C`, `SIGTERM` or, on Linux and macOS, `SIGHUP`
(a closing terminal), or for `--seconds=N`, where N is 1 or more. On Windows
`Ctrl-Break` and closing the console window stop them too. They take their ports
down before they end. `--verbose` prints the port table every half second while
`run`, `share`, `join`, `bus`, `bridge`, `loopback` and `vport` wait, and `vss_cli
--help` lists every option. An option takes its value as `--name=value` or as
`--name value`, except under `control`, `agent` and `permissions`, which take
`--name=value` only. An option a command does not have, a missing value and a value that
is not a number where one is needed end the command with exit code 2 and one line
on standard error; `agent`, `control`, `debug`, `licence`, `permissions` and
`plugin` read their own options and may follow that line with their usage text.
`vss_cli --help` and `-h` print the help and exit with 0, and `vss_cli --version`
prints the version of the build, as `vss_gui --version` does.

## Recording with `vss_cli capture`

```bash
vss_cli capture <topology.json | --uri=<uri> ...> --out=<file>
                [--format=cap|csv|jsonl] [--port=<name>[,<name>...]]
                [--seconds=N]
```

`capture` applies the topology (or opens each `--uri`, which may be repeated;
each one is named by its URI), starts recording and keeps the ports open until
you press `Ctrl-C`, it receives `SIGTERM`, or `--seconds` has passed. It does
not wait for a window to refresh, and it does not cut payloads, so every byte
that moves is in the file, with the time the engine saw it. The format is
`--format`, or comes from the extension of `--out` (`.cap`, `.csv`, `.jsonl`;
`.ndjson` and `--format=ndjson` count as `jsonl`), or is `cap`. Each line is
flushed as it is written, so a file is complete up to the last transfer even if
the process is killed. A port whose **Capture traffic in the monitor** is off
(`"capture": false` in the topology file) is not recorded. `--port` names the
ports to keep (see [Capture files](monitor-and-capture.md#capture-files) for
why you usually want one), and the command stops with a list of the real names,
and exit code 2, if one is wrong. The file is readable by its owner only, where
the system has such a setting. When the command ends it prints how many records
and bytes it wrote, and it exits with 1 if the file could not be written.

## Replaying with `vss_cli play`

```bash
vss_cli play <file> [<topology.json>] (--uri=<uri> | --port=<name>)
             [--speed=X] [--loop] [--direction=R|W] [--from=<name>]
             [--seconds=N]
```

`play` reads a `.cap` or `.jsonl` capture (not `.csv`; record JSON Lines when
you want the port names) and writes the records of one direction into one port,
each at its recorded time. That lets a recorded master or device drive the
other programs on a bus without the program that made the recording.

- The port is what a program would open: give `--uri` for a port of the
  operating system (`serial://COM6`, a pseudo terminal path, `tcp://host:port`),
  or a topology and `--port=<name>` for a port of that topology. What `play`
  writes goes out of that port to whatever is on its far end, so for a port
  Studio made, open the end the program would have opened. A loopback port echoes
  its writes back in as received bytes, which makes it a stand-in for the
  program in a topology of its own.
- **R** records (the default) are the bytes the recorded port received, so they
  replay what the program on that port *sent*. `--direction=W` plays the other
  direction. `--from=<name>` keeps only the records captured on that port, for a
  file that holds several; a `.cap` file has no names, so it needs none.
- `--speed=2` plays twice as fast and `--speed=0.5` half as fast; the number
  must be above zero.
- `--loop` starts again after the length of the capture, which is the offset of
  its last record whichever direction that is, so a recording repeats at the pace
  it was taken.
- It stops when the last record has been sent, or at `Ctrl-C`, or after
  `--seconds`, and prints how many records and bytes it sent. After the last record
  it waits until the port has written everything that was queued, for as long as
  the port keeps taking bytes. It exits with 1, and says how much was written, when
  the port refused a write or stopped taking bytes before that. Lines of the file
  that cannot be read are skipped and counted. It also exits with 1 when the file
  cannot be read or has no records of the chosen direction (and, with `--from`,
  of that port).

## Scripted devices with `vss_cli respond`

```bash
vss_cli respond <script.json> [<topology.json>] (--uri=<uri> | --port=<name>)
                [--seconds=N]
```

`respond` makes one port act as a device, or as a master that polls, with no
program behind it. The behaviour is in a script, a JSON file of its own: the
topology file and its schema do not change. The port is chosen as for `play`:
the end a program would open.

```json
{
  "version": 1,
  "seed": 7,
  "rules": [
    { "name": "status", "match": "02 31", "mask": "ff f0",
      "reply": "02 31 41 03", "delay_ms": 150, "jitter_ms": 10 }
  ],
  "poll": {
    "start_ms": 100, "interval_ms": 1000, "cycles": 0,
    "steps": [ { "send": "02 31 03" },
               { "send": "02 32 03", "at_ms": 250, "jitter_ms": 5 } ]
  }
}
```

**Rules** make a device. `match` is the bytes to wait for, in hex. `mask`, when
given, has as many bytes as `match`, and only the bits that are 1 in it are
compared, so `"02 30"` with `"ff f0"` matches `02 30` to `02 3f`. `reply` is
sent after `delay_ms` (default 0), plus or minus a random amount of up to
`jitter_ms`. The delay is counted from the moment the request arrived, not from
when the script got to it, so a reply meant for a 200 ms window is not late by
the time the program took to look.

- Bytes are matched as a stream. A request that arrives in two reads is found,
  and the bytes up to the end of a match are used up. When several rules could
  match, the one that starts first wins, and then the one listed first.
- A reply is written to the port as one piece.

**Poll** makes a master. A cycle starts `start_ms` after the script starts, and
a new one every `interval_ms`; each step is sent `at_ms` after its cycle
started, plus or minus `jitter_ms`. `cycles` is how many cycles to run, and 0
means until you stop it. With no `interval_ms` the list is sent once. A script
may have rules, a poll list, or both.

`seed` makes the jitter repeat from run to run. A key the script does not know
is an error that names it, so a misspelt `delay_ms` is found and not ignored.
`version` may be left out, and when it is given it must be 1. `name` and
`description` are allowed at the top of the script and are not used. Every time
in a script (`delay_ms`, `jitter_ms`, `start_ms`, `interval_ms` and `at_ms`) is
a whole number of milliseconds from 0 to 3600000, which is one hour. A poll
needs at least one step, and `cycles` above 1 needs an `interval_ms`. A script
file may be up to 1 MiB. A script that cannot be used is one line on standard
error, with the reason, and exit code 1. `respond` runs until `Ctrl-C` or
`--seconds`, or, for a poll-only script with a number of cycles, until the last
step has gone, and prints how many requests it matched and what it sent.

## Other commands

`vss_cli --help` lists these too.

| Command | What it does |
|---|---|
| `vss_cli permissions check [--json] [--device=<path>] [--listen=<kind>-listen://host:port]` | Says what the operating system may stop Studio doing and the smallest step to fix it. `<kind>` is `tcp`, `udp` or `rfc2217`. It reads only and changes nothing. On Windows, `permissions firewall-allow` and `permissions firewall-remove` add and remove one Windows Firewall rule for one of Studio's programs. They need administrator rights, and Studio runs them for you after you agree, so you do not need to run them yourself |
| `vss_cli licence`, `vss_cli licence import <file>` | Show the edition and the licence files, and import a licence file. See [Licences](licences.md#from-the-command-line) |
| `vss_cli plugin list`, `verify`, `install`, `remove`, `run` | See [Plugins](plugins.md) |
| `vss_cli debug` | Asks a running Studio what its ports, its engine and Windows are doing: `list`, `status`, `snapshot`, `com`, `driver`, `plugins`, `trace`, `run <operation>`, `traffic`, `gui`, `screenshot --out=<file>.png` and `audit-verify`. `--json` prints JSON, and `trace` takes `--follow`, `--failed`, `--endpoint=N` and `--last=N`. Studio must have **Debug access** on, and the person at the computer allows each program; see [Help and diagnostics](settings-and-help.md#help-and-diagnostics) and [A debug API for a running Studio](../debug-api.md). Exit code 3 means that Studio is not listening or did not allow the program |
| `vss_cli control` | Makes and links ports, sends and waits for bytes and keeps a test report, through the control gateway, from a script. See below |
| `vss_cli agent` | Works with the ports of your `vss_agent`. See below |

### The gateway and the agent

`vss_cli control` and `vss_cli agent` need a program that is running: the control
gateway, `vss_gate`, and the agent, `vss_agent`. You start both by hand, and they
are in no installer or package. Build them from source as
[Building](../building.md) describes. Without them, `control` prints `Remote
control: off` and exits with 3, and `agent` says that no agent is running and
exits with 3. The ports the gateway and the agent make are not the ports you make
in Studio's window. See [Controlling Studio from a script](../control-how-to.md)
for the steps, the roles and scopes, tokens and the Python client.

`vss_cli control` takes its options as `--name=value`. A token is never given on
the command line: `--token-file=<file>` reads it from the first line of a file.
With `--loopback-port=<port>` and `--token-file`, most of these commands connect
over TCP to `127.0.0.1` instead of the pipe or socket; `token`, `sessions`,
`kick`, `audit` and `stop` cannot be used that way.

| Command | What it does |
|---|---|
| `control status`, `control ping`, `control stop` | `status` says who you are to the gateway and what you may do. `ping` asks only whether it answers. `stop` ends the gateway and closes no port |
| `control port create <name>`, `port remove <id>`, `port rename <id> <name>`, `port show <id>` | Make, remove, rename and show a port. `port create` prints its id, such as `p_3` |
| `control link <id> <id> [--one-way]`, `unlink <id>`, `links`, `link show <id>` | Link two ports both ways, or one way with `--one-way`, take a link away, and show the links. A link has an id such as `l_2` |
| `control send <id> --hex=<digits>` | Sends bytes to a port, up to 64 KiB, as two hex digits for each byte |
| `control expect <id> --pattern=<digits> [--timeout=<ms>] [--tx] [--send-to=<id> --send=<digits>]` | Waits for 1 to 256 bytes to go by on a port, for 1 to 60000 ms (1000 by default). It prints `matched` and some counts, or ends with `(timeout)` and exit code 1 |
| `control watch [--data] [--ports=<id>,...] [--interval=<ms>] [--seconds=<n>] [--count=<n>]` | Prints what happens to the ports, one line for each event |
| `control run begin <name>`, `run record <name> --outcome=passed\|failed\|error\|skipped`, `run end`, `run report [<run id>] [--out=<file>]` | Keep a test report; `run report` gives it as JUnit XML |
| `control token create`, `token list`, `token revoke <id>`, `sessions`, `kick <number>`, `audit verify` | Tokens for other programs, who is connected, and a check of the gateway's audit log |

`vss_cli agent` takes its options as `--name=value` too. `agent status`, `ports`
and `stop` show or end the agent; `create-port <name>` makes a loopback port that
stays after the command has exited; `remove-port <name>`; `connect <first>
<second> [--one-way]`; `write <port> <text> [--hex]`; and `watch [--data]
[--seconds=N]` prints the agent's events. `--instance=<name>` names the agent to
use. The agent makes loopback ports only, and Studio does not
use it yet. [The agent](../agent.md) describes it.
