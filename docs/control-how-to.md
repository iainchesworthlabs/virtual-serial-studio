# Controlling Studio from a script

This page shows how to drive Virtual Serial Studio's ports from the command line or from a Python test script, on one
computer. It covers starting the control gateway, making ports, sending and waiting for bytes, watching what happens,
keeping a test report, and letting another program in with a token.

The design, the threat model and the full table of operations are in [remote-control.md](remote-control.md). The agent
that owns the ports is described in [agent.md](agent.md).

## What you get, and what you do not

- **Community edition.** The control gateway on this computer, its tokens, the Python client and the JUnit report need
  no licence.
- **One computer.** The gateway listens on a pipe or socket that only your user account can open, and, if you ask, on
  TCP at `127.0.0.1` and `::1`. It cannot listen on any other address. Controlling one computer from another is a Pro
  plugin and is not part of this page.
- **Loopback ports only.** A port made through the gateway is an in-process loopback port in the agent. Another
  program cannot open it, and the agent cannot attach a serial device to it yet. Use it to test a script or a
  protocol against bytes you send yourself. The ports you make in Studio's window are not the agent's, so the gateway
  does not see them.
- **Built from source.** `vss_agent`, `vss_gate` and the Python client are not in the installers yet. Build them as
  [building.md](building.md) describes. An installed `vss_cli` has the `control` commands and nothing to talk to
  until the other two programs run.

## Start the two programs

The agent owns the ports. The gateway serves the control API to your programs. Start both and leave them running:

```bash
vss_agent &
vss_gate --local-role=control &
```

Without `--local-role=control`, your programs may look but not change anything. Then check that the gateway answers:

```bash
vss_cli control ping
vss_cli control status
```

`status` says who you are to the gateway, what you may do, whether the agent is running and which edition is in
force. `vss_gate` does not read a licence yet, so the edition line says `community` whatever licence is installed.
An exit code of 3 from any `control` command means no gateway is running.

To stop the gateway, press Ctrl-C in its terminal or run `vss_cli control stop`. Stopping it closes no port; the ports
live in the agent.

## What you may do: roles and scopes

Every operation needs a scope. A role is a set of scopes.

| Role | Scopes | Lets a program |
|---|---|---|
| `monitor` | `monitor` | Read ports, links and counters, read the event stream without the bytes, keep a test run |
| `control` | `monitor`, `monitor.data`, `control.ports`, `control.data`, `control.faults`, `record` | Also read the bytes, make, remove and link ports, and send bytes. `control.faults` and `record` allow nothing yet |

