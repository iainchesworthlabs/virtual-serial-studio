# SPDX-License-Identifier: GPL-3.0-or-later
"""The client against a gateway that this file plays, over a socket pair, and
the transports' time limits.  No program of the project is started.

The gateway side is a plain socket: a test puts the bytes of the replies it
wants into it before the client asks, or has a timer put them in later.
"""

import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))

from vss_control import Client, ControlError, EngineError, TransportError  # noqa: E402
from vss_control import _transport  # noqa: E402
from vss_control._frame import FrameReader, encode_frame  # noqa: E402
from vss_control._transport import SocketTransport  # noqa: E402

HELLO = {
    "api": 1,
    "version": "test",
    "principal": "local:1000",
    "scopes": ["monitor", "monitor.data"],
    "features": ["expect.bytes"],
}


def frame(header, blob=b""):
    return encode_frame(json.dumps(header).encode("utf-8"), blob)


def reply(request_id, kind, **fields):
    header = {"id": request_id, "reply": kind}
    header.update(fields)
    return frame(header)


def error(request_id, code, message="refused", retryable=False):
    return reply(
        request_id,
        "error",
        error={"code": code, "message": message, "retryable": retryable},
    )


def batch(seq, events=(), blob=b""):
    return frame(
        {
            "event": "batch",
            "seq": seq,
            "revision": 1,
            "dropped": 0,
            "time": {"mono_ns": 1, "wall_ms": 2},
            "events": list(events),
        },
        blob,
    )


class Setup(unittest.TestCase):
    """A client at one end of a socket pair and the test at the other."""

    def setUp(self):
        self.gateway, near = socket.socketpair()
        self.gateway.settimeout(5)
        self.addCleanup(self.gateway.close)
        self.reader = FrameReader()
        self.near = near
        self.transport = SocketTransport(near)

    def greet(self, timeout=5.0, **more):
        self.gateway.sendall(reply(1, "hello", **HELLO))
        client = Client._greet(self.transport, timeout, "test", more.get("token"))
        self.addCleanup(client.close)
        self.hello_header, _ = self.read_request()
        return client

    def read_request(self):
        """The next request the client sent, as (header, blob)."""
        while True:
            found = self.reader.next()
            if found is not None:
                return json.loads(found.header), found.blob
            data = self.gateway.recv(65536)
            if not data:
                raise AssertionError("the client closed the connection")
            self.reader.feed(data)

    def assert_closed_by_client(self):
        self.gateway.settimeout(5)
        while True:
            data = self.gateway.recv(65536)
            if not data:
                return


