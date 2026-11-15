#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build, sign and verify the update feed that Studio reads.

The update feed is a small signed file that says which Studio (and which
official plugins) are current (docs/updates.md, section 4). This tool makes
its payload from the files of a release, signs the payload and checks a
finished feed. Only ``collect`` can make a network request, and only when it
is given ``--repo`` alone; the other commands work from files, so the release
job and the tests feed them the same way.

    python3 tools/update_feed.py collect --repo owner/name --out feed-input \\
        [--releases-json releases.json]
    python3 tools/update_feed.py payload --serial 12 \\
        --issued-at 2026-12-01T03:40:00Z --stable feed-input/stable \\
        [--preview feed-input/preview] [--plugins plugins.json] \\
        --out feed.payload.json
    python3 tools/update_feed.py sign --key upd-2026-a.key \\
        --key-id upd-2026-a --payload feed.payload.json \\
        --out update-feed.json
    python3 tools/update_feed.py verify --public-key <64 hex characters> \\
        [--key-id upd-2026-a] [--min-serial 12] update-feed.json
    python3 tools/update_feed.py verify \\
        --keys-from libs/vss_update/src/trusted_keys_release.cpp update-feed.json
    python3 tools/update_feed.py keys \\
        --keys-from libs/vss_update/src/trusted_keys_release.cpp

``collect`` reads the releases of a repository as the GitHub API gives them
(``GET /repos/{owner}/{repo}/releases``; ``--releases-json`` names a saved
response instead of asking), ignores drafts, picks the newest final release
and the newest pre-release by the version in the tag, and writes a release
folder for each into ``<out>/stable`` and ``<out>/preview``, which are what
``payload`` reads. It also writes ``<out>/serial.json`` and prints the serial,
the number of published releases (docs/updates.md, section 10).

A release folder holds ``release.json`` (the shape of website/data/release.json:
tag, prerelease, published_at, html_url and the assets with name, size and url),
``SHA512SUMS`` (as ``sha512sum`` writes it) and, if there is one,
``summary.txt`` (plain text, UTF-8: the summary of the release).

The keys Studio trusts are read from ``trusted_keys_release.cpp`` by
``--keys-from``, which holds the text convention described at ``read_feed_keys``
and in the comment of that file. ``keys`` prints the key ids in the file, one
for each line, and nothing when it holds none.

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
missing, a bad key id, or no usable OpenSSL, and 3 when ``verify --keys-from``
finds that the file names no key at all (the build trusts no feed key, which
is not a bad signature). Messages go to standard error; only results go to
standard output.

OpenSSL 3.0 or later is needed to sign and to verify. A key file with a
passphrase is not supported. Standard library only, Python 3.9 or later.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import sys
import urllib.request
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Tuple, Union

import plugin_sign as ps
import site_build
from plugin_sign import (
    EXIT_FAILED,
    EXIT_OK,
    EXIT_USAGE,
    MAX_ENVELOPE_SIZE,
    Refusal,
    quoted,
    shown_path,
)

EXIT_NO_KEYS = 3

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


# -- The keys Studio is built with -------------------------------------------


class FeedKey(NamedTuple):
    key_id: str
    public_key: bytes


class NoKeys(Refusal):
    """The file lists no key. It has its own exit status, so that a job can
    tell a build that trusts no feed key from a feed that does not verify."""

    def __init__(self, message: str) -> None:
        super().__init__(message, EXIT_NO_KEYS)


KEYS_FILE_LIMIT = 256 * 1024
KEY_BYTES = 32

