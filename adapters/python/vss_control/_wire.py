# SPDX-License-Identifier: GPL-3.0-or-later
"""The messages of the control API, version 1, as message.cpp writes and reads
them (libs/vss_control_proto).  A message is a frame whose header is a JSON
object.

    request  {"id": 7, "op": "ports.list"}              (+ blob for bytes)
    reply    {"id": 7, "reply": "ports", "revision": 3, ...}
    error    {"id": 7, "reply": "error", "error": {"code", "message", "retryable"}}
    batch    {"event": "batch", "seq": .., "revision": .., "dropped": ..,
              "time": {"mono_ns": .., "wall_ms": ..}, "events": [...]}
             + a blob: the bytes of the events, in order, without gaps

A decoder here ignores a field it does not know, as the API promises within a
major version.  The size of the whole frame bounds every text and list, so
there is no limit on each of them as well.
"""

from __future__ import annotations

import json
import typing

from ._frame import encode_frame

# The version of the API this module speaks.
API_VERSION = 1
# The longest text the gateway takes in a field, in bytes.
MAX_TEXT_SIZE = 1024
_U64_MAX = (1 << 64) - 1


class WireError(Exception):
    """A header that is not what the gateway sends."""


class Reply(typing.NamedTuple):
    id: int
    kind: str
    fields: dict


class Batch(typing.NamedTuple):
    value: dict


def clean_text(text):
    """Text that is safe to send: a character that is not valid becomes `?`
    and the UTF-8 is cut to MAX_TEXT_SIZE bytes between characters."""
    if not isinstance(text, str):
        raise TypeError("expected text, not %s" % type(text).__name__)
    raw = text.encode("utf-8", "replace")
    if len(raw) > MAX_TEXT_SIZE:
        raw = raw[:MAX_TEXT_SIZE]
        return raw.decode("utf-8", "ignore")
    return raw.decode("utf-8")


def encode_request(request_id, op, fields=None, blob=b""):
    """The frame bytes of a request."""
    header = {"id": request_id, "op": op}
    if fields:
        header.update(fields)
    text = json.dumps(header, separators=(",", ":"), allow_nan=False)
    return encode_frame(text.encode("ascii"), blob)


# -- reading -----------------------------------------------------------------


def _reject_constant(name):
    raise ValueError("not JSON: " + name)


def _parse_header(raw):
    try:
        header = json.loads(raw.decode("utf-8"), parse_constant=_reject_constant)
    except (UnicodeDecodeError, ValueError, RecursionError):
        raise WireError("the header is not JSON") from None
    if not isinstance(header, dict):
        raise WireError("the header is not an object")
    return header


def _is_u64(value):
    return type(value) is int and 0 <= value <= _U64_MAX


_U = ("a number", _is_u64)
_S = ("text", lambda value: isinstance(value, str))
_B = ("a boolean", lambda value: isinstance(value, bool))
_L = ("a list", lambda value: isinstance(value, list))
_D = ("an object", lambda value: isinstance(value, dict))

# The fields that each kind of reply must have, and their types.
_REPLIES = {
    "done": {},
    "hello": {
        "api": _U,
        "version": _S,
        "principal": _S,
        "scopes": _L,
        "features": _L,
    },
    "status": {"uptime_ms": _U, "sessions": _U, "agent": _D},
    "ports": {"revision": _U, "ports": _L, "links": _L},
    "created": {"created": _S},
    "matched": {"offset": _U, "mono_ns": _U, "events_lost": _U},
    "subscribed": {"interval_ms": _U, "next_seq": _U},
    "run": {
        "run": _S,
        "name": _S,
        "open": _B,
        "tests": _U,
        "failures": _U,
        "errors": _U,
        "skipped": _U,
        "duration_ms": _U,
    },
    "report": {"xml": _S},
    "error": {"error": _D},
}


def _require(header, name, kind):
    if name not in header:
        raise WireError("the field %s is missing" % name)
    description, check = kind
    if not check(header[name]):
        raise WireError("the field %s is not %s" % (name, description))
    return header[name]


def _decode_reply(header, blob):
    if blob:
        raise WireError("a reply has no blob")
    request_id = _require(header, "id", _U)
    kind = _require(header, "reply", _S)
    wanted = _REPLIES.get(kind)
    if wanted is None:
        raise WireError("a reply of an unknown kind")
    for name, type_ in wanted.items():
        _require(header, name, type_)
    if kind == "error":
        error = header["error"]
        _require(error, "code", _S)
        _require(error, "message", _S)
        _require(error, "retryable", _B)
    if kind == "status":
        _require(header["agent"], "running", _B)
        _require(header["agent"], "version", _S)
    fields = {k: v for k, v in header.items() if k not in ("id", "reply")}
    return Reply(request_id, kind, fields)


def _optional(header, name, kind, default):
    if name not in header:
        return default
    return _require(header, name, kind)


def _decode_event(item, blob, cursor):
    """One event, and where the next one's bytes start in `blob`."""
    if not isinstance(item, dict):
        raise WireError("an event is not an object")
    stored = _optional(item, "stored", _U, 0)
    if stored > len(blob) - cursor:
        raise WireError("an event names more bytes than the frame holds")
    event = {
        "kind": _require(item, "kind", _S),
        "port": _optional(item, "port", _S, ""),
        "name": _optional(item, "name", _S, ""),
        "direction": _optional(item, "direction", _S, ""),
        "size": _optional(item, "size", _U, 0),
        "data": blob[cursor : cursor + stored],
        "lines": _optional(item, "lines", _U, 0),
        "message": _optional(item, "message", _S, ""),
        "error": _optional(item, "error", _S, ""),
        "mono_ns": _optional(item, "mono_ns", _U, 0),
    }
    return event, cursor + stored


def _decode_batch(header, blob):
    events = _require(header, "events", _L)
    time_ = _require(header, "time", _D)
    batch = {
        "seq": _require(header, "seq", _U),
        "revision": _require(header, "revision", _U),
        "dropped": _require(header, "dropped", _U),
        "mono_ns": _require(time_, "mono_ns", _U),
        "wall_ms": _require(time_, "wall_ms", _U),
    }
    cursor = 0
    decoded = []
    # The events use the blob in order and without gaps, so a small frame
    # cannot name the same bytes many times.
    for item in events:
        event, cursor = _decode_event(item, blob, cursor)
        decoded.append(event)
    if cursor != len(blob):
        raise WireError("the frame holds bytes that no event names")
    batch["events"] = decoded
    return Batch(batch)


def decode_server_message(frame):
    """A Reply or a Batch, or a WireError."""
    header = _parse_header(frame.header)
    if "event" in header:
        if header["event"] != "batch":
            raise WireError("an event of an unknown kind")
        return _decode_batch(header, frame.blob)
    return _decode_reply(header, frame.blob)
