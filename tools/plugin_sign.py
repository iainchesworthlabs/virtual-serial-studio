#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the signed files of the plugin system around an OpenSSL call.

The owner keeps the private keys offline as OpenSSL Ed25519 key files and signs
by hand (docs/plugins.md, section 8.9). This script builds the message, calls
OpenSSL to sign it and writes the envelope that ``vss::license::signed_file``
reads. It never makes a key, never writes a key and never prints key material:
it passes the path of a key file to OpenSSL and nothing more.

    python3 tools/plugin_sign.py public-key --key plugins-2026.key
    python3 tools/plugin_sign.py plugin --key plugins-2026.key \\
        --key-id plugins-2026 --plugin path/to/plugin
    python3 tools/plugin_sign.py key-list --key root.key --key-id root-2026 \\
        --payload key-list.json --out official-keys.json
    python3 tools/plugin_sign.py revocations --key revocations-2026.key \\
        --key-id revocations-2026 --payload revocations.json \\
        --out revocations.json.sig
    python3 tools/plugin_sign.py verify --public-key plugins-2026.pub \\
        --purpose plugin path/to/plugin/plugin.sig

The signed message is the text ``VSS-SIGNED-FILE-1``, a zero byte, the purpose,
a zero byte and the payload bytes. The envelope is one line of JSON, written
as ``signed_file::write`` writes it.

Exit codes: 0 success, 1 a check or a verification failed, 2 a usage error:
a wrong option, a file or folder that the command line names and that is
missing, a bad key id, or no usable OpenSSL. Messages go to standard error;
only results go to standard output.

OpenSSL 3.0 or later is needed (``-rawin``). A key file with a passphrase is
not supported. Standard library only, Python 3.9 or later.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, NamedTuple, Optional

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2

MESSAGE_TAG = b"VSS-SIGNED-FILE-1"
ENVELOPE_FORMAT = 1
# ``signed_file::max_file_size``: a larger file is refused before it is parsed.
MAX_ENVELOPE_SIZE = 64 * 1024

PURPOSE_KEY_LIST = "key-list"
PURPOSE_PLUGIN = "plugin"
PURPOSE_REVOCATIONS = "plugin-revocations"
PURPOSES = (PURPOSE_KEY_LIST, PURPOSE_PLUGIN, PURPOSE_REVOCATIONS)

# The fixed start of the DER of an Ed25519 public key (RFC 8410): the 32 key
# bytes follow.
ED25519_PUBLIC_PREFIX = bytes.fromhex("302a300506032b6570032100")
KEY_BYTES = 32
SIGNATURE_BYTES = 64

# Limits of the C++ readers (vss_plugin).
KEY_LIST_FORMAT_VERSION = 1
REVOCATIONS_FORMAT_VERSION = 1
MAX_SERIAL = (1 << 53) - 1
MAX_OFFICIAL_KEYS = 64
MAX_REVOKED_KEYS = 256
MAX_REVOKED_ENTRIES = 4096
MAX_NAME_LENGTH = 100
MAX_PLUGIN_ID_LENGTH = 128
MAX_REVOCATIONS_SIZE = 256 * 1024

OPENSSL_TIMEOUT = 120


class Refusal(Exception):
    """A reason to stop, for a person, and the exit code that goes with it."""

    def __init__(self, message: str, code: int = EXIT_FAILED) -> None:
        super().__init__(message)
        self.message = message
        self.code = code


# -- Plain text helpers ------------------------------------------------------


def quoted(text: Any) -> str:
    """``text`` in quotes for a message: cut short, and plain ASCII only."""
    text = str(text)
    shown = "".join(c if " " <= c <= "~" else "?" for c in text[:40])
    return '"' + shown + ("..." if len(text) > 40 else "") + '"'


def shown_path(path: str) -> str:
    """A file name in quotes for a message, whole."""
    return '"' + path + '"'


