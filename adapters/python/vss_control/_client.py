# SPDX-License-Identifier: GPL-3.0-or-later
"""The client of the control gateway.  It does what
libs/vss_control/src/client.cpp does: one connection, one thread at a time,
blocking calls with a time limit.

Requests carry an increasing `id`.  The reply with the same id is the answer;
event batches that arrive while an answer is awaited are kept, the newest 256,
for `next_events`; an error reply with id 0 means the gateway is ending the
session and is raised.

A call that runs out of time closes the connection and raises TransportError
(code `client_timeout`): the answer may still come, and nothing says which
request it belongs to once the caller has given up, so the connection is not
used again.  The one exception is `next_events`, which only reads: a batch is
kept whole in the frame reader until it is complete, so waiting for one and
finding none leaves the connection as it was.
"""

from __future__ import annotations

import collections
import time

from . import _transport
from ._errors import ControlError, TransportError, error_from_reply
from ._frame import FrameError, FrameReader
from ._wire import (
    API_VERSION,
    Batch,
    WireError,
    clean_text,
    decode_server_message,
    encode_request,
)
from ._address import valid_instance_name

# Batches kept while the caller is busy with requests.  Older ones go.
_MAX_KEPT_BATCHES = 256
# How much one read asks for.
_READ_SIZE = 64 * 1024
# The longest a token is, and the most of a token file that is looked at.
_MAX_TOKEN_SIZE = 128
_TOKEN_FILE_READ_LIMIT = 256
# The most ports a subscription may name, and the limits of the numbers.
_MAX_PORT_FILTER = 64
_U32_MAX = (1 << 32) - 1
_U64_MAX = (1 << 64) - 1


def _read_token_file(path):
    """The first line of the file, without its line ending."""
    with open(path, "rb") as stream:
        raw = stream.read(_TOKEN_FILE_READ_LIMIT + 1)
    first, newline, _rest = raw.partition(b"\n")
    if not newline and len(raw) > _TOKEN_FILE_READ_LIMIT:
        raise ValueError("the token file does not hold a token")
    if first.endswith(b"\r"):
        first = first[:-1]
    try:
        return first.decode("ascii")
    except UnicodeDecodeError:
        raise ValueError("the token file does not hold a token") from None


def _check_token(text):
    if (
        not isinstance(text, str)
        or not text
        or len(text) > _MAX_TOKEN_SIZE
        or not text.isascii()
        or not text.isprintable()
        or " " in text
    ):
        # The text is not in the message: it may be a token.
        raise ValueError("the token is not a token")
    return text


def _bytes_argument(value, name):
    if isinstance(value, str) or not isinstance(value, (bytes, bytearray, memoryview)):
        raise TypeError("%s must be bytes, not %s" % (name, type(value).__name__))
    return bytes(value)


def _ports_argument(ports):
    if ports is None:
        return []
    if isinstance(ports, str):
        raise TypeError("ports must be a list of port ids")
    ports = [clean_text(port) for port in ports]
    if len(ports) > _MAX_PORT_FILTER:
        # The gateway ends a connection that sends more.
        raise ValueError("a stream may name %d ports at most" % _MAX_PORT_FILTER)
    return ports


def _number(value, name, largest):
    """A whole number that the gateway reads: a larger or negative one makes
    the request malformed, and the gateway ends the connection for that."""
    value = int(value)
    if not 0 <= value <= largest:
        raise ValueError("%s is out of range" % name)
    return value