class ConversationTests(Setup):
    def test_hello_and_request_ids_count_up(self):
        client = self.greet()
        self.assertEqual(client.hello["principal"], "local:1000")
        self.assertEqual(client.hello["features"], ["expect.bytes"])
        self.gateway.sendall(
            reply(2, "status", uptime_ms=5, sessions=1, agent={"running": True, "version": "t"})
        )
        self.gateway.sendall(reply(3, "done"))
        self.assertEqual(client.status()["agent"]["running"], True)
        client.remove_port("p_1")
        first, _ = self.read_request()
        second, _ = self.read_request()
        self.assertEqual((first["id"], first["op"]), (2, "status"))
        self.assertEqual((second["id"], second["op"], second["port"]), (3, "ports.remove", "p_1"))

    def test_the_hello_says_what_this_program_is_and_carries_the_token(self):
        self.gateway.sendall(reply(1, "hello", **HELLO))
        client = Client._greet(self.transport, 5.0, "my program", "vsst1_example")
        self.addCleanup(client.close)
        header, _ = self.read_request()
        self.assertEqual(header, {"id": 1, "op": "hello", "api": 1, "client": "my program", "token": "vsst1_example"})
        # Nothing the client holds is the token.
        self.assertNotIn("vsst1_example", repr(vars(client)))
        self.assertNotIn("vsst1_example", repr(client))

    def test_no_token_means_no_token_field(self):
        self.greet()
        self.assertEqual(self.hello_header, {"id": 1, "op": "hello", "api": 1, "client": "test"})

    def test_a_refused_hello_raises_and_closes(self):
        token = "vsst1_aaaaaaaaaaaaa_" + "b" * 52
        self.gateway.sendall(error(1, "unauthenticated", "not authenticated"))
        with self.assertRaises(ControlError) as caught:
            Client._greet(self.transport, 5.0, "test", token)
        self.assertEqual(caught.exception.code, "unauthenticated")
        self.assertNotIn(token, str(caught.exception))
        self.assertNotIn(token, repr(caught.exception))
        self.assertNotIn(token, repr(caught.exception.__dict__))
        self.assert_closed_by_client()

    def test_another_version_of_the_api_is_refused(self):
        self.gateway.sendall(reply(1, "hello", **dict(HELLO, api=2)))
        with self.assertRaises(ControlError) as caught:
            Client._greet(self.transport, 5.0, "test", None)
        self.assertEqual(caught.exception.code, "unsupported")
        self.assert_closed_by_client()

    def test_an_error_reply_keeps_the_connection(self):
        client = self.greet()
        self.gateway.sendall(error(2, "forbidden", "not allowed"))
        self.gateway.sendall(error(3, "engine.not_found", "the engine refused the request"))
        self.gateway.sendall(reply(4, "done"))
        with self.assertRaises(ControlError) as caught:
            client.create_port("a")
        self.assertEqual(
            (type(caught.exception), caught.exception.code, caught.exception.retryable),
            (ControlError, "forbidden", False),
        )
        with self.assertRaises(EngineError) as caught:
            client.remove_port("p_9")
        self.assertEqual(caught.exception.code, "engine.not_found")
        client.remove_port("p_1")

    def test_a_reply_for_another_request_is_skipped(self):
        client = self.greet()
        self.gateway.sendall(reply(77, "done"))
        self.gateway.sendall(reply(2, "created", created="p_5"))
        self.assertEqual(client.create_port("a"), "p_5")

    def test_the_wrong_kind_of_reply_is_a_protocol_error(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "done"))
        with self.assertRaises(TransportError) as caught:
            client.create_port("a")
        self.assertEqual(caught.exception.code, "protocol_error")
        self.assert_closed_by_client()

    def test_an_error_without_an_id_ends_the_session(self):
        client = self.greet()
        self.gateway.sendall(error(0, "busy", "too many sessions", True))
        with self.assertRaises(ControlError) as caught:
            client.status()
        self.assertEqual((caught.exception.code, caught.exception.retryable), ("busy", True))
        with self.assertRaises(TransportError) as again:
            client.status()
        self.assertEqual(again.exception.code, "connection_closed")
        self.assert_closed_by_client()

    def test_the_gateway_closing_is_connection_closed(self):
        client = self.greet()
        self.gateway.close()
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "connection_closed")
        with self.assertRaises(TransportError):
            client.next_events(0)

    def test_closing_twice_is_fine_and_a_closed_client_says_so(self):
        client = self.greet()
        client.close()
        client.close()
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "connection_closed")

    def test_the_context_manager_closes(self):
        client = self.greet()
        with client:
            pass
        self.assertIn("closed", repr(client))
        self.assert_closed_by_client()


class FramingFailureTests(Setup):
    def test_an_oversized_length_closes_the_connection(self):
        client = self.greet()
        self.gateway.sendall((1 << 30).to_bytes(4, "little"))
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "protocol_error")
        self.assertIn("oversized_frame", str(caught.exception))
        self.assert_closed_by_client()

    def test_a_bad_header_size_closes_the_connection(self):
        client = self.greet()
        self.gateway.sendall((8).to_bytes(4, "little") + (99).to_bytes(4, "little"))
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "protocol_error")
        self.assert_closed_by_client()

    def junk(self, payload):
        client = self.greet()
        self.gateway.sendall(encode_frame(payload))
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "protocol_error")
        self.assert_closed_by_client()

    def test_a_header_that_is_not_json_closes_the_connection(self):
        self.junk(b"not json")

    def test_a_reply_without_a_kind_closes_the_connection(self):
        self.junk(b'{"id":2}')

    def test_text_after_the_json_closes_the_connection(self):
        self.junk(b'{"id":2,"reply":"done"} x')


    def test_a_reply_with_a_blob_closes_the_connection(self):
        client = self.greet()
        self.gateway.sendall(frame({"id": 2, "reply": "done"}, b"bytes"))
        with self.assertRaises(TransportError) as caught:
            client.remove_port("p_1")
        self.assertEqual(caught.exception.code, "protocol_error")