def load_strict(data: bytes) -> Any:
    """Reads one JSON document as the C++ readers do.

    Refused: bytes that are not UTF-8, text that is not JSON, a number that
    is not plain JSON (``NaN``) and a key given twice in one object.
    Raises ``ValueError`` with a reason.
    """

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("is not valid UTF-8") from None

    def pairs(items: List[Any]) -> Dict[str, Any]:
        found: Dict[str, Any] = {}
        for key, value in items:
            if key in found:
                raise ValueError(
                    "the key %s is given twice in one object" % quoted(key)
                )
            found[key] = value
        return found

    def constant(name: str) -> Any:
        raise ValueError("is not JSON: %s is not a number" % quoted(name))

    try:
        return json.loads(
            text, object_pairs_hook=pairs, parse_constant=constant
        )
    except json.JSONDecodeError as error:
        raise ValueError("is not JSON: %s" % error) from None
    except RecursionError:
        raise ValueError("is nested too deep") from None


def is_whole_number(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


_DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")


def parse_date(value: Any) -> Optional[datetime.date]:
    """``YYYY-MM-DD``, a day that exists, in the year 2000 or later."""
    if not isinstance(value, str) or not _DATE.fullmatch(value):
        return None
    try:
        day = datetime.date(int(value[:4]), int(value[5:7]), int(value[8:]))
    except ValueError:
        return None
    return day if day.year >= 2000 else None


# What ``vss::plugin::is_key_id`` accepts.
_KEY_ID = re.compile(r"[a-z0-9-]{1,64}")
# What the envelope reader accepts as a key id and as a purpose.
_ENVELOPE_KEY_ID = re.compile(r"[a-z0-9-]{1,64}")
_ENVELOPE_PURPOSE = re.compile(r"[a-z0-9-]{1,32}")
_PUBLIC_KEY_HEX = re.compile(r"[0-9a-f]{64}")
_HASH_HEX = re.compile(r"[0-9a-f]{128}")
_ANY_KEY_HEX = re.compile(r"[0-9a-fA-F]{64}")


def is_key_id(value: Any) -> bool:
    return isinstance(value, str) and _KEY_ID.fullmatch(value) is not None


def check_key_id(key_id: str) -> None:
    """Refuses a key id that cannot be used to sign.

    The signed-file reader takes lower-case letters, digits and hyphens only
    in the key id of an envelope, so a file signed with any other id would be
    refused by Studio.
    """

    if _KEY_ID.fullmatch(key_id) is None:
        raise Refusal(
            "the key id %s cannot be used: a key id is 1 to 64 lower-case "
            "letters, digits and hyphens" % quoted(key_id),
            EXIT_USAGE,
        )


_UNCLEAN_POINTS = (
    set(range(0x00, 0x20))
    | {0x7F}
    | set(range(0x80, 0xA0))
    | {0x061C, 0x200E, 0x200F, 0x2028, 0x2029, 0xFEFF}
    | set(range(0x202A, 0x202F))
    | set(range(0x2066, 0x206A))
    | set(range(0xD800, 0xE000))
)


def is_clean_text(text: str) -> bool:
    """Plain text: no control character, separator, mark that changes the
    direction of the text, byte order mark or lone surrogate."""
    return not any(ord(c) in _UNCLEAN_POINTS for c in text)


# -- Plugin id and version (the rules of manifest.cpp) -----------------------


def is_plugin_id(text: Any) -> bool:
    """Lower-case names of letters and digits joined by ``.`` or ``-``, with
    at least one dot, 3 to 128 characters."""
    if not isinstance(text, str) or not 3 <= len(text) <= MAX_PLUGIN_ID_LENGTH:
        return False
    dot = False
    after_separator = True
    for c in text:
        if ("a" <= c <= "z") or ("0" <= c <= "9"):
            after_separator = False
            continue
        if c not in ".-" or after_separator:
            return False
        dot = dot or c == "."
        after_separator = True
    return dot and not after_separator


def _is_number_part(text: str) -> bool:
    if not text or len(text) > 6 or any(not "0" <= c <= "9" for c in text):
        return False
    return len(text) == 1 or text[0] != "0"


def is_plugin_version(text: Any) -> bool:
    """``major.minor.patch`` with an optional ``-pre.release``."""
    if not isinstance(text, str):
        return False
    numbers = text
    if "-" in text:
        numbers, pre = text.split("-", 1)
        if (
            not pre
            or len(pre) > 32
            or pre[0] == "."
            or pre[-1] == "."
            or ".." in pre
            or any(
                not (c.isascii() and (c.isalnum() or c in ".-")) for c in pre
            )
        ):
            return False
    parts = numbers.split(".")
    return len(parts) == 3 and all(_is_number_part(p) for p in parts)


# -- Base64url, the message and the envelope ---------------------------------


def base64url_encode(data: bytes) -> str:
    """base64url without padding (RFC 4648, section 5)."""
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


_BASE64URL = re.compile(r"[A-Za-z0-9_-]*")


def base64url_decode(text: str) -> bytes:
    """The inverse of ``base64url_encode``; refuses what ``signed_file``
    refuses: padding, white space, other alphabets, a length that no whole
    number of bytes has and spare bits that are not zero."""
    if _BASE64URL.fullmatch(text) is None or len(text) % 4 == 1:
        raise ValueError("is not base64url")
    data = base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))
    if base64url_encode(data) != text:
        raise ValueError("is not base64url")
    return data


