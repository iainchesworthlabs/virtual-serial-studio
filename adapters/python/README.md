<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# vss-control

A Python client for the control gateway of Virtual Serial Studio (`vss_gate`,
see `docs/remote-control.md`). It makes and links ports, sends bytes, waits for
bytes, and reads the event and traffic streams, from a test script.

It needs Python 3.9 or later and nothing else: only the standard library. It
talks to this user's gateway over a Windows named pipe or a Unix domain socket
and has no network code. The package can be used from this directory without
installing it (`sys.path.insert(0, "adapters/python")`), or installed with
`pip install ./adapters/python`.

## Use

Start `vss_agent`, then `vss_gate --local-role=control` (the default role may
look but not change anything).

```python
from vss_control import Client, ControlError

with Client.connect() as c:
    print(c.hello["principal"], c.hello["scopes"])
    a = c.create_port("a")
    b = c.create_port("b")
    c.connect_ports(a, b, one_way=True)
    # Listen on b, send to a, and wait for the answer, in one request.
    found = c.expect_bytes(port=b, pattern=b"OK\r\n", timeout_ms=2000,
                           send_port=a, send=b"AT\r\nOK\r\n")
    print(found["offset"])           # 4 bytes went by before the pattern
    try:
        c.expect_bytes(port=b, pattern=b"never", timeout_ms=200)
    except ControlError as error:
        print(error.code)            # timeout; the connection is still usable
    c.remove_port(a)
    c.remove_port(b)
```

A run collects the checks of a test and gives them back as a JUnit XML report,
for a test runner or a CI system:

```python
from vss_control import Client, ControlError

with Client.connect() as c:
    a = c.create_port("a")
    b = c.create_port("b")
    c.connect_ports(a, b, one_way=True)
    run = c.begin_run("smoke test")
    # While the run is open, each expect_bytes of this user is noted; the
    # label is its name in the report.
    c.expect_bytes(port=b, pattern=b"OK", timeout_ms=2000,
                   send_port=a, send=b"OK", label="the device answers")
    try:
        c.expect_bytes(port=b, pattern=b"never", timeout_ms=200,
                       label="nothing else comes")
    except ControlError:
        pass                         # noted as a failure in the run
    c.record_in_run("front panel", "skipped", message="nobody looked")
    counts = c.end_run()
    print(counts["tests"], counts["failures"])   # 3 1
    with open("smoke.xml", "w", encoding="utf-8") as report:
        report.write(c.report_junit(run["run"]))
    c.remove_port(a)
    c.remove_port(b)
```

A run belongs to the user or token that began it, not to one connection:
another `Client` of the same user may add checks to it and end it. One run is
open at a time (`conflict` for a second), and only its owner can read its
report.

`Client.connect(instance=None, token=None, token_file=None, name="python",
timeout=10.0)` connects. `instance` is the name given to `vss_gate --instance=`.
`token` is a token as text, `token_file` the path of a file whose first line is
one (`vss_cli control token create --label=rig --role=control --out=rig.token`).
With neither, the connection is the local user. `timeout` is the seconds one
call may take.

Calls: `status()`, `list_ports()`, `create_port(name)`, `remove_port(id)`,
`connect_ports(first, second, one_way=False)`, `send(port, data)`,
`expect_bytes(port, pattern, timeout_ms=1000, send_port=None, send=b"",
label=None, direction=None)`, `subscribe_events(interval_ms=250, ports=None)`,
`subscribe_traffic(...)`, `unsubscribe_events()`, `unsubscribe_traffic()`,
`next_events(timeout=1.0)`, `begin_run(name)`, `record_in_run(name, outcome,
message="", duration_ms=0)`, `end_run()` and `report_junit(run=None)`. Each
call needs the scope that the gateway asks for (see the table in section 9 of
`docs/remote-control.md`) and raises `ControlError` with the code `forbidden`
without it.

`next_events` returns a dict with `seq`, `revision`, `dropped`, `mono_ns`,
`wall_ms` and `events`, or None when no batch came in time. Each event has
`kind`, `port`, `name`, `direction`, `size`, `data` (the bytes, only on the
traffic stream), `lines`, `message`, `error` and `mono_ns`. Batches that arrive
while a call waits for its answer are kept, the newest 256.

## Errors

`ControlError` has `code` (the wire code, such as `forbidden`, `timeout`,
`not_found`), `message` and `retryable`. `EngineError` is a `ControlError` whose
code starts with `engine.`, such as `engine.not_found`.

`TransportError` is a `ControlError` for what went wrong on this side. Its codes
are `not_running`, `not_same_user`, `peer_check_failed`, `address_too_long`,
`busy`, `client_timeout`, `connection_closed`, `protocol_error` and `io_error`.

One connection is used by one thread at a time. A call that takes longer than
`timeout` closes the connection and raises `TransportError` with the code
`client_timeout`; so does a frame that breaks the framing rules (longer than
1 MiB, or with an impossible header size). `next_events` is the exception: it
only reads, so a call that finds nothing in time returns None and the
connection goes on. A `timeout` error from the gateway (the pattern of
`expect_bytes` did not come) is an ordinary reply and the connection goes on.

## Timeouts on Windows

A named pipe opened with `open(path, "r+b", buffering=0)` blocks without any
time limit. This client opens the pipe for overlapped I/O through `ctypes`
instead. Each read and write is started, waited for with a time limit, and
cancelled (`CancelIoEx`) and waited for to its end when the time is up. This
needs no thread, so there is no reader thread to stop and no queue between it
and the caller. On Unix the socket's own timeout is used.

## Who serves the endpoint

Before anything is sent, the client checks that the endpoint is served by this
user, as the C++ client does (`vss_control/_peer.py`). On Windows the process
that serves the pipe must have the same user SID as this process. On Linux
the socket's directory must be private and this user's, the socket must be
this user's, and the kernel's record of the serving process (`SO_PEERCRED`)
must say this user. macOS does the same with `getpeereid`. On any other
system the check cannot be made and the connection is refused. A check that
fails for any reason refuses the connection.

## Tokens

A token is sent in the first request, and the client keeps no copy: no
attribute holds it, and no error text, `repr` or message contains it. Python
cannot wipe a string, so a copy can stay in memory until it is collected.
Prefer `token_file`, so the token is not in the script.

## Tests

```
VSS_AGENT_EXE=.../vss_agent VSS_GATE_EXE=.../vss_gate VSS_CLI_EXE=.../vss_cli \
    python -m unittest discover -s adapters/python/tests -v
```

Without `VSS_AGENT_EXE` and `VSS_GATE_EXE` the tests that start the programs
are skipped; the others need no program. Without `VSS_CLI_EXE` the token tests
are skipped. The same run is the `python_adapter` test of `ctest`.