# What a comment or a literal looks like. A comment is blanked and a literal
# is kept, so that "//" in a string is not a comment.
_COMMENT_OR_LITERAL = re.compile(
    r"""//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'""", re.DOTALL
)
_KEY_DECLARATION = re.compile(
    r"\bconstexpr\s+(?:\w+::)*public_key\s+([A-Za-z_]\w*)\s*\{([^{}]*)\}\s*;", re.ASCII
)
_ANY_KEY_DECLARATION = re.compile(r"\bconstexpr\b[^;{}]*\bpublic_key\b", re.ASCII)
_MAKE_CALL = re.compile(r"\bmake\s*\(", re.ASCII)
_KEY_ENTRY = re.compile(
    r"""make\s*\(\s*"([^"\\]*)"\s*,\s*(?:\w+::)*feed_purpose\s*,"""
    r"""\s*([A-Za-z_]\w*)\s*\)""",
    re.ASCII,
)
_KEY_LIST = re.compile(
    r"std::array\s*<\s*(?:\w+::)*trusted_key\s*,\s*([0-9]+)\s*>", re.ASCII
)
_EMPTY_LIST = re.compile(r"\breturn\s*\{\s*\}\s*;", re.ASCII)
_KEY_WINDOW = re.compile(r"\b(not_before|not_after|revoked_from)\b", re.ASCII)
_KEY_BYTE = re.compile(r"0[xX][0-9a-fA-F]{2}")


def _code_of(text: str, where: str) -> str:
    """``text`` without its comments. An unfinished comment is refused."""

    def blank(found: "re.Match[str]") -> str:
        literal = found.group(0)
        return literal if literal[0] in "\"'" else " "

    code = _COMMENT_OR_LITERAL.sub(blank, text)
    if "/*" in _COMMENT_OR_LITERAL.sub(" ", code):
        raise Refusal("%s has a comment that is not closed" % where, EXIT_USAGE)
    return code


def read_feed_keys(path: str) -> List[FeedKey]:
    """The feed keys that ``libs/vss_update/src/trusted_keys_release.cpp``
    lists, in the order of the list: the first is the key that signs.

    The file is C++, and this reads it as text, so it takes only the form
    that the file's own comment describes (comments are ignored):

    * Each key is an array ``constexpr public_key NAME{0xHH, ..., 0xHH};``
      with exactly 32 bytes, each written as ``0x`` and two hex digits,
      separated by commas, and no comma after the last. The type may have a
      namespace in front of it.
    * Each key is listed once, as ``make("KEY-ID", feed_purpose, NAME)``,
      in the ``std::array<trusted_key, N>`` that ``builtin_feed_keys``
      returns, where N is the number of entries. A key id is 1 to 64 lower
      case letters, digits and hyphens, and is not used twice.
    * With no key the function holds ``return {};`` and the file has no
      array of keys and no entry.
    * A key window (``not_before``, ``not_after``, ``revoked_from``) is not
      read, so a file that names one is refused: a key that has a window
      must not be taken for one that has none.

    Anything else that the reader cannot account for is refused with
    ``EXIT_USAGE``: a key of another length, an entry whose array is not
    there, an array that no entry uses, a second purpose or an entry of
    another shape.
    """
    where = shown_path(path)
    data = ps.read_file(path, "the key file")
    if len(data) > KEYS_FILE_LIMIT:
        raise Refusal("%s is larger than %d bytes" % (where, KEYS_FILE_LIMIT), EXIT_USAGE)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise Refusal("%s is not valid UTF-8" % where, EXIT_USAGE) from None
    code = _code_of(text, where)

    window = _KEY_WINDOW.search(code)
    if window is not None:
        raise Refusal(
            "%s names %s, which this tool does not read; it cannot tell what "
            "that key may sign" % (where, window.group(1)),
            EXIT_USAGE,
        )

    arrays: Dict[str, bytes] = {}
    declarations = _KEY_DECLARATION.findall(code)
    if len(_ANY_KEY_DECLARATION.findall(code)) != len(declarations):
        raise Refusal(
            "%s has a public_key that is not written as constexpr public_key "
            "NAME{0x.., ...};" % where,
            EXIT_USAGE,
        )
    for name, body in declarations:
        if name in arrays:
            raise Refusal("%s declares the key %s twice" % (where, name), EXIT_USAGE)
        items = [item.strip() for item in body.split(",")]
        if len(items) != KEY_BYTES or any(_KEY_BYTE.fullmatch(i) is None for i in items):
            raise Refusal(
                "%s: the key %s must be %d bytes written 0xHH and separated by "
                "commas (and no comma after the last)" % (where, name, KEY_BYTES),
                EXIT_USAGE,
            )
        arrays[name] = bytes(int(item, 16) for item in items)

    keys: List[FeedKey] = []
    used: List[str] = []
    for call in _MAKE_CALL.finditer(code):
        # The function that makes an entry is declared as `trusted_key make(`.
        if code[: call.start()].rstrip().endswith("trusted_key"):
            continue
        entry = _KEY_ENTRY.match(code, call.start())
        if entry is None:
            raise Refusal(
                "%s has an entry that is not make(\"KEY-ID\", feed_purpose, NAME): %s"
                % (where, quoted(code[call.start():call.start() + 60].splitlines()[0])),
                EXIT_USAGE,
            )
        key_id, name = entry.group(1), entry.group(2)
        if not ps.is_key_id(key_id):
            raise Refusal(
                "%s has the key id %s: 1 to 64 lower-case letters, digits and "
                "hyphens" % (where, quoted(key_id)),
                EXIT_USAGE,
            )
        if key_id in (k.key_id for k in keys):
            raise Refusal("%s lists the key id %s twice" % (where, key_id), EXIT_USAGE)
        if name not in arrays:
            raise Refusal(
                "%s lists the key id %s with %s, which is not a key array"
                % (where, key_id, name),
                EXIT_USAGE,
            )
        if name in used:
            raise Refusal("%s uses the key %s twice" % (where, name), EXIT_USAGE)
        used.append(name)
        keys.append(FeedKey(key_id, arrays[name]))

    unused = [name for name in arrays if name not in used]
    if unused:
        raise Refusal(
            "%s declares the key %s and lists no entry for it" % (where, unused[0]),
            EXIT_USAGE,
        )
    sizes = _KEY_LIST.findall(code)
    if keys:
        if [int(size) for size in sizes] != [len(keys)]:
            raise Refusal(
                "%s must hold one std::array<trusted_key, %d> for its %d entries"
                % (where, len(keys), len(keys)),
                EXIT_USAGE,
            )
    elif sizes or _EMPTY_LIST.search(code) is None:
        raise Refusal(
            "%s lists no key, but it is not the empty list (return {};) either"
            % where,
            EXIT_USAGE,
        )
    return keys