class TimeoutTests(Setup):
    def test_a_call_that_is_not_answered_closes_the_connection(self):
        client = self.greet(timeout=0.3)
        started = time.monotonic()
        with self.assertRaises(TransportError) as caught:
            client.status()
        elapsed = time.monotonic() - started
        self.assertEqual(caught.exception.code, "client_timeout")
        self.assertGreaterEqual(elapsed, 0.25)
        self.assertLess(elapsed, 5)
        self.read_request()
        self.assert_closed_by_client()
        with self.assertRaises(TransportError) as again:
            client.status()
        self.assertEqual(again.exception.code, "connection_closed")

    def test_an_expect_may_take_its_own_time_on_top(self):
        client = self.greet(timeout=0.2)
        timer = threading.Timer(
            0.6, self.gateway.sendall, [reply(2, "matched", offset=3, mono_ns=9, events_lost=0)]
        )
        timer.start()
        self.addCleanup(timer.join)
        found = client.expect_bytes(port="p_2", pattern=b"OK", timeout_ms=1000)
        self.assertEqual(found, {"offset": 3, "mono_ns": 9, "events_lost": 0})

    def test_next_events_that_finds_nothing_leaves_the_connection_usable(self):
        client = self.greet()
        started = time.monotonic()
        self.assertIsNone(client.next_events(0.2))
        self.assertGreaterEqual(time.monotonic() - started, 0.15)
        self.assertIsNone(client.next_events(0))
        self.gateway.sendall(reply(2, "done"))
        client.remove_port("p_1")

    def test_a_batch_that_arrives_in_pieces_is_not_lost_to_a_timeout(self):
        client = self.greet()
        whole = batch(7, [{"kind": "data", "port": "p_1", "size": 3, "stored": 3}], b"abc")
        self.gateway.sendall(whole[:5])
        self.assertIsNone(client.next_events(0.1))
        self.gateway.sendall(whole[5:20])
        self.assertIsNone(client.next_events(0.1))
        self.gateway.sendall(whole[20:])
        found = client.next_events(5)
        self.assertEqual(found["seq"], 7)
        self.assertEqual(found["events"][0]["data"], b"abc")
        self.gateway.sendall(reply(2, "done"))
        client.remove_port("p_1")


class EventTests(Setup):
    def test_batches_that_arrive_during_a_call_are_kept_for_next_events(self):
        client = self.greet()
        event = {"kind": "data", "port": "p_2", "direction": "rx", "size": 2, "stored": 2}
        self.gateway.sendall(batch(1, [event], b"hi"))
        self.gateway.sendall(batch(2))
        self.gateway.sendall(reply(2, "subscribed", interval_ms=20, next_seq=3))
        started = client.subscribe_traffic(interval_ms=20, ports=["p_2"])
        self.assertEqual(started, {"interval_ms": 20, "next_seq": 3})
        header, _ = self.read_request()
        self.assertEqual(
            (header["op"], header["interval_ms"], header["ports"]),
            ("traffic.subscribe", 20, ["p_2"]),
        )
        first = client.next_events(0)
        self.assertEqual((first["seq"], first["events"][0]["data"]), (1, b"hi"))
        self.assertEqual(client.next_events(0)["seq"], 2)
        self.assertIsNone(client.next_events(0))

    def test_only_the_newest_256_batches_are_kept(self):
        client = self.greet()
        for seq in range(1, 301):
            self.gateway.sendall(batch(seq))
        self.gateway.sendall(reply(2, "done"))
        client.unsubscribe_events()
        seen = []
        while True:
            found = client.next_events(0)
            if found is None:
                break
            seen.append(found["seq"])
        self.assertEqual(seen, list(range(45, 301)))

    def test_the_end_of_the_session_is_raised_by_next_events(self):
        client = self.greet()
        self.gateway.sendall(error(0, "unauthenticated", "token revoked"))
        with self.assertRaises(ControlError) as caught:
            client.next_events(5)
        self.assertEqual(caught.exception.code, "unauthenticated")
        with self.assertRaises(TransportError):
            client.next_events(0)

    def test_a_late_answer_is_ignored_by_next_events(self):
        client = self.greet()
        self.gateway.sendall(reply(55, "done"))
        self.gateway.sendall(batch(1))
        self.assertEqual(client.next_events(5)["seq"], 1)


