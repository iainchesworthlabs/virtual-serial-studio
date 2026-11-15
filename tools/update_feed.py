#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build, sign and verify the update feed that Studio reads.

The update feed is a small signed file that says which Studio (and which
official plugins) are current (docs/updates.md, section 4). This tool makes
its payload from the files of a release, signs the payload and checks a
finished feed. It works from files only and makes no network request, so the
release job and the tests feed it the same way.

    python3 tools/update_feed.py payload --serial 12 \\
        --issued-at 2026-12-01T03:40:00Z --stable release-v0.2.0 \\
        [--preview release-v0.3.0-rc.1] [--plugins plugins.json] \\
        --out feed.payload.json
    python3 tools/update_feed.py sign --key upd-2026-a.key \\
        --key-id upd-2026-a --payload feed.payload.json \\
        --out update-feed.json
    python3 tools/update_feed.py verify --public-key <64 hex characters> \\
        [--key-id upd-2026-a] [--min-serial 12] update-feed.json

A release folder holds ``release.json`` (the shape of website/data/release.json:
tag, prerelease, published_at, html_url and the assets with name, size and url),
``SHA512SUMS`` (as ``sha512sum`` writes it) and, if there is one,
``summary.txt`` (plain text, UTF-8: the summary of the release).

The envelope, the signed message and the call to OpenSSL are those of
tools/plugin_sign.py, which this tool imports. It never makes a key, never
writes a key and never prints key material: it passes the path of a key file
to OpenSSL and nothing more. The purpose of the envelope is ``update-feed``.

``check_payload`` holds the rules of the payload, one line for each problem.
They mirror the reader in Studio (docs/updates.md, section 4.3); change both
together. ``payload`` and ``sign`` run it before they write anything, and
``verify`` runs it after the signature has been checked.

Exit codes: 0 success, 1 a check or a verification failed, 2 a usage error:
a wrong option, a file or folder that the command line names and that is
missing, a bad key id, or no usable OpenSSL. Messages go to standard error;
only results go to standard output.

OpenSSL 3.0 or later is needed to sign and to verify. A key file with a
passphrase is not supported. Standard library only, Python 3.9 or later.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
from typing import Any, Dict, List, NamedTuple, Optional, Tuple, Union

import plugin_sign as ps
from plugin_sign import (
    EXIT_FAILED,
    EXIT_OK,
    EXIT_USAGE,
    MAX_ENVELOPE_SIZE,
    Refusal,
    quoted,
    shown_path,
)

PURPOSE = "update-feed"
FEED_VERSION = 1

CHANNELS = ("stable", "preview")
ARCHES = ("x86_64", "arm64")
OSES = ("windows", "macos", "linux")

# The kind of a studio file and the file name that goes with it, in the order
# the rules are tried: the first that matches wins. The order of the table is
# also the order of the files in the payload.
FILE_RULES: Tuple[Tuple[str, str, str], ...] = (
    (r"-windows-x86_64(-[a-z]+)?\.exe", "windows-installer", "x86_64"),
    (r"-windows-x86_64(-[a-z]+)?\.zip", "windows-zip", "x86_64"),
    (r"-macos-arm64\.dmg", "macos-dmg", "arm64"),
    (r"-macos-arm64\.tar\.gz", "macos-archive", "arm64"),
    (r"_amd64\.deb", "linux-deb", "x86_64"),
    (r"\.x86_64\.rpm", "linux-rpm", "x86_64"),
    (r"-linux-x86_64\.tar\.gz", "linux-archive", "x86_64"),
)
KINDS = tuple(kind for _, kind, _ in FILE_RULES)
_RULES = tuple(
    (re.compile(pattern + r"\Z"), kind, arch) for pattern, kind, arch in FILE_RULES
)

# Limits of the reader. The serial goes up to 2^53 itself (the other signed
# files stop at 2^53 - 1, as plugin_sign.MAX_SERIAL does).
MAX_FEED_SERIAL = 1 << 53
MAX_STUDIO_ENTRIES = 2
MAX_STUDIO_FILES = 16
MAX_PLUGIN_FILES = 16
MAX_PLUGINS = 64
MAX_VERSION_LENGTH = 64
MAX_VERSION_NUMBER = 999999
MAX_PRERELEASE_IDENTIFIERS = 16
MAX_NAME_LENGTH = 255
MAX_URL_LENGTH = 2048
MAX_SIZE = 4 * 1024 * 1024 * 1024
MAX_SUMMARY_BYTES = 2000
MAX_PLUGIN_ID_LENGTH = 128
PLUGIN_ID_PREFIX = "com.virtualserialstudio."