Your own programs get the role you gave `vss_gate` with `--local-role`. A token gets the role or scopes you made it
with. A command that needs a scope you do not have ends with `(forbidden)`. The table in
[section 9 of remote-control.md](remote-control.md#operations-and-scopes) lists the scope of each operation.

## Make ports, link them and send bytes

```bash
vss_cli control port create a          # prints the id of the new port, such as p_1
vss_cli control port create b          # p_2
vss_cli control link p_1 p_2           # joins them both ways and prints the link's id, such as l_1
vss_cli control send p_1 --hex=68690d0a
```

Ids are `p_<number>` for a port and `l_<number>` for a link. They stay the same for the life of the port or link.
`send` takes the bytes as hex digits, two for each byte and no spaces, and sends 64 KiB at most. `link` with
`--one-way` joins the first port to the second only.

Look at what exists:

```bash
vss_cli control port show p_1
vss_cli control links
vss_cli control link show l_1
```

Remove a port when you are done. Its links go with it:

```bash
vss_cli control port remove p_1
```

When the engine refuses something, the line ends with the name of its error, as in `(engine.not_found)`.

## Wait for bytes

`expect` waits for a pattern to go by on a port. This is the building block of a test step: send a request and check
that the answer comes in time.

```bash
vss_cli control expect p_2 --pattern=4f4b0d0a --timeout=2000 --send-to=p_1 --send=41540d0a4f4b0d0a
```

That listens on `p_2`, then sends `AT\r\nOK\r\n` to `p_1`, and waits up to two seconds for `OK\r\n` to arrive on
`p_2`. Because the gateway starts listening before it sends, a fast reply cannot be missed.

- The pattern is 1 to 256 bytes. The timeout is 1 to 60000 ms and is 1000 ms when you leave it out.
- When the pattern came, the command prints `matched`, the number of bytes that went by before it, the time in
  nanoseconds on the computer's steady clock, and the number of events lost.
- When it did not come, the line ends with `(timeout)` and the exit code is 1.
- `--tx` waits for what the port sent instead of what it received.
- It needs the scope `monitor.data`, and `control.data` as well to send.

## Watch what happens

```bash
vss_cli control watch --count=20           # 20 events, without the bytes
vss_cli control watch --data --seconds=10  # with the bytes, for ten seconds
```

Each line is one event: batch number, time, kind, port id, direction, size and, with `--data`, the bytes in hex.
`--ports=p_1,p_2` limits it to those ports. A line that starts with `#` says how many events were lost before a
batch; that happens when a program reads more slowly than the ports talk. `--data` needs the scope `monitor.data`.

## Keep a test report

A run collects the checks of a test and gives them back as JUnit XML, which a CI system can read.

```bash
vss_cli control run begin smoke
vss_cli control expect p_2 --pattern=4f4b --send-to=p_1 --send=4f4b --label="the device answers"
vss_cli control run record "front panel" --outcome=skipped --message="nobody looked"
vss_cli control run end
vss_cli control run report --out=smoke.xml
```

While a run is open, each `expect` you make is noted with how it ended; `--label` is its name in the report.
`run record` adds a check you made yourself, with the outcome `passed`, `failed`, `error` or `skipped`. `run end`
prints the counts. One run is open at a time for a user or a token, it stays open across commands, and only whoever
opened it can read its report. The gateway keeps the eight newest runs in memory, with up to 1000 checks each, and
loses them when it stops.

## From Python

The client in `adapters/python` needs Python 3.9 or later and no other package. Use it from the source tree or
install it with `pip install ./adapters/python`.

```python
from vss_control import Client, ControlError

with Client.connect() as c:
    a = c.create_port("a")
    b = c.create_port("b")
    c.connect_ports(a, b, one_way=True)
    run = c.begin_run("smoke test")
    c.expect_bytes(port=b, pattern=b"OK\r\n", timeout_ms=2000,
                   send_port=a, send=b"AT\r\nOK\r\n", label="the device answers")
    try:
        c.expect_bytes(port=b, pattern=b"never", timeout_ms=200, label="nothing else comes")
    except ControlError as error:
        print(error.code)            # timeout; the connection is still usable
    c.end_run()
    with open("smoke.xml", "w", encoding="utf-8") as report:
        report.write(c.report_junit(run["run"]))
    c.remove_port(a)
    c.remove_port(b)
```

Before it sends anything, the client checks that the pipe or socket is served by your own user account, and refuses
to connect when it cannot check. An error from the gateway is a `ControlError` with the code the gateway gave, and an
`EngineError` when the code starts with `engine.`. The calls are listed in
[the client's README](../adapters/python/README.md).

## Let another program in with a token

By default only programs running as you can connect, and they are you to the gateway. A token lets a program connect
as something else, with only the scopes you choose.

```bash
vss_cli control token create --label=rig --role=monitor --out=rig.token
vss_cli control status --token-file=rig.token
vss_cli control token list
vss_cli control token revoke <id>
```

- A token is shown once, on the screen or in the `--out` file. The gateway keeps only a hash of it. Keep the file
  like a password.
- A token lives 30 days unless you give `--days` (1 to 365).
- A token is never given on a command line, where other programs can read it. Commands take `--token-file`.
- `revoke` ends the token and closes its sessions.

## Use TCP on this computer

Some test tools cannot open a named pipe or a Unix socket. For those, the gateway can also listen on TCP, on this
computer only:

```bash
vss_gate --local-role=control --loopback-port=5800 &
vss_cli control token create --label=rig --role=control --transport=loopback --out=rig.token
vss_cli control status --loopback-port=5800 --token-file=rig.token
```

- It is off unless you give `--loopback-port`, and it listens at `127.0.0.1` and `::1` and nowhere else.
- The system does not say who is at the other end of a TCP connection, so every TCP connection needs a token, and
  the token must have been made with `--transport=loopback`. A token made for the pipe or socket is refused over TCP,
  and the other way round. Give `--transport` twice for a token that is good on both.
- The token crosses the connection in clear. That is acceptable on the loopback address of one computer and is the
  reason this listener cannot be put on a network.
- Any other user of the computer can connect to the port. Without a token they get nothing, and repeated wrong
  tokens are slowed down.
- The token commands, `sessions`, `kick` and `stop` cannot be used over TCP.

In Python: `Client.connect(loopback_port=5800, token_file="rig.token")`.

## See who is connected

```bash
vss_cli control sessions      # one line for each session
vss_cli control kick 4        # close session number 4
```

The gateway also writes an audit log: every connection, every refusal and every change to a port, with who made it,
and never the bytes or a token. `vss_gate --audit-dir=<path>` puts the log in a directory of your choice.

## When something does not work

| What you see | What it means |
|---|---|
| Exit code 3 | No gateway is running for your user. Start `vss_gate` |
| `(unavailable)` | The gateway runs and the agent does not. Start `vss_agent` |
| `(forbidden)` | You do not hold the scope the command needs. Start `vss_gate` with `--local-role=control`, or make a token with the role or scope |
| `(unauthenticated)` | The token is wrong, expired, revoked, or was made for the other transport |
| `(not_found)` | No port or link has that id. `vss_cli control links` and `port show` say what exists |
| `(timeout)` from `expect` | The pattern did not come in time |
| `(engine.<name>)` | The engine refused; the name is its error, such as `engine.not_found` or `engine.unsupported` |
| `(busy)` | The gateway has as many sessions as it takes. Close one with `kick` |
