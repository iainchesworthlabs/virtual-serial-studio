# SPDX-License-Identifier: GPL-3.0-or-later
"""Framing, messages, addresses and errors.  No process is started."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))

from vss_control import _address, _wire  # noqa: E402
from vss_control._client import _check_token, _read_token_file  # noqa: E402
from vss_control._errors import ControlError, EngineError, error_from_reply  # noqa: E402
from vss_control._frame import (  # noqa: E402
    MAX_FRAME_SIZE,
    Frame,
    FrameError,
    FrameReader,
    encode_frame,
)


def u32(value):
    return value.to_bytes(4, "little")


class EncodeFrameTests(unittest.TestCase):
    def test_layout(self):
        data = encode_frame(b'{"a":1}', b"xyz")
        self.assertEqual(data[:4], u32(4 + 7 + 3))
        self.assertEqual(data[4:8], u32(7))
        self.assertEqual(data[8:], b'{"a":1}xyz')

    def test_empty_header_is_malformed(self):
        for header in (b"", b"{"):
            with self.assertRaises(FrameError) as caught:
                encode_frame(header)
            self.assertEqual(caught.exception.code, "malformed_frame")

    def test_frame_over_the_limit_is_oversized(self):
        # The size after the prefix counts the header size field too.
        encode_frame(b"{}", b"x" * (MAX_FRAME_SIZE - 4 - 2))
        with self.assertRaises(FrameError) as caught:
            encode_frame(b"{}", b"x" * (MAX_FRAME_SIZE - 4 - 2 + 1))
        self.assertEqual(caught.exception.code, "oversized_frame")


class FrameReaderTests(unittest.TestCase):
    def test_round_trip_in_one_piece(self):
        reader = FrameReader()
        reader.feed(encode_frame(b'{"id":1}', b"\x00\xff\n"))
        self.assertEqual(reader.next(), Frame(b'{"id":1}', b"\x00\xff\n"))
        self.assertIsNone(reader.next())
        self.assertEqual(reader.buffered, 0)

    def test_round_trip_one_byte_at_a_time(self):
        data = encode_frame(b'{"a":"b"}', b"blob") + encode_frame(b"{}")
        reader = FrameReader()
        seen = []
        for index in range(len(data)):
            reader.feed(data[index : index + 1])
            frame = reader.next()
            if frame is not None:
                seen.append(frame)
        self.assertEqual(seen, [Frame(b'{"a":"b"}', b"blob"), Frame(b"{}", b"")])

    def test_two_frames_in_one_read(self):
        reader = FrameReader()
        reader.feed(encode_frame(b"{}", b"1") + encode_frame(b"{}", b"2"))
        self.assertEqual(reader.next().blob, b"1")
        self.assertEqual(reader.next().blob, b"2")
        self.assertIsNone(reader.next())

    def test_oversized_length_is_refused_from_the_prefix_alone(self):
        reader = FrameReader()
        # Four bytes that say a gigabyte: nothing more is sent, and nothing
        # is allocated for it.
        reader.feed(u32(1 << 30))
        with self.assertRaises(FrameError) as caught:
            reader.next()
        self.assertEqual(caught.exception.code, "oversized_frame")
        self.assertLessEqual(reader.buffered, 4)

    def test_the_limit_can_be_lowered_and_never_raised(self):
        reader = FrameReader(limit=4096)
        reader.feed(u32(4097))
        with self.assertRaises(FrameError):
            reader.next()
        reader = FrameReader(limit=1 << 30)
        reader.feed(u32(MAX_FRAME_SIZE + 1))
        with self.assertRaises(FrameError):
            reader.next()
        reader = FrameReader()
        reader.set_limit(100)
        reader.feed(encode_frame(b"{}", b"x" * 200))
        with self.assertRaises(FrameError):
            reader.next()

    def test_a_frame_at_the_limit_is_taken(self):
        reader = FrameReader()
        reader.feed(encode_frame(b"{}", b"x" * (MAX_FRAME_SIZE - 6)))
        self.assertEqual(len(reader.next().blob), MAX_FRAME_SIZE - 6)

    def test_too_short_a_frame_is_malformed(self):
        reader = FrameReader()
        reader.feed(u32(5))
        with self.assertRaises(FrameError) as caught:
            reader.next()
        self.assertEqual(caught.exception.code, "malformed_frame")

    def test_bad_header_sizes_are_refused_before_the_body_arrives(self):
        for header_size in (0, 1, 9):
            reader = FrameReader()
            # A body of 8 bytes is announced; only the two sizes are sent.
            reader.feed(u32(8) + u32(header_size))
            with self.assertRaises(FrameError) as caught:
                reader.next()
            self.assertEqual(caught.exception.code, "malformed_frame", header_size)

    def test_an_error_is_final(self):
        reader = FrameReader()
        reader.feed(u32(1 << 30))
        with self.assertRaises(FrameError):
            reader.next()
        reader.feed(encode_frame(b"{}"))
        with self.assertRaises(FrameError):
            reader.next()
        self.assertLessEqual(reader.buffered, 4)

    def test_a_long_connection_does_not_grow_the_buffer(self):
        reader = FrameReader()
        for _ in range(1000):
            reader.feed(encode_frame(b"{}", b"x" * 100) + encode_frame(b"{}")[:3])
            self.assertEqual(len(reader.next().blob), 100)
            reader.feed(encode_frame(b"{}")[3:])
            self.assertEqual(reader.next().blob, b"")
        self.assertEqual(reader.buffered, 0)


def frame_of(header, blob=b""):
    return Frame(json.dumps(header).encode("utf-8"), blob)


class WireTests(unittest.TestCase):
    def test_request_header(self):
        data = _wire.encode_request(7, "ports.send", {"port": "p_1"}, b"\x01\x02")
        reader = FrameReader()
        reader.feed(data)
        frame = reader.next()
        self.assertEqual(json.loads(frame.header), {"id": 7, "op": "ports.send", "port": "p_1"})
        self.assertEqual(frame.blob, b"\x01\x02")

    def test_a_request_header_is_ascii(self):
        data = _wire.encode_request(1, "ports.create", {"name": "café ש", "kind": ""})
        header = FrameReader()
        header.feed(data)
        text = header.next().header
        self.assertEqual(text, text.decode("ascii").encode("ascii"))
        self.assertEqual(json.loads(text)["name"], "café ש")

    def test_a_hello_fits_the_frame_the_gateway_takes_before_hello(self):
        data = _wire.encode_request(1, "hello", {"api": 1, "client": "x" * 1024, "token": "t" * 128})
        self.assertLessEqual(len(data) - 4, 4096)

    def test_clean_text(self):
        self.assertEqual(_wire.clean_text("abc"), "abc")
        self.assertEqual(_wire.clean_text("a\ud800b"), "a?b")
        long = "é" * 1000
        cut = _wire.clean_text(long)
        self.assertLessEqual(len(cut.encode("utf-8")), 1024)
        self.assertEqual(cut, "é" * 512)
        with self.assertRaises(TypeError):
            _wire.clean_text(b"bytes")

    def test_reply(self):
        reply = _wire.decode_server_message(
            frame_of({"id": 3, "reply": "created", "created": "p_2", "extra": [1]})
        )
        self.assertEqual(reply.id, 3)
        self.assertEqual(reply.kind, "created")
        self.assertEqual(reply.fields, {"created": "p_2", "extra": [1]})

    def test_error_reply(self):
        reply = _wire.decode_server_message(
            frame_of(
                {
                    "id": 0,
                    "reply": "error",
                    "error": {"code": "busy", "message": "too many", "retryable": True},
                }
            )
        )
        self.assertEqual(reply.id, 0)
        self.assertEqual(reply.fields["error"]["code"], "busy")

    def test_bad_replies(self):
        bad = [
            Frame(b"not json", b""),
            Frame(b"[]", b""),
            Frame(b"\xff\xfe", b""),
            Frame(b'{"id":1,"reply":"nothing"}', b""),
            Frame(b'{"id":1,"reply":"done"}', b"blob"),
            Frame(b'{"reply":"done"}', b""),
            Frame(b'{"id":-1,"reply":"done"}', b""),
            Frame(b'{"id":true,"reply":"done"}', b""),
            Frame(b'{"id":1,"reply":"created"}', b""),
            Frame(b'{"id":1,"reply":"created","created":5}', b""),
            Frame(b'{"id":1,"reply":"matched","offset":1.5,"mono_ns":1,"events_lost":0}', b""),
            Frame(b'{"id":1,"reply":"done","x":NaN}', b""),
            Frame(b'{"id":1,"reply":"error","error":{"code":"x"}}', b""),
            Frame(b"[" * 100000, b""),
        ]
        for frame in bad:
            with self.assertRaises(_wire.WireError, msg=frame.header[:40]):
                _wire.decode_server_message(frame)

    def test_batch_takes_its_bytes_in_order(self):
        batch = _wire.decode_server_message(
            frame_of(
                {
                    "event": "batch",
                    "seq": 4,
                    "revision": 2,
                    "dropped": 1,
                    "time": {"mono_ns": 10, "wall_ms": 20},
                    "events": [
                        {"kind": "data", "port": "p_1", "direction": "rx", "size": 3, "stored": 3},
                        {"kind": "started", "port": "p_1", "future": True},
                        {"kind": "data", "port": "p_2", "size": 2, "stored": 2, "mono_ns": 5},
                    ],
                },
                b"abcde",
            )
        ).value
        self.assertEqual((batch["seq"], batch["revision"], batch["dropped"]), (4, 2, 1))
        self.assertEqual((batch["mono_ns"], batch["wall_ms"]), (10, 20))
        first, second, third = batch["events"]
        self.assertEqual(first["data"], b"abc")
        self.assertEqual(first["direction"], "rx")
        self.assertEqual(second["data"], b"")
        self.assertEqual(second["lines"], 0)
        self.assertEqual(third["data"], b"de")
        self.assertEqual(third["mono_ns"], 5)

    def test_batch_that_does_not_fit_its_blob_is_refused(self):
        def batch(events, blob):
            return frame_of(
                {
                    "event": "batch",
                    "seq": 1,
                    "revision": 1,
                    "dropped": 0,
                    "time": {"mono_ns": 1, "wall_ms": 1},
                    "events": events,
                },
                blob,
            )

        too_much = [{"kind": "data", "stored": 4}]
        with self.assertRaises(_wire.WireError):
            _wire.decode_server_message(batch(too_much, b"abc"))
        with self.assertRaises(_wire.WireError):
            # Bytes that no event names.
            _wire.decode_server_message(batch([{"kind": "data", "stored": 2}], b"abc"))
        with self.assertRaises(_wire.WireError):
            _wire.decode_server_message(batch([{"stored": 0}], b""))
        with self.assertRaises(_wire.WireError):
            _wire.decode_server_message(frame_of({"event": "other"}))


class AddressTests(unittest.TestCase):
    def test_instance_names(self):
        for good in ("", "a", "A_b-9", "x" * 32):
            self.assertTrue(_address.valid_instance_name(good), good)
        for bad in ("x" * 33, "a b", "a/b", "a.b", "a\\b", "é", None, 5):
            self.assertFalse(_address.valid_instance_name(bad), bad)

    def test_pipe_names(self):
        self.assertEqual(
            _address.windows_pipe_name("S-1-5-21-1-2-3-1001"),
            "\\\\.\\pipe\\vss-control-S-1-5-21-1-2-3-1001",
        )
        self.assertEqual(
            _address.windows_pipe_name("S-1-5-21-1-2-3-1001", "t-1"),
            "\\\\.\\pipe\\vss-control-S-1-5-21-1-2-3-1001-t-1",
        )

    def test_socket_paths(self):
        join = os.path.join
        runtime = {"XDG_RUNTIME_DIR": "/run/user/1000", "TMPDIR": "/ignored"}
        self.assertEqual(
            _address.unix_socket_path("", runtime, 1000),
            join("/run/user/1000", "virtual-serial-studio", "control.sock"),
        )
        self.assertEqual(
            _address.unix_socket_path("t1", runtime, 1000),
            join("/run/user/1000", "virtual-serial-studio", "control-t1.sock"),
        )
        # An empty XDG_RUNTIME_DIR counts as not set.
        for environ in ({"TMPDIR": "/var/tmp"}, {"XDG_RUNTIME_DIR": "", "TMPDIR": "/var/tmp"}):
            self.assertEqual(
                _address.unix_socket_path("t1", environ, 501),
                join("/var/tmp", "virtual-serial-studio-501", "control-t1.sock"),
            )
        self.assertEqual(
            _address.unix_socket_path("", {}, 7),
            join("/tmp", "virtual-serial-studio-7", "control.sock"),
        )


class ErrorTests(unittest.TestCase):
    def test_engine_codes_get_their_own_class(self):
        engine = error_from_reply("engine.not_found", "the engine refused the request", False)
        self.assertIsInstance(engine, EngineError)
        self.assertIsInstance(engine, ControlError)
        plain = error_from_reply("forbidden", "not allowed", False)
        self.assertNotIsInstance(plain, EngineError)
        self.assertEqual((plain.code, plain.message, plain.retryable), ("forbidden", "not allowed", False))
        self.assertEqual(str(plain), "forbidden: not allowed")
        self.assertIn("forbidden", repr(plain))

    def test_copies_keep_the_fields(self):
        import copy
        import pickle

        original = error_from_reply("timeout", "timed out", True)
        for clone in (copy.copy(original), pickle.loads(pickle.dumps(original))):
            self.assertEqual((clone.code, clone.message, clone.retryable), ("timeout", "timed out", True))


class TokenInputTests(unittest.TestCase):
    def setUp(self):
        import tempfile

        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name

    def write(self, data):
        path = os.path.join(self.directory, "t.token")
        with open(path, "wb") as stream:
            stream.write(data)
        return path

    def test_a_token_file_gives_its_first_line(self):
        self.assertEqual(_read_token_file(self.write(b"vsst1_abc\n")), "vsst1_abc")
        self.assertEqual(_read_token_file(self.write(b"vsst1_abc\r\n")), "vsst1_abc")
        self.assertEqual(_read_token_file(self.write(b"vsst1_abc")), "vsst1_abc")
        self.assertEqual(_read_token_file(self.write(b"vsst1_abc\nsecond line\n")), "vsst1_abc")

    def test_a_file_that_is_no_token_file(self):
        token = "vsst1_" + "q" * 60
        for data in (b"x" * 300, b"\xff\xfe\n"):
            with self.assertRaises(ValueError) as caught:
                _read_token_file(self.write(data))
            self.assertNotIn(token, str(caught.exception))
        # An empty first line reads as an empty text, which is not a token.
        with self.assertRaises(ValueError):
            _check_token(_read_token_file(self.write(b"\n")))

    def test_checking_a_token_never_shows_it(self):
        for bad in ("", "has space", "x" * 129, "tab\there", "café", 5, b"bytes"):
            with self.assertRaises(ValueError) as caught:
                _check_token(bad)
            if isinstance(bad, str) and bad:
                self.assertNotIn(bad, str(caught.exception))
        self.assertEqual(_check_token("vsst1_ok"), "vsst1_ok")


if __name__ == "__main__":
    unittest.main()