# The order of the keys in the payload (the example of docs/updates.md, 4.3).
TOP_KEYS = ("feed_version", "serial", "issued_at", "studio", "plugins")
STUDIO_KEYS = ("channel", "version", "released", "summary", "notes_url", "files")
STUDIO_FILE_KEYS = ("kind", "arch", "name", "url", "size", "sha512")
PLUGIN_KEYS = (
    "id", "version", "released", "requires_studio", "summary", "notes_url", "files",
)
PLUGIN_FILE_KEYS = ("os", "arch", "name", "url", "size", "sha512")


# -- Versions ----------------------------------------------------------------

_NUMBER = re.compile(r"(0|[1-9][0-9]*)")
_IDENTIFIER = re.compile(r"[0-9A-Za-z-]+")
_DIGITS = re.compile(r"[0-9]+")


def _parse_version(text: Any) -> Tuple[Tuple[int, int, int], Tuple[str, ...]]:
    """The three numbers and the pre-release identifiers of a version.
    Raises ``ValueError`` with the reason when it is not one the feed allows."""
    if not isinstance(text, str):
        raise ValueError("must be a string")
    if not 1 <= len(text) <= MAX_VERSION_LENGTH:
        raise ValueError("must be 1 to %d characters" % MAX_VERSION_LENGTH)
    core, dash, pre = text.partition("-")
    parts = core.split(".")
    if len(parts) != 3 or any(_NUMBER.fullmatch(p) is None for p in parts):
        raise ValueError(
            "must be a version such as 0.2.0 or 0.3.0-rc.1: three numbers "
            "joined by dots, no leading zero, no leading v and no build "
            "metadata"
        )
    if any(int(p) > MAX_VERSION_NUMBER for p in parts):
        raise ValueError("has a number above %d" % MAX_VERSION_NUMBER)
    numbers = (int(parts[0]), int(parts[1]), int(parts[2]))
    if not dash:
        return numbers, ()
    identifiers = pre.split(".")
    if len(identifiers) > MAX_PRERELEASE_IDENTIFIERS:
        raise ValueError(
            "has more than %d pre-release identifiers" % MAX_PRERELEASE_IDENTIFIERS
        )
    for identifier in identifiers:
        if _IDENTIFIER.fullmatch(identifier) is None:
            raise ValueError(
                "has a pre-release identifier that is empty or holds a "
                "character other than letters, digits and hyphen"
            )
        if _DIGITS.fullmatch(identifier) and _NUMBER.fullmatch(identifier) is None:
            raise ValueError("has a numeric pre-release identifier with a leading zero")
    if pre == "dev":
        raise ValueError("is a development version, which no feed may name")
    return numbers, tuple(identifiers)


def version_problem(text: Any) -> Optional[str]:
    """Why ``text`` is not a version the feed allows, or ``None``."""
    try:
        _parse_version(text)
    except ValueError as error:
        return str(error)
    return None


def is_version(text: Any) -> bool:
    return version_problem(text) is None


def is_prerelease(version: str) -> bool:
    """True when a valid version has a pre-release part."""
    return "-" in version


def compare_versions(first: str, second: str) -> int:
    """-1, 0 or 1: the precedence of two versions as Semantic Versioning
    2.0.0 defines it (item 11). Both must be versions the feed allows."""
    numbers_a, pre_a = _parse_version(first)
    numbers_b, pre_b = _parse_version(second)
    if numbers_a != numbers_b:
        return -1 if numbers_a < numbers_b else 1
    if pre_a == pre_b:
        return 0
    if not pre_a:
        return 1
    if not pre_b:
        return -1
    for left, right in zip(pre_a, pre_b):
        if left == right:
            continue
        left_numeric = _DIGITS.fullmatch(left) is not None
        right_numeric = _DIGITS.fullmatch(right) is not None
        if left_numeric and right_numeric:
            return -1 if int(left) < int(right) else 1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return -1 if left < right else 1
    if len(pre_a) == len(pre_b):
        return 0
    return -1 if len(pre_a) < len(pre_b) else 1


# -- The rules of the payload (check_payload) --------------------------------