def signed_message(purpose: str, payload: bytes) -> bytes:
    """The bytes a signature covers: the tag, the purpose and the payload,
    each followed by a zero byte except the payload."""
    return MESSAGE_TAG + b"\0" + purpose.encode("ascii") + b"\0" + payload


def envelope_text(
    purpose: str, key_id: str, payload: bytes, signature: bytes
) -> str:
    """The text of an envelope, as ``signed_file::write`` writes it: the five
    fields in this order, no spaces, one line feed at the end."""
    fields = {
        "format": ENVELOPE_FORMAT,
        "purpose": purpose,
        "key_id": key_id,
        "payload": base64url_encode(payload),
        "signature": base64url_encode(signature),
    }
    return json.dumps(fields, separators=(",", ":")) + "\n"


def manifest_payload(
    plugin_id: str, version: str, manifest_bytes: bytes
) -> bytes:
    """The payload of a ``plugin.sig``: compact JSON with the plugin id, the
    version and the SHA-512 of the exact bytes of ``plugin.json``."""
    fields = {
        "plugin_id": plugin_id,
        "version": version,
        "manifest_sha512": hashlib.sha512(manifest_bytes).hexdigest(),
    }
    return json.dumps(fields, separators=(",", ":")).encode("ascii")


class Envelope(NamedTuple):
    purpose: str
    key_id: str
    payload: bytes
    signature: bytes


def parse_envelope(data: bytes) -> Envelope:
    """Reads an envelope as ``signed_file::parse`` does. Does not check the
    signature. Raises ``ValueError`` with a reason."""
    if len(data) > MAX_ENVELOPE_SIZE:
        raise ValueError("is larger than %d bytes" % MAX_ENVELOPE_SIZE)
    document = load_strict(data)
    if not isinstance(document, dict):
        raise ValueError("is not a JSON object")
    number = document.get("format")
    if not is_whole_number(number) or not 1 <= number <= 1000:
        raise ValueError("has no format number")
    if number != ENVELOPE_FORMAT:
        raise ValueError(
            "is format %d, and this tool reads format %d"
            % (number, ENVELOPE_FORMAT)
        )
    names = ("purpose", "key_id", "payload", "signature")
    if len(document) != 5 or any(
        not isinstance(document.get(name), str) for name in names
    ):
        raise ValueError(
            "must hold the five fields format, purpose, key_id, payload and "
            "signature, and no other"
        )
    purpose = document["purpose"]
    key_id = document["key_id"]
    if _ENVELOPE_PURPOSE.fullmatch(purpose) is None:
        raise ValueError("has a purpose that is not valid")
    if _ENVELOPE_KEY_ID.fullmatch(key_id) is None:
        raise ValueError("has a key id that is not valid")
    try:
        payload = base64url_decode(document["payload"])
        signature = base64url_decode(document["signature"])
    except ValueError:
        raise ValueError("has a payload or a signature that is not base64url")
    if len(signature) != SIGNATURE_BYTES:
        raise ValueError("has a signature that is not 64 bytes")
    return Envelope(purpose, key_id, payload, signature)