class Client:
    """A connection to this user's control gateway.  Make one with
    `Client.connect`; use it from one thread at a time."""

    def __init__(self, transport, timeout):
        # Not for callers: `connect` makes the transport and says hello.
        self._transport = transport
        self._timeout = float(timeout)
        self._reader = FrameReader()
        self._events = collections.deque(maxlen=_MAX_KEPT_BATCHES)
        self._last_id = 0
        self._closed = False
        self._hello = {}

    # -- connecting ----------------------------------------------------------

    @classmethod
    def connect(
        cls,
        instance=None,
        token=None,
        token_file=None,
        name="python",
        timeout=10.0,
        loopback_port=None,
    ):
        """Connects to this user's gateway and says hello.

        `instance` names a separate gateway (up to 32 of `A-Z a-z 0-9 _ -`);
        None is the user's ordinary one.  `token` is a token as text, or
        `token_file` the path of a file whose first line is one; with neither
        the connection is the local user.  `name` is what the audit log calls
        this program.  `timeout` is the seconds one call may take.

        `loopback_port` (1 to 65535) connects over TCP to 127.0.0.1 at that
        port, where `vss_gate --loopback-port` listens, instead of to the
        local pipe or socket.  There is no way to name another host.  Nobody
        is identified over TCP, so a token is required (`token` or
        `token_file`), made for it with `vss_cli control token create
        --transport=loopback`, and `instance` does not apply; ValueError
        otherwise, before anything is connected.

        TransportError (`not_running`, `not_same_user`, ...) when the gateway
        cannot be reached or is not this user's, and ControlError
        (`unauthenticated`, `busy`, ...) when it refuses the hello.
        """
        if instance is None:
            instance = ""
        if not valid_instance_name(instance):
            raise ValueError("an instance name is up to 32 letters, digits, _ and -")
        if token is not None and token_file is not None:
            raise ValueError("give a token or a token file, not both")
        if not timeout > 0:
            raise ValueError("the timeout must be more than 0")
        if token_file is not None:
            token = _read_token_file(token_file)
        if token is not None:
            token = _check_token(token)
        if loopback_port is not None:
            if (
                isinstance(loopback_port, bool)
                or not isinstance(loopback_port, int)
                or not 1 <= loopback_port <= 65535
            ):
                raise ValueError("a loopback port is a whole number from 1 to 65535")
            if instance:
                raise ValueError("an instance name does not apply over TCP")
            if token is None:
                raise ValueError("a connection over TCP needs a token")

        # Who serves the endpoint is checked inside; nothing is sent first.
        # Over TCP nothing can be checked, and the preface goes first.
        transport = _transport.open_transport(instance, float(timeout), loopback_port)
        return cls._greet(transport, timeout, name, token)

    @classmethod
    def _greet(cls, transport, timeout, name, token):
        """Says hello on a transport that is connected and checked."""
        client = cls(transport, timeout)
        try:
            fields = {"api": API_VERSION, "client": clean_text(name)}
            if token is not None:
                fields["token"] = token
            # The token goes into this one request and is not kept: the
            # client has no attribute that holds it, and the names here are
            # dropped when this function ends.  Python cannot wipe a string,
            # so a copy may stay in memory until it is collected.
            reply = client._call("hello", fields, expect="hello")
            if reply["api"] != API_VERSION:
                raise ControlError("unsupported", "the gateway speaks another version of the API")
            client._hello = reply
        except BaseException:
            client._abort()
            raise
        finally:
            token = None
            fields = None
        return client

    # -- the connection ------------------------------------------------------

    def close(self):
        """Closes the connection.  Safe to call again."""
        self._abort()

    def __enter__(self):
        return self

    def __exit__(self, *_unused):
        self.close()

    def __repr__(self):
        return "<vss_control.Client %s>" % ("closed" if self._closed else "connected")

    def _abort(self):
        self._closed = True
        self._transport.close()

    @property
    def hello(self):
        """What the gateway said about itself: `api`, `version`, `principal`,
        `scopes` and `features`."""
        return dict(self._hello)

    # -- the exchange --------------------------------------------------------

    def _closed_error(self):
        return TransportError("connection_closed", "the connection is closed")

    def _timed_out(self):
        self._abort()
        return TransportError(
            "client_timeout", "no answer in time; the connection is closed", retryable=True
        )

    def _receive(self, deadline):
        """The next Reply or Batch.  Raises TimeoutError at the deadline,
        leaving the connection as it was; closes the connection and raises
        TransportError for anything else."""
        while True:
            try:
                frame = self._reader.next()
            except FrameError as error:
                self._abort()
                raise TransportError(
                    "protocol_error", "the gateway sent a frame that is not allowed (%s)" % error.code
                ) from None
            if frame is not None:
                try:
                    return decode_server_message(frame)
                except WireError as error:
                    self._abort()
                    raise TransportError(
                        "protocol_error", "the gateway sent a message that is not valid: %s" % error
                    ) from None
            try:
                data = self._transport.recv_some(_READ_SIZE, deadline)
            except TimeoutError:
                raise
            except OSError:
                self._abort()
                raise self._closed_error() from None
            if not data:
                self._abort()
                raise TransportError("connection_closed", "the gateway closed the connection")
            self._reader.feed(data)

    def _call(self, op, fields=None, blob=b"", wait=0.0, expect="done"):
        """Sends a request and waits for its answer, which must be a reply of
        the kind `expect`.  Returns the fields of the reply."""
        if self._closed:
            raise self._closed_error()
        request_id = self._last_id + 1
        try:
            data = encode_request(request_id, op, fields, blob)
        except FrameError:
            raise ValueError("the request is too large to send") from None
        self._last_id = request_id
        deadline = time.monotonic() + self._timeout + wait

        try:
            send_failed = False
            try:
                self._transport.send_all(data, deadline)
            except TimeoutError:
                raise self._timed_out() from None
            except OSError:
                # The gateway may have said why before it closed the
                # connection (`busy`), so what it sent is still read.
                send_failed = True
            data = None

            while True:
                try:
                    message = self._receive(deadline)
                except TimeoutError:
                    raise self._timed_out() from None
                except TransportError:
                    if send_failed:
                        raise self._closed_error() from None
                    raise
                if isinstance(message, Batch):
                    self._events.append(message.value)
                    continue
                refused = message.kind == "error"
                # Id 0 is an error about the connection itself.
                if message.id != request_id and not (message.id == 0 and refused):
                    continue
                if refused:
                    error = message.fields["error"]
                    if message.id == 0:
                        self._abort()
                    raise error_from_reply(
                        error["code"], error["message"], error["retryable"]
                    )
                if message.kind != expect:
                    self._abort()
                    raise TransportError("protocol_error", "the gateway sent the wrong kind of reply")
                return message.fields
        except ControlError:
            raise
        except BaseException:
            # An interrupt, or anything unexpected, leaves the connection in
            # a state nobody knows.
            self._abort()
            raise

    # -- operations ----------------------------------------------------------

    def status(self):
        """`uptime_ms`, `sessions` and `agent` (`running`, `version`)."""
        return self._call("status", expect="status")

    def list_ports(self):
        """`revision`, `ports` and `links`, as the gateway lists them."""
        return self._call("ports.list", expect="ports")

    def create_port(self, name, kind=None):
        """Makes a port in the agent and returns its id (`p_3`).  Needs
        `control.ports`.  `kind` is a kind the agent makes; None for a
        loopback port."""
        fields = {"name": clean_text(name), "kind": clean_text(kind or "")}
        return self._call("ports.create", fields, expect="created")["created"]

    def remove_port(self, port):
        """Removes the port with this id.  Needs `control.ports`."""
        self._call("ports.remove", {"port": clean_text(port)})

    def connect_ports(self, first, second, one_way=False):
        """Links two ports, both ways or from `first` to `second` only, and
        returns the id of the link (`l_1`).  Needs `control.ports`."""
        fields = {
            "first": clean_text(first),
            "second": clean_text(second),
            "one_way": bool(one_way),
        }
        return self._call("links.connect", fields, expect="created")["created"]

    def send(self, port, data):
        """Writes bytes to a port.  Needs `control.data`; the gateway takes
        64 KiB at most."""
        blob = _bytes_argument(data, "data")
        self._call("ports.send", {"port": clean_text(port)}, blob)

    def expect_bytes(
        self,
        port,
        pattern,
        timeout_ms=1000,
        send_port=None,
        send=b"",
        label=None,
        direction=None,
    ):
        """Waits, inside the gateway, until `pattern` (1 to 256 bytes) has
        gone by on `port`.  With `send_port` the gateway listens first and
        then sends `send` to that port, so nothing the bytes cause can be
        missed.  `direction` is `rx` (the usual) or `tx`.  `label` names the
        check in the report of a run.

        Returns `offset` (how many bytes went by before the pattern),
        `mono_ns` and `events_lost`.  Raises ControlError with code `timeout`
        when the pattern does not come in `timeout_ms`; the connection stays
        usable.  Needs `monitor.data`, and `control.data` to send.
        """
        pattern = _bytes_argument(pattern, "pattern")
        send = _bytes_argument(send, "send")
        if not 1 <= len(pattern) <= 256:
            raise ValueError("a pattern is 1 to 256 bytes")
        if send and not send_port:
            # The gateway ends a connection that sends this.
            raise ValueError("send needs a send_port")
        if direction not in (None, "", "rx", "tx"):
            raise ValueError("direction is rx or tx")
        timeout_ms = _number(timeout_ms, "timeout_ms", _U32_MAX)
        fields = {
            "port": clean_text(port),
            "pattern": pattern.hex(),
            "timeout_ms": timeout_ms,
        }
        if direction:
            fields["direction"] = direction
        if send_port:
            fields["send_port"] = clean_text(send_port)
        if label:
            fields["label"] = clean_text(label)
        # The call may take the time of the request on top of the usual one.
        return self._call(
            "expect.bytes", fields, send, wait=max(timeout_ms, 0) / 1000.0, expect="matched"
        )

    # -- streams -------------------------------------------------------------

    def subscribe_events(self, interval_ms=250, ports=None):
        """Starts the stream of what happens to the ports, without bytes, or
        changes its ports and its pace.  Returns `interval_ms` (the pace the
        gateway took) and `next_seq`.  One connection has one stream pace:
        the last subscription of either kind says it."""
        fields = {
            "interval_ms": _number(interval_ms, "interval_ms", _U32_MAX),
            "ports": _ports_argument(ports),
        }
        return self._call("events.subscribe", fields, expect="subscribed")

    def subscribe_traffic(self, interval_ms=250, ports=None):
        """As `subscribe_events`, with the bytes of the traffic.  Needs
        `monitor.data`."""
        fields = {
            "interval_ms": _number(interval_ms, "interval_ms", _U32_MAX),
            "ports": _ports_argument(ports),
        }
        return self._call("traffic.subscribe", fields, expect="subscribed")

    def unsubscribe_events(self):
        self._call("events.unsubscribe")

    def unsubscribe_traffic(self):
        self._call("traffic.unsubscribe")

    def next_events(self, timeout=1.0):
        """The next batch of events, waiting up to `timeout` seconds, or None
        when none came; the connection stays usable.  A batch is a dict with
        `seq`, `revision`, `dropped`, `mono_ns`, `wall_ms` and `events`; an
        event has `kind`, `port`, `name`, `direction`, `size`, `data` (bytes,
        empty without the traffic stream), `lines`, `message`, `error` and
        `mono_ns`.  With a timeout of 0 it takes what has already arrived.

        Batches that arrived while a request waited for its answer are kept,
        the newest 256; `seq` shows a gap.  When the gateway ends the session
        it says why first, and that is the error raised.
        """
        deadline = time.monotonic() + max(float(timeout), 0.0)
        while True:
            if self._events:
                return self._events.popleft()
            if self._closed:
                raise self._closed_error()
            try:
                message = self._receive(deadline)
            except TimeoutError:
                return None
            if isinstance(message, Batch):
                self._events.append(message.value)
                continue
            # No request is waiting, so an error with no id is the gateway
            # saying why it ends the session.  Anything else is the late
            # answer to a request that was given up on.
            if message.id == 0 and message.kind == "error":
                error = message.fields["error"]
                self._abort()
                raise error_from_reply(error["code"], error["message"], error["retryable"])

    # -- runs and reports ----------------------------------------------------

    def begin_run(self, name):
        """Begins a named run: while it is open the gateway notes how each
        `expect_bytes` ended and what `record_in_run` tells it.  Returns the
        run (`run` id, `name`, `open`, `tests`, `failures`, ...).  Raises
        `conflict` while a run of this principal is open."""
        return self._call("runs.begin", {"name": clean_text(name)}, expect="run")

    def record_in_run(self, name, outcome, message="", duration_ms=0):
        """Adds a check of your own to the open run.  `outcome` is `passed`,
        `failed`, `error` or `skipped`."""
        fields = {
            "name": clean_text(name),
            "outcome": clean_text(outcome),
            "message": clean_text(message),
            "duration_ms": _number(duration_ms, "duration_ms", _U64_MAX),
        }
        self._call("runs.record", fields)

    def end_run(self):
        """Ends the open run and returns its counts, as `begin_run` does.
        Raises `not_found` when none is open."""
        return self._call("runs.end", expect="run")

    def report_junit(self, run=None):
        """A run as a JUnit XML document (a str).  `run` is the id
        `begin_run` gave; None is the latest of this principal."""
        fields = {"run": clean_text(run or "")}
        return self._call("report.junit", fields, expect="report")["xml"]