_DATE = re.compile(r"([0-9]{4})-([0-9]{2})-([0-9]{2})")
_TIMESTAMP = re.compile(
    r"([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})Z"
)
_FILE_NAME = re.compile(r"[A-Za-z0-9._+~-]+")
# What Windows gives to a device, whatever follows the first dot of the name.
_DEVICE_NAME = re.compile(r"(con|prn|aux|nul|com[0-9]|lpt[0-9])", re.IGNORECASE)
_PLUGIN_ID = re.compile(r"[a-z0-9.-]+")
_SHA512 = re.compile(r"[0-9a-f]{128}")


def is_date(value: Any) -> bool:
    """``YYYY-MM-DD`` and a day that exists."""
    found = _DATE.fullmatch(value) if isinstance(value, str) else None
    if found is None:
        return False
    try:
        datetime.date(*(int(part) for part in found.groups()))
    except ValueError:
        return False
    return True


def is_timestamp(value: Any) -> bool:
    """``YYYY-MM-DDTHH:MM:SSZ`` and a time that exists."""
    found = _TIMESTAMP.fullmatch(value) if isinstance(value, str) else None
    if found is None:
        return False
    year, month, day, hour, minute, second = (int(p) for p in found.groups())
    try:
        datetime.datetime(year, month, day, hour, minute, second)
    except ValueError:
        return False
    return True


def is_file_name(value: Any) -> bool:
    """1 to 255 characters of letters, digits and ``._+~-``, not starting or
    ending with a dot and not a device name of Windows: a file name on every
    system, and nothing that can be a path."""
    return (
        isinstance(value, str)
        and 1 <= len(value) <= MAX_NAME_LENGTH
        and _FILE_NAME.fullmatch(value) is not None
        and not value.startswith(".")
        and not value.endswith(".")
        and _DEVICE_NAME.fullmatch(value.split(".", 1)[0]) is None
    )


def url_problem(value: Any) -> Optional[str]:
    """Why ``value`` is not an address the feed allows, or ``None``."""
    if not isinstance(value, str):
        return "must be a string"
    if not 1 <= len(value) <= MAX_URL_LENGTH:
        return "must be 1 to %d characters" % MAX_URL_LENGTH
    if any(not 0x21 <= ord(c) <= 0x7E for c in value):
        return "must be printable ASCII with no space"
    if not value.startswith("https://"):
        return "must start with https://"
    if "\\" in value:
        return "must not hold a backslash"
    authority = value[len("https://"):].split("/", 1)[0]
    if "@" in authority:
        return "must not hold a user name or password"
    # The host runs up to a port, a query or a fragment.
    if not re.split(r"[:?#]", authority, maxsplit=1)[0]:
        return "must name a host after https://"
    return None


def summary_problem(value: Any) -> Optional[str]:
    """Why ``value`` is not a summary the feed allows, or ``None``."""
    if not isinstance(value, str):
        return "must be a string"
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        return "must be text that has a UTF-8 form"
    if size > MAX_SUMMARY_BYTES:
        return "must be at most %d bytes in UTF-8" % MAX_SUMMARY_BYTES
    for c in value:
        point = ord(c)
        if c != "\n" and (point < 0x20 or 0x7F <= point <= 0x9F):
            return "must hold no control character other than a line feed"
    return None


def _check_text(problems: List[str], where: str, text: Optional[str]) -> None:
    if text is not None:
        problems.append("%s: %s" % (where, text))


def _check_object(
    value: Any,
    where: str,
    required: Tuple[str, ...],
    optional: Tuple[str, ...],
    problems: List[str],
) -> Optional[Dict[str, Any]]:
    """``value`` as an object with every required key and no key that is not
    listed. The object is returned, with problems noted, so that the values
    that are there are checked as well."""
    if not isinstance(value, dict):
        problems.append("%s: must be an object" % where)
        return None
    for key in value:
        if key not in required and key not in optional:
            problems.append("%s: unknown key %s" % (where, quoted(key)))
    for key in required:
        if key not in value:
            problems.append("%s: the key %s is missing" % (where, quoted(key)))
    return value


def _check_list(
    value: Any, where: str, low: int, high: int, problems: List[str]
) -> Optional[List[Any]]:
    if not isinstance(value, list):
        problems.append("%s: must be a list" % where)
        return None
    if not low <= len(value) <= high:
        problems.append("%s: must hold %d to %d entries" % (where, low, high))
        return None
    return value


