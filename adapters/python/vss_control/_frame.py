# SPDX-License-Identifier: GPL-3.0-or-later
"""The framing of the agent protocol, as libs/vss_agent_proto/frame.hpp has it.

    u32le  size of everything after this field   (6 .. MAX_FRAME_SIZE)
    u32le  size of the header                    (2 .. size - 4)
    header a JSON object, UTF-8
    blob   the rest: bytes the header describes

FrameReader cuts a byte stream into frames.  It looks at the length prefix as
soon as the four bytes are there and refuses a frame that is too long or
that has an impossible header size at once, before it holds any of the body:
it never allocates for a length it has not accepted.  An error is final.
"""

from __future__ import annotations

import typing

# The most bytes a frame may have after its length prefix.
MAX_FRAME_SIZE = 1 << 20
# The bytes of the length prefix.
PREFIX_SIZE = 4
# The smallest header is `{}`.
MIN_HEADER_SIZE = 2


class FrameError(Exception):
    """A frame that breaks the rules.  `code` is `oversized_frame` or
    `malformed_frame`, the names of the C++ protocol errors."""

    def __init__(self, code, message):
        super().__init__(code, message)
        self.code = code
        self.message = message

    def __str__(self):
        return "%s: %s" % (self.code, self.message)


class Frame(typing.NamedTuple):
    header: bytes
    blob: bytes


def encode_frame(header, blob=b""):
    """The bytes of one frame.  `header` is bytes of UTF-8 JSON."""
    header = bytes(header)
    blob = bytes(blob)
    if len(header) < MIN_HEADER_SIZE:
        raise FrameError("malformed_frame", "the header is empty")
    if len(header) > MAX_FRAME_SIZE or len(blob) > MAX_FRAME_SIZE:
        raise FrameError("oversized_frame", "the frame is too long")
    size = PREFIX_SIZE + len(header) + len(blob)
    if size > MAX_FRAME_SIZE:
        raise FrameError("oversized_frame", "the frame is too long")
    return (
        size.to_bytes(4, "little")
        + len(header).to_bytes(4, "little")
        + header
        + blob
    )


class FrameReader:
    """Feed it what was read, in pieces of any size, and take frames."""

    def __init__(self, limit=MAX_FRAME_SIZE):
        self._buffer = bytearray()
        self._start = 0
        self._limit = MAX_FRAME_SIZE
        self._failed = None
        self.set_limit(limit)

    def set_limit(self, limit):
        """The most bytes the next frames may have after their prefix."""
        self._limit = min(int(limit), MAX_FRAME_SIZE)

    @property
    def buffered(self):
        """Bytes held that are not yet part of a returned frame."""
        return len(self._buffer) - self._start

    def feed(self, data):
        if self._failed is not None:
            return
        # Drop what earlier frames used once it is most of the buffer, so a
        # long connection does not grow it.
        if self._start and self._start >= len(self._buffer) // 2:
            del self._buffer[: self._start]
            self._start = 0
        self._buffer += data

    def _fail(self, code, message):
        self._failed = (code, message)
        raise FrameError(code, message)

    def next(self):
        """The next whole Frame, None when more bytes are needed, or a
        FrameError, which ends the stream for good."""
        if self._failed is not None:
            raise FrameError(*self._failed)
        buffer = self._buffer
        start = self._start
        available = len(buffer) - start
        if available < PREFIX_SIZE:
            return None

        size = int.from_bytes(buffer[start : start + 4], "little")
        if size > self._limit:
            self._fail("oversized_frame", "the frame is longer than allowed")
        if size < PREFIX_SIZE + MIN_HEADER_SIZE:
            self._fail("malformed_frame", "the frame is too short")
        # The header size is checked as soon as it is here, not when the
        # whole frame is, so a bad one is refused before its body is held.
        if available < 2 * PREFIX_SIZE:
            return None
        header_size = int.from_bytes(buffer[start + 4 : start + 8], "little")
        if header_size < MIN_HEADER_SIZE or header_size > size - PREFIX_SIZE:
            self._fail("malformed_frame", "the header size is not possible")
        if available < PREFIX_SIZE + size:
            return None

        header_from = start + 2 * PREFIX_SIZE
        blob_from = header_from + header_size
        end = start + PREFIX_SIZE + size
        frame = Frame(bytes(buffer[header_from:blob_from]), bytes(buffer[blob_from:end]))
        self._start = end
        if self._start == len(buffer):
            buffer.clear()
            self._start = 0
        return frame