class RequestTests(Setup):
    def test_expect_bytes_puts_the_pattern_in_the_header_and_the_send_in_the_blob(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "matched", offset=6, mono_ns=1, events_lost=0))
        client.expect_bytes(
            port="p_2",
            pattern=b"OK\r\n",
            timeout_ms=2000,
            send_port="p_1",
            send=b"AT\r",
            label="at",
            direction="tx",
        )
        header, blob = self.read_request()
        self.assertEqual(
            header,
            {
                "id": 2,
                "op": "expect.bytes",
                "port": "p_2",
                "pattern": "4f4b0d0a",
                "timeout_ms": 2000,
                "send_port": "p_1",
                "label": "at",
                "direction": "tx",
            },
        )
        self.assertEqual(blob, b"AT\r")

    def test_expect_bytes_leaves_out_what_is_not_given(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "matched", offset=0, mono_ns=1, events_lost=0))
        client.expect_bytes("p_2", b"\x00")
        header, blob = self.read_request()
        self.assertEqual(header, {"id": 2, "op": "expect.bytes", "port": "p_2", "pattern": "00", "timeout_ms": 1000})
        self.assertEqual(blob, b"")

    def test_send_puts_the_bytes_in_the_blob(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "done"))
        client.send("p_1", bytearray(b"\x00\xff\n"))
        header, blob = self.read_request()
        self.assertEqual((header["op"], header["port"]), ("ports.send", "p_1"))
        self.assertEqual(blob, b"\x00\xff\n")

    def test_the_other_requests(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "created", created="p_1"))
        self.gateway.sendall(reply(3, "created", created="l_1"))
        self.gateway.sendall(
            reply(4, "run", run="r_1", name="n", open=True, tests=0, failures=0, errors=0, skipped=0, duration_ms=0)
        )
        self.gateway.sendall(reply(5, "done"))
        self.gateway.sendall(
            reply(6, "run", run="r_1", name="n", open=False, tests=1, failures=0, errors=0, skipped=0, duration_ms=3)
        )
        self.gateway.sendall(reply(7, "report", xml="<testsuites/>"))
        self.assertEqual(client.create_port("a", kind="loopback"), "p_1")
        self.assertEqual(client.connect_ports("p_1", "p_2", one_way=True), "l_1")
        self.assertTrue(client.begin_run("n")["open"])
        client.record_in_run("check", "passed", message="m", duration_ms=4)
        self.assertEqual(client.end_run()["tests"], 1)
        self.assertEqual(client.report_junit(), "<testsuites/>")
        sent = [self.read_request()[0] for _ in range(6)]
        self.assertEqual(sent[0], {"id": 2, "op": "ports.create", "name": "a", "kind": "loopback"})
        self.assertEqual(sent[1], {"id": 3, "op": "links.connect", "first": "p_1", "second": "p_2", "one_way": True})
        self.assertEqual(sent[2], {"id": 4, "op": "runs.begin", "name": "n"})
        self.assertEqual(
            sent[3],
            {"id": 5, "op": "runs.record", "name": "check", "outcome": "passed", "message": "m", "duration_ms": 4},
        )
        self.assertEqual(sent[4], {"id": 6, "op": "runs.end"})
        self.assertEqual(sent[5], {"id": 7, "op": "report.junit", "run": ""})

    def test_arguments_that_would_end_the_connection_are_refused_here(self):
        client = self.greet()
        problems = [
            (ValueError, lambda: client.expect_bytes("p_1", b"")),
            (ValueError, lambda: client.expect_bytes("p_1", b"x" * 257)),
            (ValueError, lambda: client.expect_bytes("p_1", b"x", send=b"y")),
            (ValueError, lambda: client.expect_bytes("p_1", b"x", direction="up")),
            (ValueError, lambda: client.expect_bytes("p_1", b"x", timeout_ms=-1)),
            (ValueError, lambda: client.expect_bytes("p_1", b"x", timeout_ms=1 << 32)),
            (TypeError, lambda: client.expect_bytes("p_1", "text")),
            (TypeError, lambda: client.send("p_1", "text")),
            (TypeError, lambda: client.send("p_1", 5)),
            (ValueError, lambda: client.send("p_1", b"x" * (1 << 20))),
            (ValueError, lambda: client.subscribe_events(ports=["p_%d" % n for n in range(65)])),
            (TypeError, lambda: client.subscribe_events(ports="p_1")),
            (ValueError, lambda: client.subscribe_traffic(interval_ms=-5)),
            (ValueError, lambda: client.record_in_run("n", "passed", duration_ms=-1)),
            (TypeError, lambda: client.create_port(5)),
        ]
        for kind, call in problems:
            with self.assertRaises(kind):
                call()
        # Nothing was sent, and the connection is as it was.
        self.gateway.sendall(reply(client._last_id + 1, "done"))
        client.remove_port("p_1")
        header, _ = self.read_request()
        self.assertEqual(header["op"], "ports.remove")

    def test_a_text_that_is_too_long_is_cut_not_refused(self):
        client = self.greet()
        self.gateway.sendall(reply(2, "created", created="p_1"))
        client.create_port("n" * 5000)
        header, _ = self.read_request()
        self.assertEqual(len(header["name"]), 1024)