def _check_file(
    value: Any, where: str, keys: Tuple[str, ...], problems: List[str]
) -> Optional[Tuple[Any, Any]]:
    """One file entry. ``keys`` starts with ``kind`` or ``os``. Returns the
    pair that must be unique in its list."""
    entry = _check_object(value, where, keys, (), problems)
    if entry is None:
        return None
    first = keys[0]
    allowed = KINDS if first == "kind" else OSES
    if first in entry and entry[first] not in allowed:
        problems.append(
            "%s.%s: must be one of %s" % (where, first, ", ".join(allowed))
        )
    if "arch" in entry and entry["arch"] not in ARCHES:
        problems.append("%s.arch: must be one of %s" % (where, ", ".join(ARCHES)))
    if "name" in entry:
        if not is_file_name(entry["name"]):
            problems.append(
                "%s.name: must be 1 to %d characters of letters, digits and "
                "._+~-, not start or end with a dot, and not be a device name "
                "of Windows" % (where, MAX_NAME_LENGTH)
            )
    if "url" in entry:
        _check_text(problems, where + ".url", url_problem(entry["url"]))
    if "size" in entry:
        size = entry["size"]
        if not ps.is_whole_number(size) or not 1 <= size <= MAX_SIZE:
            problems.append(
                "%s.size: must be a whole number from 1 to %d" % (where, MAX_SIZE)
            )
    if "sha512" in entry:
        digest = entry["sha512"]
        if not isinstance(digest, str) or _SHA512.fullmatch(digest) is None:
            problems.append(
                "%s.sha512: must be 128 lower-case hex characters" % where
            )
    return (entry.get(first), entry.get("arch"))


def _check_files(
    value: Any,
    where: str,
    low: int,
    high: int,
    keys: Tuple[str, ...],
    problems: List[str],
) -> None:
    files = _check_list(value, where, low, high, problems)
    if files is None:
        return
    seen: List[Tuple[Any, Any]] = []
    for index, item in enumerate(files):
        pair = _check_file(item, "%s[%d]" % (where, index), keys, problems)
        if pair is None:
            continue
        if pair in seen:
            problems.append(
                "%s: holds two files for %s and %s"
                % (where, quoted(pair[0]), quoted(pair[1]))
            )
        else:
            seen.append(pair)


def _check_release_fields(
    entry: Dict[str, Any], where: str, problems: List[str]
) -> None:
    """The fields that a studio entry and a plugin entry share."""
    if "released" in entry and not is_date(entry["released"]):
        problems.append("%s.released: must be a date written YYYY-MM-DD" % where)
    if "summary" in entry:
        _check_text(problems, where + ".summary", summary_problem(entry["summary"]))
    if "notes_url" in entry:
        _check_text(problems, where + ".notes_url", url_problem(entry["notes_url"]))


def _check_studio(value: Any, problems: List[str]) -> None:
    entries = _check_list(value, "studio", 0, MAX_STUDIO_ENTRIES, problems)
    if entries is None:
        return
    channels: List[str] = []
    for index, item in enumerate(entries):
        where = "studio[%d]" % index
        entry = _check_object(
            item,
            where,
            ("channel", "version", "released", "notes_url", "files"),
            ("summary",),
            problems,
        )
        if entry is None:
            continue
        channel = entry.get("channel")
        if "channel" in entry:
            if channel not in CHANNELS:
                problems.append(
                    "%s.channel: must be %s" % (where, " or ".join(CHANNELS))
                )
            elif channel in channels:
                problems.append(
                    "studio: holds two entries for the channel %s" % channel
                )
            else:
                channels.append(channel)
        if "version" in entry:
            reason = version_problem(entry["version"])
            _check_text(problems, where + ".version", reason)
            if reason is None and channel in CHANNELS:
                pre = is_prerelease(entry["version"])
                if channel == "stable" and pre:
                    problems.append(
                        "%s.version: a stable version cannot be a pre-release"
                        % where
                    )
                if channel == "preview" and not pre:
                    problems.append(
                        "%s.version: a preview version must be a pre-release"
                        % where
                    )
        _check_release_fields(entry, where, problems)
        if "files" in entry:
            _check_files(
                entry["files"], where + ".files", 1, MAX_STUDIO_FILES,
                STUDIO_FILE_KEYS, problems,
            )