# -- OpenSSL -----------------------------------------------------------------


def find_openssl(override: Optional[str] = None) -> str:
    """The path of an OpenSSL 3 program. Refuses when there is none."""
    wanted = override if override else "openssl"
    path = shutil.which(wanted)
    if path is None:
        if override:
            raise Refusal(
                "OpenSSL was not found at %s" % shown_path(override), EXIT_USAGE
            )
        raise Refusal(
            "OpenSSL was not found on the PATH; install OpenSSL 3 or give "
            "its path with --openssl",
            EXIT_USAGE,
        )
    try:
        shown = subprocess.run(
            [path, "version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=OPENSSL_TIMEOUT,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise Refusal("could not run %s" % shown_path(path), EXIT_USAGE) from None
    line = shown.stdout.decode("utf-8", "replace").strip().splitlines()
    found = re.match(r"OpenSSL (\d+)\.", line[0]) if line else None
    if shown.returncode != 0 or found is None or int(found.group(1)) < 3:
        what = line[0] if line else "no version"
        raise Refusal(
            "OpenSSL 3.0 or later is needed (it must support -rawin); "
            "%s said: %s" % (shown_path(path), quoted(what)),
            EXIT_USAGE,
        )
    return path


def run_openssl(openssl: str, arguments: List[str]) -> "subprocess.CompletedProcess[bytes]":
    """Runs OpenSSL with a list of arguments, never through a shell."""
    try:
        return subprocess.run(
            [openssl] + arguments,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=OPENSSL_TIMEOUT,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise Refusal("OpenSSL did not finish in time") from None
    except OSError:
        raise Refusal("could not run OpenSSL", EXIT_USAGE) from None


def first_line(result: "subprocess.CompletedProcess[bytes]") -> str:
    lines = result.stderr.decode("utf-8", "replace").strip().splitlines()
    return lines[0].strip() if lines else "no message"


class Workdir:
    """A temporary folder for the message and signature files. It is removed
    when the ``with`` block ends."""

    def __enter__(self) -> str:
        self.path = tempfile.mkdtemp(prefix="plugin-sign-")
        return self.path

    def __exit__(self, *_: Any) -> None:
        shutil.rmtree(self.path, ignore_errors=True)


def _public_from_der(der: bytes) -> bytes:
    if len(der) != len(ED25519_PUBLIC_PREFIX) + KEY_BYTES or not der.startswith(
        ED25519_PUBLIC_PREFIX
    ):
        raise Refusal("the key is not an Ed25519 key")
    return der[-KEY_BYTES:]


def public_key_of(openssl: str, key_file: str) -> bytes:
    """The 32 bytes of the public key of a private or a public Ed25519 key
    file, as OpenSSL gives them: the last 32 bytes of the DER."""
    with Workdir() as work:
        out = os.path.join(work, "public.der")
        first = run_openssl(
            openssl,
            ["pkey", "-in", key_file, "-pubout", "-outform", "DER", "-out", out],
        )
        if first.returncode != 0:
            second = run_openssl(
                openssl,
                ["pkey", "-pubin", "-in", key_file, "-outform", "DER", "-out", out],
            )
            if second.returncode != 0:
                raise Refusal(
                    "OpenSSL could not read an Ed25519 key from %s (%s)"
                    % (shown_path(key_file), first_line(first))
                )
        with open(out, "rb") as handle:
            return _public_from_der(handle.read())


def sign_bytes(openssl: str, key_file: str, message: bytes) -> bytes:
    """``openssl pkeyutl -sign -rawin``: the 64 bytes of the signature."""
    with Workdir() as work:
        message_file = os.path.join(work, "message.bin")
        signature_file = os.path.join(work, "signature.bin")
        with open(message_file, "wb") as handle:
            handle.write(message)
        done = run_openssl(
            openssl,
            [
                "pkeyutl", "-sign", "-inkey", key_file, "-rawin",
                "-in", message_file, "-out", signature_file,
            ],
        )
        if done.returncode != 0:
            raise Refusal("OpenSSL could not sign (%s)" % first_line(done))
        with open(signature_file, "rb") as handle:
            signature = handle.read()
    if len(signature) != SIGNATURE_BYTES:
        raise Refusal("OpenSSL made a signature that is not 64 bytes")
    return signature


def verify_bytes(
    openssl: str, public_key: bytes, message: bytes, signature: bytes
) -> bool:
    """``openssl pkeyutl -verify -pubin -rawin`` with the 32 bytes of a
    public key. False when the signature does not hold."""
    with Workdir() as work:
        key_file = os.path.join(work, "public.der")
        message_file = os.path.join(work, "message.bin")
        signature_file = os.path.join(work, "signature.bin")
        for name, content in (
            (key_file, ED25519_PUBLIC_PREFIX + public_key),
            (message_file, message),
            (signature_file, signature),
        ):
            with open(name, "wb") as handle:
                handle.write(content)
        done = run_openssl(
            openssl,
            [
                "pkeyutl", "-verify", "-pubin", "-inkey", key_file,
                "-keyform", "DER", "-rawin", "-in", message_file,
                "-sigfile", signature_file,
            ],
        )
    return done.returncode == 0


def sign_envelope(
    openssl: str, key_file: str, purpose: str, key_id: str, payload: bytes
) -> str:
    """Signs ``payload`` for ``purpose`` and returns the envelope text. The
    signature is checked with the public key of the key file before it is
    returned."""
    check_key_id(key_id)
    public_key = public_key_of(openssl, key_file)
    message = signed_message(purpose, payload)
    signature = sign_bytes(openssl, key_file, message)
    if not verify_bytes(openssl, public_key, message, signature):
        raise Refusal("the signature that was made does not verify")
    text = envelope_text(purpose, key_id, payload, signature)
    if len(text) > MAX_ENVELOPE_SIZE:
        raise Refusal(
            "the signed file would be %d bytes; Studio reads at most %d"
            % (len(text), MAX_ENVELOPE_SIZE)
        )
    return text


# -- Checks of the payloads, as the C++ readers make them --------------------


def _members(
    value: Any, where: str, known: List[str], problems: List[str]
) -> Optional[Dict[str, Any]]:
    """``value`` as an object with no key outside ``known``."""
    if not isinstance(value, dict):
        problems.append("%s: must be an object" % where)
        return None
    unknown = [key for key in value if key not in known]
    if unknown:
        problems.append("%s: unknown key %s" % (where, quoted(unknown[0])))
        return None
    return value


def _serial_problem(document: Dict[str, Any]) -> Optional[str]:
    serial = document.get("serial")
    if not is_whole_number(serial) or not 1 <= serial <= MAX_SERIAL:
        return "serial: must be a whole number from 1 to %d" % MAX_SERIAL
    return None


def _load_payload(data: bytes, what: str, problems: List[str]) -> Any:
    try:
        return load_strict(data)
    except ValueError as error:
        problems.append("%s %s" % (what, error))
        return None


def validate_key_list(data: bytes) -> List[str]:
    """What the key list reader (``parse_key_list``) would refuse in these
    bytes, one line each. An empty list means it reads the payload."""
    problems: List[str] = []
    document = _load_payload(data, "key list", problems)
    if problems:
        return problems
    root = _members(
        document,
        "key list",
        ["key_list_version", "serial", "issued", "keys", "revoked"],
        problems,
    )
    if root is None:
        return problems

    version = root.get("key_list_version")
    if not is_whole_number(version) or version != KEY_LIST_FORMAT_VERSION:
        problems.append("key_list_version: must be %d" % KEY_LIST_FORMAT_VERSION)
    serial_problem = _serial_problem(root)
    if serial_problem:
        problems.append(serial_problem)
    if parse_date(root.get("issued")) is None:
        problems.append("issued: must be a date such as 2026-11-13")

    keys = root.get("keys", [])
    if not isinstance(keys, list) or len(keys) > MAX_OFFICIAL_KEYS:
        problems.append("keys: must be a list of at most %d" % MAX_OFFICIAL_KEYS)
        keys = []
    seen: List[str] = []
    for index, item in enumerate(keys):
        where = "keys[%d]" % index
        entry = _members(
            item,
            where,
            ["key_id", "public_key", "purpose", "name", "valid_from", "valid_to"],
            problems,
        )
        if entry is not None:
            _check_key_entry(entry, where, seen, problems)

    revoked = root.get("revoked", [])
    if not isinstance(revoked, list) or len(revoked) > MAX_REVOKED_KEYS:
        problems.append("revoked: must be a list of at most %d" % MAX_REVOKED_KEYS)
    elif not all(is_key_id(item) for item in revoked):
        problems.append("revoked: holds something that is not a key id")
    return problems


def _check_key_entry(
    entry: Dict[str, Any], where: str, seen: List[str], problems: List[str]
) -> None:
    fields = ("key_id", "public_key", "purpose", "name", "valid_from", "valid_to")
    missing = [f for f in fields if not isinstance(entry.get(f), str)]
    if missing:
        problems.append("%s.%s: must be a string" % (where, missing[0]))
        return
    key_id = entry["key_id"]
    if not is_key_id(key_id):
        problems.append("%s.key_id: cannot be %s" % (where, quoted(key_id)))
    elif key_id in seen:
        problems.append("keys: holds %s twice" % quoted(key_id))
    else:
        seen.append(key_id)
    if _PUBLIC_KEY_HEX.fullmatch(entry["public_key"]) is None:
        problems.append(
            "%s.public_key: must be 64 lower-case hex characters" % where
        )
    if entry["purpose"] not in (PURPOSE_PLUGIN, PURPOSE_REVOCATIONS):
        problems.append(
            "%s.purpose: cannot be %s; use \"plugin\" or "
            "\"plugin-revocations\"" % (where, quoted(entry["purpose"]))
        )
    name = entry["name"]
    if (
        not name
        or not is_clean_text(name)
        or len(name.encode("utf-8", "replace")) > MAX_NAME_LENGTH
    ):
        problems.append("%s.name: must be a short plain text" % where)
    start = parse_date(entry["valid_from"])
    end = parse_date(entry["valid_to"])
    if start is None:
        problems.append(
            "%s.valid_from: must be a date such as 2026-11-13" % where
        )
    if end is None:
        problems.append("%s.valid_to: must be a date such as 2026-11-13" % where)
    if start is not None and end is not None and end < start:
        problems.append("%s.valid_to: is before valid_from" % where)


def validate_revocations(data: bytes) -> List[str]:
    """What the revocation list reader (``parse_revocations``) would refuse in
    these bytes, one line each. An empty list means it reads the payload."""
    problems: List[str] = []
    if len(data) > MAX_REVOCATIONS_SIZE:
        return ["revocations: is larger than %d bytes" % MAX_REVOCATIONS_SIZE]
    document = _load_payload(data, "revocations", problems)
    if problems:
        return problems
    root = _members(
        document,
        "revocations",
        [
            "revocations_version", "serial", "issued_at", "key_ids",
            "plugin_hashes", "plugins",
        ],
        problems,
    )
    if root is None:
        return problems

    version = root.get("revocations_version")
    if not is_whole_number(version) or version != REVOCATIONS_FORMAT_VERSION:
        problems.append(
            "revocations_version: must be %d" % REVOCATIONS_FORMAT_VERSION
        )
    serial_problem = _serial_problem(root)
    if serial_problem:
        problems.append(serial_problem)
    if parse_date(root.get("issued_at")) is None:
        problems.append("issued_at: must be a date such as 2026-11-13")

    def entries(name: str) -> List[Any]:
        value = root.get(name, [])
        if not isinstance(value, list):
            problems.append("%s: must be a list" % name)
            return []
        if len(value) > MAX_REVOKED_ENTRIES:
            problems.append(
                "%s: has more than %d entries" % (name, MAX_REVOKED_ENTRIES)
            )
            return []
        return value

    if not all(is_key_id(item) for item in entries("key_ids")):
        problems.append("key_ids: holds something that is not a key id")
    if not all(
        isinstance(item, str) and _HASH_HEX.fullmatch(item)
        for item in entries("plugin_hashes")
    ):
        problems.append(
            "plugin_hashes: holds something that is not 128 lower-case hex "
            "characters"
        )
    for index, item in enumerate(entries("plugins")):
        where = "plugins[%d]" % index
        entry = _members(item, where, ["id", "max_version"], problems)
        if entry is None:
            continue
        for name in ("id", "max_version"):
            if not isinstance(entry.get(name), str):
                problems.append("%s.%s: must be a string" % (where, name))
        if isinstance(entry.get("id"), str) and not is_plugin_id(entry["id"]):
            problems.append(
                "%s.id: cannot be %s" % (where, quoted(entry["id"]))
            )
        if isinstance(entry.get("max_version"), str) and not is_plugin_version(
            entry["max_version"]
        ):
            problems.append(
                "%s.max_version: cannot be %s"
                % (where, quoted(entry["max_version"]))
            )
    return problems


def check_payload(problems: List[str], what: str) -> None:
    if problems:
        raise Refusal(
            "the %s payload would be refused by Studio:\n  %s"
            % (what, "\n  ".join(problems))
        )


# -- Files -------------------------------------------------------------------


def read_file(path: str, what: str) -> bytes:
    if not os.path.isfile(path):
        raise Refusal("%s %s was not found" % (what, shown_path(path)), EXIT_USAGE)
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as error:
        raise Refusal(
            "could not read %s %s (%s)" % (what, shown_path(path), error.strerror),
            EXIT_USAGE,
        ) from None


def require_key_file(path: str) -> None:
    if not os.path.isfile(path):
        raise Refusal("the key file %s was not found" % shown_path(path), EXIT_USAGE)


def check_output(path: str, protected: List[str]) -> None:
    """Refuses an output file that is one of the ``protected`` files, which
    are the key and the input: writing there would destroy them."""
    for other in protected:
        if os.path.exists(path) and os.path.exists(other) and os.path.samefile(
            path, other
        ):
            raise Refusal(
                "the output file %s is an input file" % shown_path(path),
                EXIT_USAGE,
            )


def write_text(path: str, text: str) -> None:
    """Writes ``text`` as ASCII with the line feed kept as it is."""
    try:
        with open(path, "wb") as handle:
            handle.write(text.encode("ascii"))
    except OSError as error:
        raise Refusal(
            "could not write %s (%s)" % (shown_path(path), error.strerror),
            EXIT_USAGE,
        ) from None


# -- Commands ----------------------------------------------------------------


def command_public_key(args: argparse.Namespace) -> int:
    require_key_file(args.key)
    openssl = find_openssl(args.openssl)
    print(public_key_of(openssl, args.key).hex())
    return EXIT_OK


def command_plugin(args: argparse.Namespace) -> int:
    check_key_id(args.key_id)
    require_key_file(args.key)
    if not os.path.isdir(args.plugin):
        raise Refusal(
            "the plugin folder %s was not found" % shown_path(args.plugin),
            EXIT_USAGE,
        )
    manifest_path = os.path.join(args.plugin, "plugin.json")
    manifest = read_file(manifest_path, "the manifest")
    try:
        document = load_strict(manifest)
    except ValueError as error:
        raise Refusal("plugin.json %s" % error) from None
    if not isinstance(document, dict):
        raise Refusal("plugin.json is not a JSON object")
    plugin_id = document.get("id")
    version = document.get("version")
    if not is_plugin_id(plugin_id):
        raise Refusal(
            "plugin.json: id %s is not a plugin id, such as "
            "\"com.example.my-plugin\"" % quoted(plugin_id)
        )
    if not is_plugin_version(version):
        raise Refusal(
            "plugin.json: version %s must be a version such as \"1.2.0\""
            % quoted(version)
        )
    out = args.out if args.out else os.path.join(args.plugin, "plugin.sig")
    check_output(out, [args.key, manifest_path])
    openssl = find_openssl(args.openssl)
    payload = manifest_payload(plugin_id, version, manifest)
    text = sign_envelope(openssl, args.key, PURPOSE_PLUGIN, args.key_id, payload)
    write_text(out, text)
    print("wrote %s" % out)
    return EXIT_OK


def _command_payload(
    args: argparse.Namespace, purpose: str, what: str, validate: Any
) -> int:
    check_key_id(args.key_id)
    require_key_file(args.key)
    payload = read_file(args.payload, "the payload file")
    check_payload(validate(payload), what)
    check_output(args.out, [args.key, args.payload])
    openssl = find_openssl(args.openssl)
    text = sign_envelope(openssl, args.key, purpose, args.key_id, payload)
    write_text(args.out, text)
    print("wrote %s" % args.out)
    return EXIT_OK


def command_key_list(args: argparse.Namespace) -> int:
    return _command_payload(
        args, PURPOSE_KEY_LIST, "key list", validate_key_list
    )


def command_revocations(args: argparse.Namespace) -> int:
    return _command_payload(
        args, PURPOSE_REVOCATIONS, "revocation list", validate_revocations
    )


def command_verify(args: argparse.Namespace) -> int:
    data = read_file(args.file, "the signed file")
    openssl = find_openssl(args.openssl)
    if _ANY_KEY_HEX.fullmatch(args.public_key):
        public_key = bytes.fromhex(args.public_key)
    else:
        if not os.path.isfile(args.public_key):
            raise Refusal(
                "the public key %s is neither 64 hex characters nor a file"
                % shown_path(args.public_key),
                EXIT_USAGE,
            )
        public_key = public_key_of(openssl, args.public_key)
    try:
        found = parse_envelope(data)
    except ValueError as error:
        raise Refusal("%s %s" % (args.file, error)) from None
    if found.purpose != args.purpose:
        raise Refusal(
            "the file is signed as %s, not as %s"
            % (quoted(found.purpose), quoted(args.purpose))
        )
    message = signed_message(found.purpose, found.payload)
    if not verify_bytes(openssl, public_key, message, found.signature):
        raise Refusal("the signature does not hold")
    print("ok")
    return EXIT_OK


# -- Command line ------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="plugin_sign.py",
        description="Build the signed files of the plugin system around an "
        "OpenSSL call. It never makes, writes or prints a key.",
        allow_abbrev=False,
    )
    commands = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    common.add_argument(
        "--openssl", metavar="PATH", help="the OpenSSL program, if not on PATH"
    )

    def add(name: str, help_text: str, function: Any) -> argparse.ArgumentParser:
        sub = commands.add_parser(
            name, help=help_text, parents=[common], allow_abbrev=False
        )
        sub.set_defaults(handler=function)
        return sub

    sub = add(
        "public-key",
        "print the public key of a key file as 64 hex characters",
        command_public_key,
    )
    sub.add_argument("--key", required=True, help="an Ed25519 key file (PEM)")

    sub = add("plugin", "sign a plugin folder", command_plugin)
    sub.add_argument("--key", required=True, help="the private key file")
    sub.add_argument("--key-id", required=True, help="the id of the key")
    sub.add_argument("--plugin", required=True, help="the plugin folder")
    sub.add_argument("--out", help="the file to write; default plugin.sig")

    for name, help_text, function in (
        ("key-list", "sign a key list payload", command_key_list),
        ("revocations", "sign a revocation list payload", command_revocations),
    ):
        sub = add(name, help_text, function)
        sub.add_argument("--key", required=True, help="the private key file")
        sub.add_argument("--key-id", required=True, help="the id of the key")
        sub.add_argument("--payload", required=True, help="the JSON payload file")
        sub.add_argument("--out", required=True, help="the file to write")

    sub = add("verify", "check the signature of a signed file", command_verify)
    sub.add_argument(
        "--public-key",
        required=True,
        help="64 hex characters, or a public key file",
    )
    sub.add_argument("--purpose", required=True, choices=PURPOSES)
    sub.add_argument("file", help="the signed file")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        return int(arguments.handler(arguments))
    except Refusal as refusal:
        print("plugin_sign: %s" % refusal.message, file=sys.stderr)
        return refusal.code


if __name__ == "__main__":
    sys.exit(main())