class ConnectArgumentTests(unittest.TestCase):
    def test_bad_arguments_are_refused_before_anything_is_opened(self):
        for arguments in (
            {"instance": "a b"},
            {"instance": "x" * 33},
            {"token": "vsst1_zzz", "token_file": "f"},
            {"timeout": 0},
            {"timeout": -1},
            {"token": ""},
            {"token": "x" * 200},
        ):
            with self.assertRaises(ValueError, msg=arguments) as caught:
                Client.connect(**arguments)
            token = arguments.get("token")
            if token:
                self.assertNotIn(token, str(caught.exception))


class SocketTransportTests(unittest.TestCase):
    def setUp(self):
        self.far, near = socket.socketpair()
        self.addCleanup(self.far.close)
        self.transport = SocketTransport(near)
        self.addCleanup(self.transport.close)

    def test_a_read_times_out_and_can_be_tried_again(self):
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.transport.recv_some(100, time.monotonic() + 0.2)
        self.assertGreaterEqual(time.monotonic() - started, 0.15)
        self.assertLess(time.monotonic() - started, 5)
        self.far.sendall(b"hello")
        self.assertEqual(self.transport.recv_some(100, time.monotonic() + 5), b"hello")

    def test_what_has_arrived_is_taken_even_when_time_is_up(self):
        self.far.sendall(b"abc")
        # Data crosses the pair a moment later on some systems.
        deadline = time.monotonic() + 5
        data = b""
        while not data and time.monotonic() < deadline:
            try:
                data = self.transport.recv_some(10, time.monotonic() - 1)
            except TimeoutError:
                pass
        self.assertEqual(data, b"abc")

    def test_end_of_stream_is_an_empty_read(self):
        self.far.close()
        self.assertEqual(self.transport.recv_some(10, time.monotonic() + 5), b"")

    def test_a_write_that_cannot_finish_times_out(self):
        started = time.monotonic()
        deadline = started + 0.5
        with self.assertRaises(TimeoutError):
            # More than any socket buffer holds, and nobody reads.  Several
            # writes: one system accepts a single large write whole.
            for _ in range(500):
                self.transport.send_all(b"x" * (1 << 20), deadline)
        self.assertGreaterEqual(time.monotonic() - started, 0.4)
        self.assertLess(time.monotonic() - started, 10)

    def test_a_write_arrives_whole(self):
        self.transport.send_all(b"abc" * 1000, time.monotonic() + 5)
        data = b""
        while len(data) < 3000:
            data += self.far.recv(4096)
        self.assertEqual(data, b"abc" * 1000)