def _check_plugins(value: Any, problems: List[str]) -> None:
    entries = _check_list(value, "plugins", 0, MAX_PLUGINS, problems)
    if entries is None:
        return
    ids: List[str] = []
    for index, item in enumerate(entries):
        where = "plugins[%d]" % index
        entry = _check_object(
            item,
            where,
            ("id", "version", "released", "requires_studio", "notes_url", "files"),
            ("summary",),
            problems,
        )
        if entry is None:
            continue
        if "id" in entry:
            plugin_id = entry["id"]
            if (
                not isinstance(plugin_id, str)
                or not plugin_id.startswith(PLUGIN_ID_PREFIX)
                or len(plugin_id) == len(PLUGIN_ID_PREFIX)
                or len(plugin_id) > MAX_PLUGIN_ID_LENGTH
                or _PLUGIN_ID.fullmatch(plugin_id) is None
            ):
                problems.append(
                    "%s.id: must start with %s, name a plugin after it and be "
                    "at most %d characters of a-z, 0-9, . and -"
                    % (where, PLUGIN_ID_PREFIX, MAX_PLUGIN_ID_LENGTH)
                )
            elif plugin_id in ids:
                problems.append("plugins: holds the id %s twice" % quoted(plugin_id))
            else:
                ids.append(plugin_id)
        for name in ("version", "requires_studio"):
            if name in entry:
                _check_text(
                    problems, "%s.%s" % (where, name), version_problem(entry[name])
                )
        _check_release_fields(entry, where, problems)
        if "files" in entry:
            _check_files(
                entry["files"], where + ".files", 0, MAX_PLUGIN_FILES,
                PLUGIN_FILE_KEYS, problems,
            )


def check_payload(text: Union[str, bytes]) -> List[str]:
    """What the reader in Studio would refuse in this payload, one line for
    each problem. An empty list means that it reads the payload."""
    try:
        data = text.encode("utf-8") if isinstance(text, str) else text
    except UnicodeEncodeError:
        return ["payload: is not valid UTF-8"]
    try:
        document = ps.load_strict(data)
    except ValueError as error:
        return ["payload: %s" % error]

    problems: List[str] = []
    root = _check_object(
        document, "payload", TOP_KEYS[:4], TOP_KEYS[4:], problems
    )
    if root is None:
        return problems
    if "feed_version" in root and (
        not ps.is_whole_number(root["feed_version"])
        or root["feed_version"] != FEED_VERSION
    ):
        problems.append("feed_version: must be the whole number %d" % FEED_VERSION)
    if "serial" in root and (
        not ps.is_whole_number(root["serial"])
        or not 1 <= root["serial"] <= MAX_FEED_SERIAL
    ):
        problems.append(
            "serial: must be a whole number from 1 to %d" % MAX_FEED_SERIAL
        )
    if "issued_at" in root and not is_timestamp(root["issued_at"]):
        problems.append("issued_at: must be a UTC time written YYYY-MM-DDTHH:MM:SSZ")
    if "studio" in root:
        _check_studio(root["studio"], problems)
    if "plugins" in root:
        _check_plugins(root["plugins"], problems)
    return problems


# -- The release folders -----------------------------------------------------


class Release(NamedTuple):
    version: str
    released: str
    notes_url: str
    summary: Optional[str]
    files: List[Dict[str, Any]]


_SUM_LINE = re.compile(r"([0-9a-fA-F]{128}) [ *](.+)")