# -- Collecting the inputs from the releases ---------------------------------

API_TIMEOUT_SECONDS = 30
API_PAGE_SIZE = 100
API_MAX_PAGES = 20
API_USER_AGENT = "virtual-serial-studio-update-feed"
MAX_API_RESPONSE = 16 * 1024 * 1024
MAX_SUMS_BYTES = 1024 * 1024
SUMS_ASSET = "SHA512SUMS"
_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", re.ASCII)


def _read_limited(response: Any, limit: int, what: str) -> bytes:
    data = response.read(limit + 1)
    if len(data) > limit:
        raise Refusal("%s is larger than %d bytes" % (what, limit))
    return bytes(data)


def fetch_releases(repo: str) -> List[Any]:
    """All the releases of ``repo`` that the GitHub API lists, newest first.
    One request for each page of 100, with the token in GH_TOKEN or
    GITHUB_TOKEN if there is one. Nothing but the API host sees the token and
    no message shows it."""
    headers = {"Accept": "application/vnd.github+json", "User-Agent": API_USER_AGENT}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    if token:
        headers["Authorization"] = "Bearer " + token
    entries: List[Any] = []
    for page in range(1, API_MAX_PAGES + 1):
        request = urllib.request.Request(
            "https://api.github.com/repos/%s/releases?per_page=%d&page=%d"
            % (repo, API_PAGE_SIZE, page),
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
                body = _read_limited(response, MAX_API_RESPONSE, "the answer of GitHub")
            found = json.loads(body.decode("utf-8"))
        except (OSError, ValueError) as error:
            raise Refusal("could not read the releases of %s (%s)" % (repo, error)) from None
        if not isinstance(found, list):
            raise Refusal("GitHub did not answer with a list of releases")
        entries.extend(found)
        if len(found) < API_PAGE_SIZE:
            return entries
    raise Refusal("%s has more releases than this tool reads" % repo)


def download_bytes(url: str, limit: int) -> bytes:
    """A file of a release over HTTPS, at most ``limit`` bytes. No token is
    sent: a release file is public, and a redirect would pass a token on to
    another host."""
    if not url.startswith("https://"):
        raise Refusal("%s is not an https address" % shown_path(url))
    request = urllib.request.Request(url, headers={"User-Agent": API_USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
            return _read_limited(response, limit, shown_path(url))
    except OSError as error:
        raise Refusal("could not download %s (%s)" % (shown_path(url), error)) from None


_MARKDOWN_LINK = re.compile(r"\[([^\]\n]*)\]\([^)\s]*\)")
_MARKDOWN_HEADING = re.compile(r"^#{1,6}[ \t]+", re.MULTILINE)


def without_markdown(text: str) -> str:
    """Release notes are written in Markdown and Studio shows a summary as
    plain text, so the marks that would only be noise there are taken out: a
    link keeps its words and loses its address, a heading loses its ``#``,
    and ``**`` and the backtick go. Everything else stays as written."""
    text = _MARKDOWN_LINK.sub(lambda found: found.group(1), text)
    text = _MARKDOWN_HEADING.sub("", text)
    return text.replace("**", "").replace("`", "")


def summary_of(body: Any) -> Optional[str]:
    """The body of a release as the plain text of a feed summary, or ``None``
    for an empty body: the Markdown marks gone (``without_markdown``), line
    ends as a line feed, any other control character gone (a tab becomes a
    space), at most ``MAX_SUMMARY_BYTES`` bytes of UTF-8, with ``...`` where
    it was cut. It is cut at the end of a line when one is near the limit."""
    if not isinstance(body, str):
        return None
    kept = []
    plain = without_markdown(body.replace("\r\n", "\n").replace("\r", "\n"))
    for c in plain:
        point = ord(c)
        if c == "\n":
            kept.append(c)
        elif c == "\t":
            kept.append(" ")
        elif point < 0x20 or 0x7F <= point <= 0x9F or 0xD800 <= point <= 0xDFFF:
            continue
        else:
            kept.append(c)
    text = "".join(kept).strip()
    if not text:
        return None
    encoded = text.encode("utf-8")
    if len(encoded) > MAX_SUMMARY_BYTES:
        room = MAX_SUMMARY_BYTES - len("...")
        text = encoded[:room].decode("utf-8", "ignore")
        line_end = text.rfind("\n")
        if line_end > len(text) * 3 // 4:
            text = text[:line_end].rstrip() + "\n"
        else:
            text = text.rstrip()
        text += "..."
    return text


def _release_version(entry: Dict[str, Any]) -> Optional[str]:
    tag = entry.get("tag_name")
    if not isinstance(tag, str):
        return None
    version = tag[1:] if tag.startswith("v") else tag
    return version if is_version(version) else None


def pick_releases(
    entries: List[Any],
) -> Tuple[Optional[Dict[str, Any]], Optional[Dict[str, Any]], int]:
    """The newest final release, the newest pre-release and the number of
    published releases. A draft is not published. The newest is the greatest
    version of the tag, not the latest date; a tag that is not a version is
    not picked, though it counts as a release."""
    published = 0
    newest: Dict[bool, Optional[Dict[str, Any]]] = {False: None, True: None}
    for entry in entries:
        if not isinstance(entry, dict):
            raise Refusal("the list of releases holds something that is not a release")
        if entry.get("draft") is True:
            continue
        published += 1
        version = _release_version(entry)
        if version is None:
            continue
        pre = entry.get("prerelease") is True
        best = newest[pre]
        best_version = _release_version(best) if best is not None else None
        if best_version is None or compare_versions(version, best_version) > 0:
            newest[pre] = entry
    return newest[False], newest[True], published


def write_release_folder(
    entry: Dict[str, Any],
    repo: str,
    folder: str,
    download: Callable[[str, int], bytes],
) -> None:
    """Writes ``release.json``, ``SHA512SUMS`` and, if the body gives one,
    ``summary.txt`` for a release as ``load_release`` reads them."""
    tag = entry.get("tag_name")
    try:
        # The same conversion, with the same checks of every address, as the
        # website's record of the newest release.
        record = site_build.convert_release([entry], repo)
    except site_build.BuildError as error:
        raise Refusal("the release %s: %s" % (quoted(tag), error)) from None
    sums = next((a for a in record["assets"] if a["name"] == SUMS_ASSET), None)
    if sums is None:
        raise Refusal("the release %s has no %s" % (quoted(tag), SUMS_ASSET))
    sums_data = download(str(sums["url"]), MAX_SUMS_BYTES)
    summary = summary_of(entry.get("body"))
    try:
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "release.json"), "wb") as handle:
            handle.write((json.dumps(record, indent=1) + "\n").encode("utf-8"))
        with open(os.path.join(folder, SUMS_ASSET), "wb") as handle:
            handle.write(sums_data)
        if summary is not None:
            with open(os.path.join(folder, "summary.txt"), "wb") as handle:
                handle.write((summary + "\n").encode("utf-8"))
    except OSError as error:
        raise Refusal(
            "could not write in %s (%s)" % (shown_path(folder), error.strerror), EXIT_USAGE
        ) from None


def collect(
    repo: str,
    entries: List[Any],
    out: str,
    download: Callable[[str, int], bytes] = download_bytes,
) -> int:
    """Writes ``<out>/stable``, ``<out>/preview`` and ``<out>/serial.json``
    from the releases and returns the serial. A folder of an earlier run that
    this run does not write again is removed."""
    stable, preview, serial = pick_releases(entries)
    if stable is None and preview is None:
        raise Refusal("%s has no published release with a version in its tag" % repo)
    if stable is not None and preview is not None:
        stable_version = str(_release_version(stable))
        preview_version = str(_release_version(preview))
        if compare_versions(preview_version, stable_version) <= 0:
            print(
                "update_feed: the pre-release %s is not newer than the release %s, "
                "so it is left out" % (preview_version, stable_version),
                file=sys.stderr,
            )
            preview = None
    for name in ("stable", "preview", "serial.json"):
        old = os.path.join(out, name)
        try:
            if os.path.isdir(old):
                shutil.rmtree(old)
            elif os.path.isfile(old):
                os.remove(old)
        except OSError as error:
            raise Refusal(
                "could not remove %s (%s)" % (shown_path(old), error.strerror), EXIT_USAGE
            ) from None
    for name, entry in (("stable", stable), ("preview", preview)):
        if entry is not None:
            write_release_folder(entry, repo, os.path.join(out, name), download)
            print(
                "update_feed: %s is the release for the %s channel"
                % (entry.get("tag_name"), name),
                file=sys.stderr,
            )
    try:
        with open(os.path.join(out, "serial.json"), "wb") as handle:
            handle.write((json.dumps({"serial": serial}) + "\n").encode("utf-8"))
    except OSError as error:
        raise Refusal(
            "could not write in %s (%s)" % (shown_path(out), error.strerror), EXIT_USAGE
        ) from None
    return serial


# -- Commands ----------------------------------------------------------------


def command_collect(args: argparse.Namespace) -> int:
    if _REPOSITORY.fullmatch(args.repo) is None:
        raise Refusal("--repo must be written owner/name", EXIT_USAGE)
    if args.releases_json is not None:
        try:
            entries = ps.load_strict(ps.read_file(args.releases_json, "the releases file"))
        except ValueError as error:
            raise Refusal("%s %s" % (shown_path(args.releases_json), error)) from None
        if not isinstance(entries, list):
            raise Refusal(
                "%s must hold the list of releases that GitHub answers with"
                % shown_path(args.releases_json)
            )
    else:
        entries = fetch_releases(args.repo)
    serial = collect(args.repo, entries, args.out, download_bytes)
    print(serial)
    return EXIT_OK



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


def verify_feed(
    data: bytes,
    name: str,
    find_key: Callable[[str], bytes],
    key_id: Optional[str] = None,
    min_serial: Optional[int] = None,
    openssl_override: Optional[str] = None,
) -> int:
    """Checks a feed as Studio would and returns its serial. ``find_key``
    gives the public key for the key id that the file names, or refuses.
    Raises ``Refusal`` for every reason the feed is not accepted."""
    if len(data) > MAX_ENVELOPE_SIZE:
        raise Refusal(
            "%s is larger than %d bytes, which Studio does not read"
            % (shown_path(name), MAX_ENVELOPE_SIZE)
        )
    try:
        found = ps.parse_envelope(data)
    except ValueError as error:
        raise Refusal("%s %s" % (shown_path(name), error)) from None
    if found.purpose != PURPOSE:
        raise Refusal(
            "the file is signed as %s, not as %s" % (quoted(found.purpose), quoted(PURPOSE))
        )
    # Studio finds the key by the id in the file, so the right key under the
    # wrong id is a feed that Studio refuses.
    if key_id is not None and found.key_id != key_id:
        raise Refusal(
            "the file names the key %s, not %s" % (quoted(found.key_id), quoted(key_id))
        )
    public_key = find_key(found.key_id)
    openssl = ps.find_openssl(openssl_override)
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
    if min_serial is not None and serial < min_serial:
        raise Refusal(
            "the serial %d is lower than the lowest allowed, %d" % (serial, min_serial)
        )
    return int(serial)


def key_finder(keys: List[FeedKey]) -> Callable[[str], bytes]:
    """``find_key`` for a list of keys: the one whose id the feed names."""
    by_id = {key.key_id: key.public_key for key in keys}

    def find(key_id: str) -> bytes:
        if key_id not in by_id:
            raise Refusal(
                "the file names the key %s, which this build does not trust "
                "(it trusts %s)" % (quoted(key_id), ", ".join(by_id))
            )
        return by_id[key_id]

    return find


def trusted_keys(keys_path: str) -> List[FeedKey]:
    """The keys in the C++ file of the keys Studio is built with. Raises
    ``NoKeys`` when the file lists none: a build that trusts no feed key."""
    keys = read_feed_keys(keys_path)
    if not keys:
        raise NoKeys(
            "%s lists no update feed key, so this build trusts none"
            % shown_path(keys_path)
        )
    return keys


def verify_with_keys_file(
    data: bytes,
    name: str,
    keys_path: str,
    min_serial: Optional[int] = None,
    openssl_override: Optional[str] = None,
) -> int:
    """Checks a feed with the keys that ``keys_path`` lists and returns its
    serial. Raises ``NoKeys`` when the file lists no key and ``Refusal`` for
    any other reason that the feed is not accepted."""
    return verify_feed(
        data, name, key_finder(trusted_keys(keys_path)), None, min_serial,
        openssl_override,
    )


def command_verify(args: argparse.Namespace) -> int:
    if (args.public_key is None) == (args.keys_from is None):
        raise Refusal("give --public-key or --keys-from, and not both", EXIT_USAGE)
    if args.public_key is not None:
        if _PUBLIC_KEY_HEX.fullmatch(args.public_key) is None:
            raise Refusal("--public-key must be 64 hex characters", EXIT_USAGE)
        public_key = bytes.fromhex(args.public_key)

        def find_key(_key_id: str) -> bytes:
            return public_key

    else:
        # The keys come first: a build with none is not a bad signature, and
        # that is the answer whether or not the feed file is there.
        find_key = key_finder(trusted_keys(args.keys_from))
    data = ps.read_file(args.file, "the update feed")
    serial = verify_feed(
        data, args.file, find_key, args.key_id, args.min_serial, args.openssl
    )
    print("ok serial %d" % serial)
    return EXIT_OK


def command_keys(args: argparse.Namespace) -> int:
    for key in read_feed_keys(args.keys_from):
        print(key.key_id)
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

    sub = add(
        "collect", "write the release folders and the serial from the releases", command_collect
    )
    sub.add_argument("--repo", required=True, help="the repository, as owner/name")
    sub.add_argument(
        "--releases-json",
        metavar="FILE",
        help="a saved answer of the GitHub releases API, instead of asking GitHub",
    )
    sub.add_argument("--out", required=True, help="the folder to write")

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
    sub.add_argument("--public-key", help="64 hex characters")
    sub.add_argument(
        "--keys-from",
        metavar="FILE",
        help="the C++ file of the keys Studio is built with, instead of --public-key",
    )
    sub.add_argument("--key-id", help="refuse a feed that names another key")
    sub.add_argument("--min-serial", type=int, help="refuse a lower serial")
    sub.add_argument("--openssl", metavar="PATH", help=openssl_help)
    sub.add_argument("file", help="the feed file")

    sub = add("keys", "list the feed key ids of the keys file, one for each line", command_keys)
    sub.add_argument(
        "--keys-from",
        required=True,
        metavar="FILE",
        help="the C++ file of the keys Studio is built with",
    )
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
