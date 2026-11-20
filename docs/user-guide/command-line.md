# Command line equivalents

| Task | Command |
|---|---|
| Create one virtual port | `vss_cli virtual --kind=auto` |
| Create a Windows port with the COM number you choose | `vss_cli virtual --kind=windows-driver --com=3` |
| Create a port with a stable name (Linux, macOS) | `vss_cli vport --name=ttyVSS0` |
| Create and hold a topology | `vss_cli run topology.json --seconds=60` |
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
| List the installed plugins | `vss_cli plugin list` (add `--json` for a script) |
| Check a plugin folder | `vss_cli plugin verify <folder>` |
| Install or remove a plugin | `vss_cli plugin install <folder>`, `vss_cli plugin remove <id>` |
| Run one installed plugin | `vss_cli plugin run <id>` (add `--json` for a script) |

`virtual`, `vport`, `bus`, `capture`, `run`, `share`, `join`, `bridge` and
`loopback` keep running until `Ctrl-C`, `SIGTERM` or, on Linux and macOS, `SIGHUP`
(a closing terminal), or for `--seconds=N`, where N is 1 or more. On Windows
`Ctrl-Break` and closing the console window stop them too. They take their ports
down before they end. `--verbose` prints the port table every half second while
`run`, `share`, `join`, `bus`, `bridge`, `loopback` and `vport` wait, and `vss_cli
--help` lists every option. An option takes its value as `--name=value` or as
`--name value`, except under `control`, `agent`, `permissions` and `plugin`,
which take `--name=value` only; an option a command does not have, a missing value and a value that
is not a number where one is needed end the command with exit code 2 and one line
on standard error. `vss_cli --version` prints the version of the build, as
`vss_gui --version` does.

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
`--format`, or comes from the extension of `--out` (`.cap`, `.csv`, `.jsonl`),
or is `cap`. Each line is flushed as it is written, so a file is complete up to
the last transfer even if the process is killed. A port whose *Capture
traffic* is off in the topology is not recorded. `--port` names the ports to
keep (see [Capture files](monitor-and-capture.md#capture-files) for why you usually want one), and
the command stops with a list of the real names if one is wrong. The file is
readable by its owner only, where the system has such a setting.

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
- `--speed=2` plays twice as fast and `--speed=0.5` half as fast. Times are
  absolute deadlines, so they do not drift over a long file.
- `--loop` starts again after the length of the capture, which is the offset of
  its last record whichever direction that is, so a recording repeats at the pace
  it was taken.
- It stops when the last record has been sent, or at `Ctrl-C`, or after
  `--seconds`, and prints how many records and bytes it sent. After the last record
  it waits until the port has written everything that was queued, for as long as
  the port keeps taking bytes. It exits with 1, and says how much was written, when
  the port refused a write or stopped taking bytes before that. Lines of the file
  that cannot be read are skipped and counted.

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
Delays are limited to one hour. `respond` runs until `Ctrl-C` or `--seconds`, or,
for a poll-only script with a number of cycles, until the last step has gone,
and prints how many requests it matched and what it sent.