@unittest.skipUnless(os.name == "nt", "named pipes are Windows only")
class PipeTransportTests(unittest.TestCase):
    """The overlapped calls, against a pipe made here."""

    def setUp(self):
        import ctypes
        import secrets
        from ctypes import wintypes

        from vss_control import _win32

        self.ctypes = ctypes
        self.w = _win32
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateNamedPipeW.restype = ctypes.c_void_p
        kernel.CreateNamedPipeW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
        ]
        kernel.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        kernel.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel = kernel
        self.wintypes = wintypes
        self.name = "\\\\.\\pipe\\vsspy-test-%s" % secrets.token_hex(6)
        # Small buffers, so that a write that nobody reads cannot finish.
        server = kernel.CreateNamedPipeW(self.name, 3, 0, 1, 4096, 4096, 0, None)
        self.assertTrue(server, "CreateNamedPipeW failed")
        self.server = server
        client = _transport.open_pipe_handle(self.name, time.monotonic() + 5)
        self.transport = _transport.PipeTransport(client)
        self.addCleanup(self.transport.close)
        self.addCleanup(self.close_server)

    def close_server(self):
        if self.server:
            self.kernel.CloseHandle(self.server)
            self.server = None

    def server_write(self, data):
        done = self.wintypes.DWORD(0)
        ok = self.kernel.WriteFile(self.server, data, len(data), self.ctypes.byref(done), None)
        self.assertTrue(ok)

    def server_read(self, size):
        buffer = self.ctypes.create_string_buffer(size)
        done = self.wintypes.DWORD(0)
        ok = self.kernel.ReadFile(self.server, buffer, size, self.ctypes.byref(done), None)
        self.assertTrue(ok)
        return buffer.raw[: done.value]

    def test_a_read_times_out_and_can_be_tried_again(self):
        for _ in range(3):
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                self.transport.recv_some(100, time.monotonic() + 0.2)
            elapsed = time.monotonic() - started
            self.assertGreaterEqual(elapsed, 0.15)
            self.assertLess(elapsed, 5)
        self.server_write(b"hello")
        self.assertEqual(self.transport.recv_some(100, time.monotonic() + 5), b"hello")
        self.server_write(b"again")
        self.assertEqual(self.transport.recv_some(100, time.monotonic() + 5), b"again")

    def test_what_has_arrived_is_taken_even_when_time_is_up(self):
        self.server_write(b"abc")
        self.assertEqual(self.transport.recv_some(10, time.monotonic() - 1), b"abc")

    def test_a_read_without_time_returns_at_once(self):
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.transport.recv_some(10, time.monotonic())
        self.assertLess(time.monotonic() - started, 2)

    def test_end_of_stream_is_an_empty_read(self):
        self.close_server()
        self.assertEqual(self.transport.recv_some(10, time.monotonic() + 5), b"")

    def test_a_write_arrives_whole(self):
        self.transport.send_all(b"hello", time.monotonic() + 5)
        self.assertEqual(self.server_read(100), b"hello")

    def test_a_write_that_nobody_reads_times_out(self):
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.transport.send_all(b"x" * (1 << 20), time.monotonic() + 0.3)
        elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 0.25)
        self.assertLess(elapsed, 5)

    def test_a_write_to_a_pipe_that_is_gone_is_an_error(self):
        self.close_server()
        with self.assertRaises(OSError):
            self.transport.send_all(b"hello", time.monotonic() + 5)

    def test_closing_twice_is_fine(self):
        self.transport.close()
        self.transport.close()

    def test_a_pipe_that_does_not_exist_is_not_running(self):
        with self.assertRaises(TransportError) as caught:
            _transport.open_pipe_handle("\\\\.\\pipe\\vsspy-nothing-here", time.monotonic() + 1)
        self.assertEqual(caught.exception.code, "not_running")


if __name__ == "__main__":
    unittest.main()