def parse_sums(data: bytes, where: str) -> Dict[str, str]:
    """The lines of a ``SHA512SUMS`` as a map from file name to the lower-case
    hash. ``*`` may stand before the name (binary mode)."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise Refusal("%s is not valid UTF-8" % where) from None
    sums: Dict[str, str] = {}
    for number, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r")
        if not line:
            continue
        found = _SUM_LINE.fullmatch(line)
        if found is None:
            raise Refusal(
                "%s, line %d, is not a hash, two spaces and a file name"
                % (where, number)
            )
        name, digest = found.group(2), found.group(1).lower()
        if sums.get(name, digest) != digest:
            raise Refusal(
                "%s gives two different hashes for %s" % (where, shown_path(name))
            )
        sums[name] = digest
    return sums


# GitHub changes the name of a release file when it is uploaded: every
# character other than a letter, a digit, a dot, a hyphen and an underscore
# becomes a dot. The deb of a pre-release has a tilde in its name
# (0.1.0~rc.5), so SHA512SUMS, which was written before the upload, names
# that file differently from the release.
_NOT_KEPT_BY_GITHUB = re.compile(r"[^A-Za-z0-9._-]")


def uploaded_name(name: str) -> str:
    """The name GitHub gives a release file called ``name``."""
    return _NOT_KEPT_BY_GITHUB.sub(".", name)


def find_sum(sums: Dict[str, str], name: str, where: str) -> Optional[str]:
    """The hash of the release file ``name``: the line with that name, or
    else the line whose name GitHub changed to it. ``None`` when there is no
    such line; a refusal when two lines with different hashes fit."""
    if name in sums:
        return sums[name]
    found = sorted(
        (listed, digest)
        for listed, digest in sums.items()
        if uploaded_name(listed) == name
    )
    if len({digest for _, digest in found}) > 1:
        raise Refusal(
            "%s: SHA512SUMS has more than one line that could be %s: %s"
            % (where, shown_path(name), ", ".join(shown_path(n) for n, _ in found))
        )
    return found[0][1] if found else None


def match_asset(name: str) -> Optional[Tuple[int, str, str]]:
    """The place in ``FILE_RULES``, the kind and the architecture of the
    first rule that the name matches, or ``None``."""
    for index, (pattern, kind, arch) in enumerate(_RULES):
        if pattern.search(name):
            return index, kind, arch
    return None


def _field(document: Dict[str, Any], name: str, kind: type, where: str) -> Any:
    value = document.get(name)
    if not isinstance(value, kind):
        raise Refusal("%s: %s must be a %s" % (where, name, kind.__name__))
    return value


def load_release(folder: str, channel: str) -> Release:
    """Reads a release folder for the channel ``stable`` or ``preview``."""
    if not os.path.isdir(folder):
        raise Refusal("the release folder %s was not found" % shown_path(folder), EXIT_USAGE)
    json_path = os.path.join(folder, "release.json")
    sums_path = os.path.join(folder, "SHA512SUMS")
    summary_path = os.path.join(folder, "summary.txt")
    where = "the %s release" % channel

    try:
        document = ps.load_strict(ps.read_file(json_path, "the release file"))
    except ValueError as error:
        raise Refusal("%s %s" % (shown_path(json_path), error)) from None
    if not isinstance(document, dict):
        raise Refusal("%s is not a JSON object" % shown_path(json_path))
    tag = _field(document, "tag", str, where)
    flagged = _field(document, "prerelease", bool, where)
    published_at = _field(document, "published_at", str, where)
    notes_url = _field(document, "html_url", str, where)
    assets = _field(document, "assets", list, where)

    version = tag[1:] if tag.startswith("v") else tag
    reason = version_problem(version)
    if reason is not None:
        raise Refusal("%s: the tag %s %s" % (where, quoted(tag), reason))
    pre = is_prerelease(version)
    if channel == "stable" and (flagged or pre):
        raise Refusal(
            "%s is %s, which is a pre-release; give it as --preview"
            % (shown_path(folder), version)
        )
    if channel == "preview" and not (flagged or pre):
        raise Refusal(
            "%s is %s, which is not a pre-release; give it as --stable"
            % (shown_path(folder), version)
        )
    if channel == "preview" and not pre:
        raise Refusal(
            "%s is flagged as a pre-release, but its version %s has no "
            "pre-release part" % (shown_path(folder), version)
        )
    released, separator, _ = published_at.partition("T")
    if not separator or not is_date(released):
        raise Refusal(
            "%s: published_at %s must start with a date and T"
            % (where, quoted(published_at))
        )

    summary: Optional[str] = None
    if os.path.isfile(summary_path):
        try:
            text = ps.read_file(summary_path, "the summary").decode("utf-8")
        except UnicodeDecodeError:
            raise Refusal("%s is not valid UTF-8" % shown_path(summary_path)) from None
        # Line ends of the platform are not part of the text, and neither is
        # the line end that a text file has after its last line.
        text = text.replace("\r\n", "\n").rstrip("\n")
        summary = text if text else None

    sums = parse_sums(ps.read_file(sums_path, "the checksum file"), "SHA512SUMS of " + where)

    found: Dict[Tuple[str, str], Tuple[int, str]] = {}
    files: List[Tuple[int, Dict[str, Any]]] = []
    for index, asset in enumerate(assets):
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise Refusal("%s: assets[%d] has no name" % (where, index))
        name = asset["name"]
        matched = match_asset(name)
        if matched is None:
            continue
        place, kind, arch = matched
        if (kind, arch) in found:
            raise Refusal(
                "%s: %s and %s are both the %s for %s"
                % (where, shown_path(found[(kind, arch)][1]), shown_path(name), kind, arch)
            )
        found[(kind, arch)] = (place, name)
        digest = find_sum(sums, name, where)
        if digest is None:
            raise Refusal(
                "%s: %s has no line in SHA512SUMS" % (where, shown_path(name))
            )
        files.append(
            (
                place,
                {
                    "kind": kind,
                    "arch": arch,
                    "name": name,
                    "url": asset.get("url"),
                    "size": asset.get("size"),
                    "sha512": digest,
                },
            )
        )
    if not files:
        raise Refusal("%s has no file that a Studio install uses" % where)
    files.sort(key=lambda item: item[0])
    return Release(version, released, notes_url, summary, [f for _, f in files])


def studio_entry(release: Release, channel: str) -> Dict[str, Any]:
    entry: Dict[str, Any] = {
        "channel": channel,
        "version": release.version,
        "released": release.released,
    }
    if release.summary is not None:
        entry["summary"] = release.summary
    entry["notes_url"] = release.notes_url
    entry["files"] = release.files
    return entry


# -- The payload -------------------------------------------------------------


def _in_order(value: Any, order: Tuple[str, ...]) -> Any:
    """An object with its keys in ``order``, and any other key after them (so
    that ``check_payload`` can name it). Anything else is returned as it is."""
    if not isinstance(value, dict):
        return value
    known = {key: value[key] for key in order if key in value}
    known.update({key: v for key, v in value.items() if key not in known})
    return known


def canonical_plugins(plugins: Any) -> Any:
    """The ``plugins`` array with the keys of every entry in the order of the
    design. What cannot be put in order is left for ``check_payload``."""
    if not isinstance(plugins, list):
        return plugins
    result = []
    for plugin in plugins:
        plugin = _in_order(plugin, PLUGIN_KEYS)
        if isinstance(plugin, dict) and isinstance(plugin.get("files"), list):
            plugin["files"] = [_in_order(f, PLUGIN_FILE_KEYS) for f in plugin["files"]]
        result.append(plugin)
    return result


def payload_text(document: Dict[str, Any]) -> str:
    """The text of a payload: two-space indent, line feeds, one at the end,
    and characters other than ASCII as they are."""
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def build_payload(
    serial: int,
    issued_at: str,
    studio: List[Dict[str, Any]],
    plugins: Any,
) -> str:
    document = {
        "feed_version": FEED_VERSION,
        "serial": serial,
        "issued_at": issued_at,
        "studio": studio,
        "plugins": canonical_plugins(plugins),
    }
    return payload_text(document)


def refuse_problems(problems: List[str], what: str) -> None:
    if problems:
        raise Refusal(
            "the %s would be refused by Studio:\n  %s" % (what, "\n  ".join(problems))
        )


# -- Commands ----------------------------------------------------------------


def command_payload(args: argparse.Namespace) -> int:
    if args.stable is None and args.preview is None:
        raise Refusal("give --stable, --preview or both", EXIT_USAGE)
    plugins: Any = []
    if args.plugins is not None:
        try:
            plugins = ps.load_strict(ps.read_file(args.plugins, "the plugins file"))
        except ValueError as error:
            raise Refusal("%s %s" % (shown_path(args.plugins), error)) from None
        if not isinstance(plugins, list):
            raise Refusal(
                "%s must hold the plugins array, as a JSON list" % shown_path(args.plugins)
            )
    protected = [args.plugins] if args.plugins is not None else []
    ps.check_output(args.out, protected)

    stable = load_release(args.stable, "stable") if args.stable is not None else None
    preview = load_release(args.preview, "preview") if args.preview is not None else None
    if stable is not None and preview is not None:
        if compare_versions(preview.version, stable.version) <= 0:
            print(
                "update_feed: the preview release %s is not newer than the stable "
                "release %s, so the feed has no preview entry"
                % (preview.version, stable.version),
                file=sys.stderr,
            )
            preview = None
    studio = []
    if stable is not None:
        studio.append(studio_entry(stable, "stable"))
    if preview is not None:
        studio.append(studio_entry(preview, "preview"))

    text = build_payload(args.serial, args.issued_at, studio, plugins)
    refuse_problems(check_payload(text), "payload")
    try:
        with open(args.out, "wb") as handle:
            handle.write(text.encode("utf-8"))
    except OSError as error:
        raise Refusal(
            "could not write %s (%s)" % (shown_path(args.out), error.strerror), EXIT_USAGE
        ) from None
    print("wrote %s" % args.out)
    return EXIT_OK


def command_sign(args: argparse.Namespace) -> int:
    ps.check_key_id(args.key_id)
    ps.require_key_file(args.key)
    payload = ps.read_file(args.payload, "the payload file")
    refuse_problems(check_payload(payload), "payload")
    ps.check_output(args.out, [args.key, args.payload])
    openssl = ps.find_openssl(args.openssl)
    text = ps.sign_envelope(openssl, args.key, PURPOSE, args.key_id, payload)
    ps.write_text(args.out, text)
    print("wrote %s" % args.out)
    return EXIT_OK


_PUBLIC_KEY_HEX = re.compile(r"[0-9a-fA-F]{64}")


def command_verify(args: argparse.Namespace) -> int:
    if _PUBLIC_KEY_HEX.fullmatch(args.public_key) is None:
        raise Refusal("--public-key must be 64 hex characters", EXIT_USAGE)
    public_key = bytes.fromhex(args.public_key)
    data = ps.read_file(args.file, "the update feed")
    if len(data) > MAX_ENVELOPE_SIZE:
        raise Refusal(
            "%s is larger than %d bytes, which Studio does not read"
            % (shown_path(args.file), MAX_ENVELOPE_SIZE)
        )
    try:
        found = ps.parse_envelope(data)
    except ValueError as error:
        raise Refusal("%s %s" % (shown_path(args.file), error)) from None
    if found.purpose != PURPOSE:
        raise Refusal(
            "the file is signed as %s, not as %s" % (quoted(found.purpose), quoted(PURPOSE))
        )
    # Studio finds the key by the id in the file, so the right key under the
    # wrong id is a feed that Studio refuses.
    if args.key_id is not None and found.key_id != args.key_id:
        raise Refusal(
            "the file names the key %s, not %s" % (quoted(found.key_id), quoted(args.key_id))
        )
    openssl = ps.find_openssl(args.openssl)
    message = ps.signed_message(found.purpose, found.payload)
    if not ps.verify_bytes(openssl, public_key, message, found.signature):
        raise Refusal("the signature does not hold")
    problems = check_payload(found.payload)
    if problems:
        more = " (and %d more)" % (len(problems) - 1) if len(problems) > 1 else ""
        raise Refusal(
            "the signature holds, but the payload is not valid: %s%s"
            % (problems[0], more)
        )
    serial = ps.load_strict(found.payload)["serial"]
    if args.min_serial is not None and serial < args.min_serial:
        raise Refusal(
            "the serial %d is lower than the lowest allowed, %d" % (serial, args.min_serial)
        )
    print("ok serial %d" % serial)
    return EXIT_OK


# -- Command line ------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="update_feed.py",
        description="Build, sign and verify the update feed that Studio reads. "
        "It never makes, writes or prints a key.",
        allow_abbrev=False,
    )
    commands = parser.add_subparsers(dest="command", required=True)

    def add(name: str, help_text: str, function: Any) -> argparse.ArgumentParser:
        sub = commands.add_parser(name, help=help_text, allow_abbrev=False)
        sub.set_defaults(handler=function)
        return sub

    openssl_help = "the OpenSSL program, if not on PATH"

    sub = add("payload", "write the payload from release folders", command_payload)
    sub.add_argument("--serial", required=True, type=int, help="the serial of the feed")
    sub.add_argument("--issued-at", required=True, help="UTC, as 2026-12-01T03:40:00Z")
    sub.add_argument("--stable", help="the folder of the newest final release")
    sub.add_argument("--preview", help="the folder of the newest pre-release")
    sub.add_argument("--plugins", help="a JSON file with the plugins array")
    sub.add_argument("--out", required=True, help="the payload file to write")

    sub = add("sign", "sign a payload and write the feed", command_sign)
    sub.add_argument("--key", required=True, help="the private key file")
    sub.add_argument("--key-id", required=True, help="the id of the key")
    sub.add_argument("--payload", required=True, help="the payload file")
    sub.add_argument("--out", required=True, help="the feed file to write")
    sub.add_argument("--openssl", metavar="PATH", help=openssl_help)

    sub = add("verify", "check the signature and the payload of a feed", command_verify)
    sub.add_argument("--public-key", required=True, help="64 hex characters")
    sub.add_argument("--key-id", help="refuse a feed that names another key")
    sub.add_argument("--min-serial", type=int, help="refuse a lower serial")
    sub.add_argument("--openssl", metavar="PATH", help=openssl_help)
    sub.add_argument("file", help="the feed file")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        return int(arguments.handler(arguments))
    except Refusal as refusal:
        print("update_feed: %s" % refusal.message, file=sys.stderr)
        return refusal.code


if __name__ == "__main__":
    sys.exit(main())
