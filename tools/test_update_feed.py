#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/update_feed.py.

Run with:

    python -m unittest discover -s tools -p "test_update_feed.py" -v

The tests that sign or verify call OpenSSL 3 and are skipped, with a message,
when it is not on the PATH. The rest is plain Python and always runs. The key
for the signing tests is a THROWAWAY key that OpenSSL makes in a temporary
folder when the tests start; no key file is in the repository. The files of
keys that the tests read are made in the tests, and no test makes a request:
the two calls of ``collect`` are replaced.
"""

from __future__ import annotations

import base64
import contextlib
import copy
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
TOOL = HERE / "update_feed.py"

sys.path.insert(0, str(HERE))

import plugin_sign as ps  # noqa: E402
import update_feed as uf  # noqa: E402


def openssl_problem() -> str:
    """Why OpenSSL 3 cannot be used here, or an empty text."""
    try:
        ps.find_openssl()
    except ps.Refusal as refusal:
        return refusal.message
    return ""


OPENSSL_PROBLEM = openssl_problem()
needs_openssl = unittest.skipIf(
    bool(OPENSSL_PROBLEM), "OpenSSL 3 is not on the PATH: " + OPENSSL_PROBLEM
)

_KEY_DIR: Optional[Path] = None
KEY: Optional[Path] = None
OTHER_KEY: Optional[Path] = None
PUBLIC_KEY = ""
OTHER_PUBLIC_KEY = ""


def setUpModule() -> None:
    """Makes two throwaway keys with OpenSSL, when there is OpenSSL."""
    global _KEY_DIR, KEY, OTHER_KEY, PUBLIC_KEY, OTHER_PUBLIC_KEY
    if OPENSSL_PROBLEM:
        return
    openssl = ps.find_openssl()
    _KEY_DIR = Path(tempfile.mkdtemp(prefix="update-feed-key-"))
    keys = []
    for name in ("first.pem", "second.pem"):
        path = _KEY_DIR / name
        subprocess.run(
            [openssl, "genpkey", "-algorithm", "ed25519", "-out", str(path)],
            check=True,
            capture_output=True,
        )
        keys.append(path)
    KEY, OTHER_KEY = keys
    PUBLIC_KEY = ps.public_key_of(openssl, str(KEY)).hex()
    OTHER_PUBLIC_KEY = ps.public_key_of(openssl, str(OTHER_KEY)).hex()


def tearDownModule() -> None:
    if _KEY_DIR is not None:
        shutil.rmtree(_KEY_DIR, ignore_errors=True)


# -- A good payload, and the ways to spoil it --------------------------------

SITE = "https://github.com/iainchesworthlabs/virtual-serial-studio/releases"


def file_entry(kind: str = "windows-installer", arch: str = "x86_64", **changes: Any) -> Dict[str, Any]:
    name = "virtual-serial-studio-v0.2.0-%s.bin" % kind
    entry: Dict[str, Any] = {
        "kind": kind,
        "arch": arch,
        "name": name,
        "url": "%s/download/v0.2.0/%s" % (SITE, name),
        "size": 50132336,
        "sha512": "ab" * 64,
    }
    entry.update(changes)
    return entry


def plugin_file(os_name: str = "windows", arch: str = "x86_64", **changes: Any) -> Dict[str, Any]:
    entry: Dict[str, Any] = {
        "os": os_name,
        "arch": arch,
        "name": "decoding-1.3.0-%s.zip" % os_name,
        "url": "https://example.com/decoding-1.3.0-%s.zip" % os_name,
        "size": 123456,
        "sha512": "0f" * 64,
    }
    entry.update(changes)
    return entry


def good_payload() -> Dict[str, Any]:
    return {
        "feed_version": 1,
        "serial": 12,
        "issued_at": "2026-12-01T03:40:00Z",
        "studio": [
            {
                "channel": "stable",
                "version": "0.2.0",
                "released": "2026-12-01",
                "summary": "Plain text,\nwith a line end and an umlaut: \u00e4.",
                "notes_url": SITE + "/tag/v0.2.0",
                "files": [file_entry(), file_entry("linux-deb")],
            },
            {
                "channel": "preview",
                "version": "0.3.0-rc.1",
                "released": "2026-12-09",
                "notes_url": SITE + "/tag/v0.3.0-rc.1",
                "files": [file_entry("macos-dmg", "arm64")],
            },
        ],
        "plugins": [
            {
                "id": "com.virtualserialstudio.decoding",
                "version": "1.3.0",
                "released": "2026-12-01",
                "requires_studio": "0.2.0",
                "summary": "Plain text.",
                "notes_url": "https://example.com/notes",
                "files": [plugin_file("windows"), plugin_file("linux")],
            }
        ],
    }


def text_of(document: Any) -> str:
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def problems_of(document: Any) -> List[str]:
    return uf.check_payload(text_of(document))


Mutation = Callable[[Dict[str, Any]], None]


def studio(doc: Dict[str, Any], index: int = 0) -> Dict[str, Any]:
    return doc["studio"][index]  # type: ignore[no-any-return]


def studio_file(doc: Dict[str, Any], index: int = 0) -> Dict[str, Any]:
    return doc["studio"][0]["files"][index]  # type: ignore[no-any-return]


def plugin(doc: Dict[str, Any]) -> Dict[str, Any]:
    return doc["plugins"][0]  # type: ignore[no-any-return]


def setter(get: Callable[[Dict[str, Any]], Dict[str, Any]], key: str, value: Any) -> Mutation:
    def mutate(doc: Dict[str, Any]) -> None:
        get(doc)[key] = value

    return mutate


def remover(get: Callable[[Dict[str, Any]], Dict[str, Any]], key: str) -> Mutation:
    def mutate(doc: Dict[str, Any]) -> None:
        del get(doc)[key]

    return mutate


def top(doc: Dict[str, Any]) -> Dict[str, Any]:
    return doc


def more_files(count: int, kinds: Tuple[str, ...] = uf.KINDS) -> List[Dict[str, Any]]:
    """``count`` studio files; the (kind, arch) pairs repeat after 14."""
    pairs = [(k, a) for k in kinds for a in uf.ARCHES]
    return [file_entry(*pairs[i % len(pairs)]) for i in range(count)]


# What each rule of check_payload refuses: a name, the text the problem must
# hold and the change that spoils the good payload.
BAD_PAYLOADS: List[Tuple[str, str, Mutation]] = [
    # -- the top
    ("unknown top key", "extra", setter(top, "extra", 1)),
    ("missing feed_version", "feed_version", remover(top, "feed_version")),
    ("missing serial", "serial", remover(top, "serial")),
    ("missing issued_at", "issued_at", remover(top, "issued_at")),
    ("missing studio", "studio", remover(top, "studio")),
    ("feed_version 2", "feed_version", setter(top, "feed_version", 2)),
    ("feed_version 0", "feed_version", setter(top, "feed_version", 0)),
    ("feed_version string", "feed_version", setter(top, "feed_version", "1")),
    ("feed_version bool", "feed_version", setter(top, "feed_version", True)),
    ("feed_version fraction", "feed_version", setter(top, "feed_version", 1.0)),
    ("serial 0", "serial", setter(top, "serial", 0)),
    ("serial negative", "serial", setter(top, "serial", -1)),
    ("serial above 2^53", "serial", setter(top, "serial", (1 << 53) + 1)),
    ("serial fraction", "serial", setter(top, "serial", 1.5)),
    ("serial whole but written with a fraction", "serial", setter(top, "serial", 12.0)),
    ("serial bool", "serial", setter(top, "serial", True)),
    ("serial string", "serial", setter(top, "serial", "12")),
    ("issued_at with a space", "issued_at", setter(top, "issued_at", "2026-12-01 03:40:00Z")),
    ("issued_at without Z", "issued_at", setter(top, "issued_at", "2026-12-01T03:40:00")),
    ("issued_at with an offset", "issued_at", setter(top, "issued_at", "2026-12-01T03:40:00+00:00")),
    ("issued_at lower-case z", "issued_at", setter(top, "issued_at", "2026-12-01T03:40:00z")),
    ("issued_at with a fraction", "issued_at", setter(top, "issued_at", "2026-12-01T03:40:00.5Z")),
    ("issued_at month 13", "issued_at", setter(top, "issued_at", "2026-13-01T03:40:00Z")),
    ("issued_at 30 February", "issued_at", setter(top, "issued_at", "2026-02-30T03:40:00Z")),
    ("issued_at hour 24", "issued_at", setter(top, "issued_at", "2026-12-01T24:00:00Z")),
    ("issued_at second 60", "issued_at", setter(top, "issued_at", "2026-12-01T03:40:60Z")),
    ("issued_at a date only", "issued_at", setter(top, "issued_at", "2026-12-01")),
    ("issued_at a number", "issued_at", setter(top, "issued_at", 20261201)),
    # -- studio entries
    ("studio not a list", "studio", setter(top, "studio", {})),
    ("studio three entries", "studio", lambda d: d["studio"].append(copy.deepcopy(studio(d)))),
    ("studio two stable entries", "two entries for the channel",
     lambda d: d["studio"].__setitem__(1, copy.deepcopy(studio(d)))),
    ("studio entry not an object", "studio[0]", lambda d: d["studio"].__setitem__(0, [])),
    ("studio entry unknown key", "extra", setter(studio, "extra", 1)),
    ("studio missing channel", "channel", remover(studio, "channel")),
    ("studio missing version", "version", remover(studio, "version")),
    ("studio missing released", "released", remover(studio, "released")),
    ("studio missing notes_url", "notes_url", remover(studio, "notes_url")),
    ("studio missing files", "files", remover(studio, "files")),
    ("studio channel unknown", "channel", setter(studio, "channel", "beta")),
    ("studio channel not a string", "channel", setter(studio, "channel", 1)),
    ("stable version is a pre-release", "stable version", setter(studio, "version", "0.2.0-rc.1")),
    ("preview version is final",
     "preview version", lambda d: studio(d, 1).__setitem__("version", "0.3.0")),
    ("studio released not a date", "released", setter(studio, "released", "2026-02-30")),
    ("studio released with a time", "released", setter(studio, "released", "2026-12-01T00:00:00Z")),
    ("studio released a number", "released", setter(studio, "released", 20261201)),
    ("studio released single digits", "released", setter(studio, "released", "2026-1-1")),
    ("studio summary not a string", "summary", setter(studio, "summary", 5)),
    ("studio summary with a bell", "control", setter(studio, "summary", "a\x07b")),
    ("studio summary with a carriage return", "control", setter(studio, "summary", "a\r\nb")),
    ("studio summary with a tab", "control", setter(studio, "summary", "a\tb")),
    ("studio summary with a delete", "control", setter(studio, "summary", "a\x7fb")),
    ("studio summary with a C1 control", "control", setter(studio, "summary", "a\u0085b")),
    ("studio summary over 2000 bytes", "2000", setter(studio, "summary", "a" * 2001)),
    ("studio summary over 2000 bytes in UTF-8", "2000", setter(studio, "summary", "\u00e4" * 1001)),
    ("studio summary lone surrogate", "UTF-8", setter(studio, "summary", "a\ud800b")),
    # -- files
    ("files not a list", "files", setter(studio, "files", {})),
    ("files empty", "files", setter(studio, "files", [])),
    ("files 17", "files", setter(studio, "files", more_files(17))),
    ("files same kind and arch", "two files",
     lambda d: studio(d)["files"].append(file_entry())),
    ("studio file not an object", "files[0]", lambda d: studio(d)["files"].__setitem__(0, "x")),
    ("file unknown key", "extra", setter(studio_file, "extra", 1)),
    ("file missing kind", "kind", remover(studio_file, "kind")),
    ("file missing arch", "arch", remover(studio_file, "arch")),
    ("file missing name", "name", remover(studio_file, "name")),
    ("file missing url", "url", remover(studio_file, "url")),
    ("file missing size", "size", remover(studio_file, "size")),
    ("file missing sha512", "sha512", remover(studio_file, "sha512")),
    ("file has os instead of kind", "os", lambda d: studio_file(d).__setitem__("os", "windows")),
    ("file kind unknown", "kind", setter(studio_file, "kind", "windows-msi")),
    ("file kind not a string", "kind", setter(studio_file, "kind", 7)),
    ("file arch unknown", "arch", setter(studio_file, "arch", "x86")),
    ("file arch aarch64", "arch", setter(studio_file, "arch", "aarch64")),
    ("file name empty", "name", setter(studio_file, "name", "")),
    ("file name 256 characters", "name", setter(studio_file, "name", "a" * 256)),
    ("file name with a leading dot", "name", setter(studio_file, "name", ".hidden")),
    ("file name with a dot at the end", "name", setter(studio_file, "name", "a.exe.")),
    ("file name that is a device", "name", setter(studio_file, "name", "CON")),
    ("file name that is a device with an ending", "name", setter(studio_file, "name", "Nul.exe")),
    ("file name that is a device with two endings", "name", setter(studio_file, "name", "aux.tar.gz")),
    ("file name that is a serial device", "name", setter(studio_file, "name", "com9.exe")),
    ("file name that is a printer device", "name", setter(studio_file, "name", "LPT0.deb")),
    ("file name with a space", "name", setter(studio_file, "name", "a b.exe")),
    ("file name with a slash", "name", setter(studio_file, "name", "a/b.exe")),
    ("file name with a backslash", "name", setter(studio_file, "name", "a\\b.exe")),
    ("file name with a non-ASCII letter", "name", setter(studio_file, "name", "caf\u00e9.exe")),
    ("file name a number", "name", setter(studio_file, "name", 5)),
    ("file size 0", "size", setter(studio_file, "size", 0)),
    ("file size over 4 GiB", "size", setter(studio_file, "size", 4294967297)),
    ("file size negative", "size", setter(studio_file, "size", -5)),
    ("file size fraction", "size", setter(studio_file, "size", 1.5)),
    ("file size bool", "size", setter(studio_file, "size", True)),
    ("file size string", "size", setter(studio_file, "size", "5")),
    ("sha512 short", "sha512", setter(studio_file, "sha512", "ab" * 63)),
    ("sha512 long", "sha512", setter(studio_file, "sha512", "ab" * 65)),
    ("sha512 upper case", "sha512", setter(studio_file, "sha512", "AB" * 64)),
    ("sha512 not hex", "sha512", setter(studio_file, "sha512", "xy" * 64)),
    ("sha512 a number", "sha512", setter(studio_file, "sha512", 5)),
    # -- addresses (the url of a file, the notes_url of a release)
    *[
        ("%s %s" % (field, label), field, setter(getter, field, value))
        for field, getter in (("url", studio_file), ("notes_url", studio))
        for label, value in (
            ("empty", ""),
            ("http", "http://example.com/a"),
            ("upper-case scheme", "HTTPS://example.com/a"),
            ("no scheme", "example.com/a"),
            ("ftp", "ftp://example.com/a"),
            ("no host", "https://"),
            ("no host before a path", "https:///a"),
            ("a user name", "https://user@example.com/a"),
            ("a user name and password", "https://user:pw@example.com/a"),
            ("an @ before the first slash with no path", "https://example.com@evil.example"),
            ("a backslash", "https://example.com\\a"),
            ("a backslash in the path", "https://example.com/a\\b"),
            ("a space", "https://example.com/a b"),
            ("a line feed", "https://example.com/a\n"),
            ("a control character", "https://example.com/\x01"),
            ("a non-ASCII character", "https://example.com/\u00e4"),
            ("2049 characters", "https://example.com/" + "a" * (2049 - 20)),
            ("a number", 5),
        )
    ],
    # -- versions (in studio, in plugins and in requires_studio)
    *[
        ("%s %s" % (label, shown), field, setter(getter, field, value))
        for label, field, getter in (
            ("studio", "version", studio),
            ("plugin", "version", plugin),
            ("plugin requires_studio", "requires_studio", plugin),
        )
        for shown, value in (
            ("empty", ""),
            ("with a leading v", "v1.0.0"),
            ("with two parts", "1.0"),
            ("with four parts", "1.0.0.0"),
            ("with build metadata", "1.0.0+build.5"),
            ("with a leading zero", "01.0.0"),
            ("with a leading zero in the minor", "1.00.0"),
            ("number above 999999", "1000000.0.0"),
            ("an empty pre-release", "1.0.0-"),
            ("a leading zero in a numeric identifier", "1.0.0-01"),
            ("an empty identifier", "1.0.0-a..b"),
            ("a trailing dot", "1.0.0-a."),
            ("a non-ASCII identifier", "1.0.0-\u00e9"),
            ("an underscore", "1.0.0-a_b"),
            ("dev", "1.0.0-dev"),
            ("65 characters", "1.0.0-" + "a" * 59),
            ("17 identifiers", "1.0.0-" + ".".join(["a"] * 17)),
            ("a letter", "1.0.x"),
            ("a number", 1),
        )
    ],
    # -- plugins
    ("plugins not a list", "plugins", setter(top, "plugins", {})),
    ("65 plugins", "plugins",
     lambda d: d.__setitem__("plugins", [
         dict(copy.deepcopy(plugin(d)), id="com.virtualserialstudio.p%d" % i)
         for i in range(65)
     ])),
    ("plugin not an object", "plugins[0]", lambda d: d["plugins"].__setitem__(0, 5)),
    ("plugin unknown key", "extra", setter(plugin, "extra", 1)),
    ("plugin missing id", "id", remover(plugin, "id")),
    ("plugin missing version", "version", remover(plugin, "version")),
    ("plugin missing released", "released", remover(plugin, "released")),
    ("plugin missing requires_studio", "requires_studio", remover(plugin, "requires_studio")),
    ("plugin missing notes_url", "notes_url", remover(plugin, "notes_url")),
    ("plugin missing files", "files", remover(plugin, "files")),
    ("plugin id with another prefix", "id", setter(plugin, "id", "com.example.decoding")),
    ("plugin id with a shorter prefix", "id", setter(plugin, "id", "com.virtualserial.decoding")),
    ("plugin id with an upper-case letter", "id", setter(plugin, "id", "com.virtualserialstudio.Decoding")),
    ("plugin id with an underscore", "id", setter(plugin, "id", "com.virtualserialstudio.de_coding")),
    ("plugin id that is the prefix alone", "id", setter(plugin, "id", "com.virtualserialstudio.")),
    ("plugin released in the year 0", "released", setter(plugin, "released", "0000-12-01")),
    ("plugin notes_url with a port and no host", "notes_url", setter(plugin, "notes_url", "https://:443/a")),
    ("plugin notes_url with a query and no host", "notes_url", setter(plugin, "notes_url", "https://?x")),
    ("plugin notes_url with a fragment and no host", "notes_url", setter(plugin, "notes_url", "https://#x")),
    ("plugin id over 128 characters", "id", setter(plugin, "id", "com.virtualserialstudio." + "a" * 105)),
    ("plugin id a number", "id", setter(plugin, "id", 5)),
    ("plugin ids not unique", "twice",
     lambda d: d["plugins"].append(copy.deepcopy(plugin(d)))),
    ("plugin released not a date", "released", setter(plugin, "released", "2026-00-10")),
    ("plugin summary with a control character", "control", setter(plugin, "summary", "a\x00b")),
    ("plugin summary too long", "2000", setter(plugin, "summary", "a" * 2001)),
    ("plugin notes_url not https", "notes_url", setter(plugin, "notes_url", "http://example.com/")),
    ("plugin files not a list", "files", setter(plugin, "files", None)),
    ("plugin files 17", "files",
     lambda d: plugin(d).__setitem__(
         "files",
         [plugin_file(o, a, name="f%d.zip" % i) for i, (o, a) in enumerate(
             [(o, a) for o in uf.OSES for a in uf.ARCHES] * 3)][:17],
     )),
    ("plugin files same os and arch", "two files",
     lambda d: plugin(d)["files"].append(plugin_file("windows"))),
    ("plugin file os unknown", "os", lambda d: plugin(d)["files"][0].__setitem__("os", "freebsd")),
    ("plugin file arch unknown", "arch", lambda d: plugin(d)["files"][0].__setitem__("arch", "riscv")),
    ("plugin file with kind", "kind", lambda d: plugin(d)["files"][0].__setitem__("kind", "linux-deb")),
    ("plugin file missing sha512", "sha512", lambda d: plugin(d)["files"][0].pop("sha512")),
    ("plugin file name with a leading dot", "name",
     lambda d: plugin(d)["files"][0].__setitem__("name", ".x")),
    ("plugin file size 0", "size", lambda d: plugin(d)["files"][0].__setitem__("size", 0)),
    ("plugin file url with a user name", "url",
     lambda d: plugin(d)["files"][0].__setitem__("url", "https://a@example.com/x")),
]


class CheckPayloadTests(unittest.TestCase):
    def test_a_good_payload_has_no_problem(self) -> None:
        self.assertEqual(problems_of(good_payload()), [])

    def test_the_one_in_the_design_has_no_problem(self) -> None:
        document = {
            "feed_version": 1,
            "serial": 12,
            "issued_at": "2026-12-01T03:40:00Z",
            "studio": [
                {
                    "channel": "stable",
                    "version": "0.2.0",
                    "released": "2026-12-01",
                    "summary": "Plain text, at most 2000 characters.",
                    "notes_url": SITE + "/tag/v0.2.0",
                    "files": [file_entry()],
                }
            ],
            "plugins": [],
        }
        self.assertEqual(problems_of(document), [])

    def test_plugins_may_be_absent(self) -> None:
        document = good_payload()
        del document["plugins"]
        self.assertEqual(problems_of(document), [])

    def test_studio_may_be_empty_but_not_absent(self) -> None:
        document = good_payload()
        document["studio"] = []
        self.assertEqual(problems_of(document), [])

    def test_summaries_may_be_absent(self) -> None:
        document = good_payload()
        del studio(document)["summary"]
        del plugin(document)["summary"]
        self.assertEqual(problems_of(document), [])

    def test_a_plugin_may_have_no_files(self) -> None:
        document = good_payload()
        plugin(document)["files"] = []
        self.assertEqual(problems_of(document), [])

    def test_the_largest_values_that_are_allowed(self) -> None:
        document = good_payload()
        document["serial"] = 1 << 53
        document["feed_version"] = 1
        entry = studio(document)
        entry["summary"] = "a" * 2000
        entry["version"] = "999999.999999.999999"
        entry["files"] = more_files(14)
        entry["files"][0].update(
            size=4294967296, name="a" * 255, url="https://h/" + "a" * (2048 - 10)
        )
        studio(document, 1)["version"] = "0.0.1-" + ".".join(["a"] * 16)
        self.assertEqual(problems_of(document), [])

    def test_the_smallest_values_that_are_allowed(self) -> None:
        document = good_payload()
        document["serial"] = 1
        studio_file(document).update(size=1, name="a", url="https://h")
        studio(document)["summary"] = ""
        studio(document, 1)["version"] = "0.0.0-0"
        self.assertEqual(problems_of(document), [])

    def test_a_summary_of_exactly_2000_bytes_in_utf8(self) -> None:
        document = good_payload()
        studio(document)["summary"] = "\u00e4" * 1000
        self.assertEqual(problems_of(document), [])
        studio(document)["summary"] = "\U0001f600\n" * 300 + "x" * 500
        self.assertEqual(len(studio(document)["summary"].encode("utf-8")), 2000)
        self.assertEqual(problems_of(document), [])

    def test_versions_that_are_allowed(self) -> None:
        for good in (
            "0.0.0", "1.2.3", "0.1.0-rc.5", "1.0.0-alpha", "1.0.0-0", "1.0.0-rc.0",
            "1.0.0-a-b", "1.0.0-x.7.z.92", "1.0.0-dev.1", "1.0.0-device",
            "1.0.0-0abc", "1.0.0--", "999999.999999.999999", "1.0.0-" + "a" * 58,
        ):
            self.assertIsNone(uf.version_problem(good), good)
        for bad in ("1.0.0-dev", "1.0.0+1", "v1.0.0", "1.0.0-01", ""):
            self.assertIsNotNone(uf.version_problem(bad), bad)

    def test_every_bad_payload_is_refused(self) -> None:
        self.assertGreater(len(BAD_PAYLOADS), 150)
        names = [name for name, _, _ in BAD_PAYLOADS]
        self.assertEqual(len(names), len(set(names)), "a name is used twice")
        for name, expected, mutate in BAD_PAYLOADS:
            with self.subTest(name):
                document = good_payload()
                mutate(document)
                problems = problems_of(document)
                self.assertTrue(problems, "the payload was accepted")
                self.assertTrue(
                    any(expected in line for line in problems),
                    "no problem mentions %r: %r" % (expected, problems),
                )

    def test_a_number_with_an_exponent_is_refused(self) -> None:
        text = text_of(good_payload())
        for number in ("1.2e1", "12.0", "1E1", "12e0"):
            with self.subTest(number):
                changed = text.replace('"serial": 12,', '"serial": %s,' % number)
                self.assertNotEqual(changed, text)
                self.assertTrue(uf.check_payload(changed))
        sized = text.replace('"size": 50132336', '"size": 5e7')
        self.assertTrue(uf.check_payload(sized))

    def test_a_key_given_twice_is_refused(self) -> None:
        text = text_of(good_payload()).replace(
            '"serial": 12,', '"serial": 12,\n  "serial": 13,'
        )
        problems = uf.check_payload(text)
        self.assertTrue(any("twice" in line for line in problems), problems)
        inner = text_of(good_payload()).replace(
            '"arch": "x86_64",', '"arch": "x86_64",\n "arch": "x86_64",', 1
        )
        self.assertTrue(uf.check_payload(inner))

    def test_what_is_not_an_object_or_not_json(self) -> None:
        for text in ("", "[]", "5", '"x"', "null", "{", "{} {}", "NaN",
                     '{"serial": NaN}', "\ufeff{}", "{'a': 1}"):
            with self.subTest(text):
                self.assertTrue(uf.check_payload(text))

    def test_bytes_that_are_not_utf8(self) -> None:
        good = text_of(good_payload()).encode("utf-8")
        self.assertEqual(uf.check_payload(good), [])
        problems = uf.check_payload(good.replace(b"Plain text.", b"Plain \xff."))
        self.assertTrue(any("UTF-8" in line for line in problems), problems)

    def test_a_deeply_nested_document_is_refused_not_a_crash(self) -> None:
        self.assertTrue(uf.check_payload("[" * 100000))

    def test_a_huge_whole_number_is_refused_not_a_crash(self) -> None:
        text = text_of(good_payload()).replace('"serial": 12', '"serial": ' + "9" * 5000)
        self.assertTrue(uf.check_payload(text))

    def test_the_problems_name_the_place(self) -> None:
        document = good_payload()
        studio_file(document, 1)["sha512"] = "x"
        self.assertEqual(
            [line.split(":")[0] for line in problems_of(document)],
            ["studio[0].files[1].sha512"],
        )


# -- Versions, dates and addresses -------------------------------------------


class FileNameTests(unittest.TestCase):
    def test_a_device_name_is_the_whole_part_before_the_first_dot(self) -> None:
        for name in ("console.exe", "com.exe", "com10.exe", "lpt.zip", "nulls",
                     "a.con", "studio-con.exe", "a.b"):
            self.assertTrue(uf.is_file_name(name), name)
        for name in ("con", "CON", "Nul.exe", "aux.tar.gz", "prn.zip", "COM1",
                     "com9.exe", "LPT0.deb", "a.", ".a", "", "a b", 5):
            self.assertFalse(uf.is_file_name(name), name)


class VersionTests(unittest.TestCase):
    def assertOrdered(self, versions: List[str]) -> None:
        for lower, higher in zip(versions, versions[1:]):
            self.assertEqual(uf.compare_versions(lower, higher), -1, (lower, higher))
            self.assertEqual(uf.compare_versions(higher, lower), 1, (higher, lower))
            self.assertEqual(uf.compare_versions(lower, lower), 0, lower)

    def test_the_example_of_semantic_versioning(self) -> None:
        self.assertOrdered([
            "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
        ])

    def test_every_pair_of_that_example_is_in_order(self) -> None:
        versions = [
            "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
        ]
        for i, lower in enumerate(versions):
            for higher in versions[i + 1:]:
                self.assertEqual(uf.compare_versions(lower, higher), -1)
                self.assertEqual(uf.compare_versions(higher, lower), 1)

    def test_the_release_candidates_of_this_project(self) -> None:
        self.assertOrdered(["0.1.0-rc.4", "0.1.0-rc.5", "0.1.0"])

    def test_the_numbers_count_as_numbers(self) -> None:
        self.assertOrdered(["0.9.0", "0.10.0", "1.0.0", "1.0.1", "1.1.0", "2.0.0"])
        self.assertOrdered(["1.0.0-9", "1.0.0-10", "1.0.0-a"])

    def test_a_longer_set_is_greater_when_the_start_is_equal(self) -> None:
        self.assertOrdered(["1.0.0-a", "1.0.0-a.0", "1.0.0-a.0.0"])

    def test_letters_compare_as_ascii(self) -> None:
        self.assertOrdered(["1.0.0-Z", "1.0.0-a", "1.0.0-b"])
        self.assertOrdered(["1.0.0-a-b", "1.0.0-a0"])

    def test_the_pre_release_of_a_lower_version_is_below_the_next_final(self) -> None:
        self.assertOrdered(["0.2.0-rc.1", "0.2.0", "0.3.0-rc.1", "0.3.0"])

    def test_a_version_that_is_not_one_cannot_be_compared(self) -> None:
        for bad in ("v1.0.0", "1.0", "1.0.0+a", "1.0.0-dev"):
            with self.assertRaises(ValueError, msg=bad):
                uf.compare_versions(bad, "1.0.0")
            with self.assertRaises(ValueError, msg=bad):
                uf.compare_versions("1.0.0", bad)

    def test_is_prerelease(self) -> None:
        self.assertTrue(uf.is_prerelease("0.1.0-rc.5"))
        self.assertTrue(uf.is_prerelease("1.0.0-a-b"))
        self.assertFalse(uf.is_prerelease("0.1.0"))


class DateTests(unittest.TestCase):
    def test_dates(self) -> None:
        for good in ("2026-12-01", "2024-02-29", "2000-01-01"):
            self.assertTrue(uf.is_date(good), good)
        for bad in ("2026-02-29", "2026-00-01", "2026-12-32", "2026-1-1",
                    "26-12-01", "2026-12-01 ", "2026-12-01\n", "\uff12026-12-01",
                    "2026/12/01", "", None, 20261201):
            self.assertFalse(uf.is_date(bad), repr(bad))

    def test_timestamps(self) -> None:
        for good in ("2026-12-01T03:40:00Z", "2024-02-29T23:59:59Z"):
            self.assertTrue(uf.is_timestamp(good), good)
        for bad in ("2026-12-01T03:40:00", "2026-12-01t03:40:00Z",
                    "2026-12-01T03:40Z", "2026-12-01T24:00:00Z",
                    "2026-12-01T03:60:00Z", "2026-12-01T03:40:00Z\n", None):
            self.assertFalse(uf.is_timestamp(bad), repr(bad))


class UrlTests(unittest.TestCase):
    def test_good_addresses(self) -> None:
        for good in ("https://example.com", "https://example.com/a?b=c#d",
                     "https://example.com:8443/a", "https://h/a@b",
                     "https://example.com/x@y",
                     "https://[::1]/a"):
            self.assertIsNone(uf.url_problem(good), good)

    def test_the_limit_is_2048_characters(self) -> None:
        self.assertIsNone(uf.url_problem("https://h/" + "a" * (2048 - 10)))
        self.assertIsNotNone(uf.url_problem("https://h/" + "a" * (2049 - 10)))


# -- Release folders ---------------------------------------------------------

REPO = "https://github.com/iainchesworthlabs/virtual-serial-studio"


def release_names(tag: str, windows_suffix: str = "") -> List[str]:
    """The files of a release, as ``docs/releasing.md`` lists them, with the
    signatures, the checksums and the SBOM."""
    version = tag[1:]
    rpm_version = version.replace("-", "-0.", 1) if "-" in version else version + "-1"
    deb_version = version.replace("-", ".", 1) + "-1"
    packages = [
        "virtual-serial-studio-%s.x86_64.rpm" % rpm_version,
        "virtual-serial-studio-%s-linux-x86_64.tar.gz" % tag,
        "virtual-serial-studio-%s-macos-arm64.dmg" % tag,
        "virtual-serial-studio-%s-macos-arm64.tar.gz" % tag,
        "virtual-serial-studio-%s-windows-x86_64%s.exe" % (tag, windows_suffix),
        "virtual-serial-studio-%s-windows-x86_64%s.zip" % (tag, windows_suffix),
        "virtual-serial-studio_%s_amd64.deb" % deb_version,
    ]
    names = ["SHA512SUMS", "SHA512SUMS.asc", "sbom.spdx.json", "sbom.spdx.json.asc"]
    for package in packages:
        names += [package, package + ".asc"]
    return names + ["update-feed.json"]


def digest_of(name: str) -> str:
    return hashlib.sha512(name.encode("utf-8")).hexdigest()


class ToolTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="update-feed-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def run_tool(self, *arguments: Any) -> "subprocess.CompletedProcess[str]":
        done = subprocess.run(
            [sys.executable, str(TOOL)] + [str(a) for a in arguments],
            capture_output=True,
            text=True,
            check=False,
        )
        # No command ever shows a key.
        for output in (done.stdout, done.stderr):
            self.assertNotIn("PRIVATE KEY", output)
            if KEY is not None:
                for line in KEY.read_text(encoding="ascii").splitlines()[1:-1]:
                    self.assertNotIn(line, output)
        return done

    def write(self, name: str, data: bytes) -> Path:
        path = self.root / name
        path.write_bytes(data)
        return path

    def make_release(
        self,
        folder: str,
        tag: str,
        published_at: str = "2026-12-01T03:35:45Z",
        prerelease: Optional[bool] = None,
        windows_suffix: str = "",
        summary: Optional[bytes] = None,
        leave_out: Tuple[str, ...] = (),
        add: Tuple[str, ...] = (),
        binary_marker: Tuple[str, ...] = (),
        reverse: bool = False,
        sums: Optional[bytes] = None,
    ) -> Path:
        path = self.root / folder
        path.mkdir()
        names = release_names(tag, windows_suffix) + list(add)
        if reverse:
            names.reverse()
        assets = [
            {
                "name": name,
                "size": 1000 + len(name),
                "url": "%s/releases/download/%s/%s" % (REPO, tag, name),
            }
            for name in names
        ]
        document = {
            "tag": tag,
            "prerelease": ("-" in tag) if prerelease is None else prerelease,
            "published_at": published_at,
            "html_url": "%s/releases/tag/%s" % (REPO, tag),
            "assets": assets,
        }
        (path / "release.json").write_bytes(json.dumps(document, indent=1).encode())
        if sums is None:
            lines = []
            for name in sorted(names):
                if name.endswith(".asc") or name in ("SHA512SUMS", "update-feed.json"):
                    continue
                if name in leave_out:
                    continue
                marker = "*" if name in binary_marker else " "
                lines.append("%s %s%s\n" % (digest_of(name), marker, name))
            sums = "".join(lines).encode("utf-8")
        (path / "SHA512SUMS").write_bytes(sums)
        if summary is not None:
            (path / "summary.txt").write_bytes(summary)
        return path

    def payload(self, *arguments: Any, out: str = "feed.payload.json"):
        return self.run_tool(
            "payload", "--serial", 12, "--issued-at", "2026-12-01T03:40:00Z",
            *arguments, "--out", self.root / out,
        )

    def read_payload(self, out: str = "feed.payload.json") -> Dict[str, Any]:
        return json.loads((self.root / out).read_bytes())  # type: ignore[no-any-return]


EXPECTED_KINDS = [
    ("windows-installer", "x86_64"),
    ("windows-zip", "x86_64"),
    ("macos-dmg", "arm64"),
    ("macos-archive", "arm64"),
    ("linux-deb", "x86_64"),
    ("linux-rpm", "x86_64"),
    ("linux-archive", "x86_64"),
]


class MatchTests(unittest.TestCase):
    def test_the_names_of_a_release(self) -> None:
        names = release_names("v0.1.0-rc.5", "-testsigned")
        found = [uf.match_asset(name) for name in names]
        kinds = sorted(
            ((m[0], m[1], m[2]) for m in found if m is not None)
        )
        self.assertEqual([(k, a) for _, k, a in kinds], EXPECTED_KINDS)

    def test_names_that_are_ignored(self) -> None:
        for name in ("SHA512SUMS", "SHA512SUMS.asc", "sbom.spdx.json",
                     "sbom.spdx.json.asc", "update-feed.json",
                     "virtual-serial-studio-v1.0.0-windows-x86_64.exe.asc",
                     "virtual-serial-studio-v1.0.0-macos-arm64.dmg.asc",
                     "virtual-serial-studio_1.0.0-1_amd64.deb.asc",
                     "virtual-serial-studio-v1.0.0-windows-x86_64-1.exe",
                     "virtual-serial-studio-v1.0.0-windows-arm64.exe",
                     "virtual-serial-studio-v1.0.0-linux-arm64.tar.gz",
                     "virtual-serial-studio-v1.0.0-windows-x86_64.exe\n"):
            self.assertIsNone(uf.match_asset(name), name)

    def test_the_suffix_of_a_windows_package(self) -> None:
        for suffix in ("", "-unsigned", "-testsigned", "-x"):
            name = "virtual-serial-studio-v1.0.0-windows-x86_64%s.exe" % suffix
            found = uf.match_asset(name)
            self.assertIsNotNone(found, name)
            assert found is not None
            self.assertEqual(found[1:], ("windows-installer", "x86_64"))

    def test_the_two_archives_of_macos_and_linux_are_not_mixed_up(self) -> None:
        mac = uf.match_asset("virtual-serial-studio-v1.0.0-macos-arm64.tar.gz")
        linux = uf.match_asset("virtual-serial-studio-v1.0.0-linux-x86_64.tar.gz")
        assert mac is not None and linux is not None
        self.assertEqual(mac[1], "macos-archive")
        self.assertEqual(linux[1], "linux-archive")


class SumsTests(unittest.TestCase):
    def test_both_markers_and_both_line_ends(self) -> None:
        one, two = "ab" * 64, "cd" * 64
        data = ("%s  a.exe\r\n\r\n%s *b file.zip\n" % (one, two)).encode()
        self.assertEqual(
            uf.parse_sums(data, "x"), {"a.exe": one, "b file.zip": two}
        )

    def test_upper_case_hex_is_written_in_lower_case(self) -> None:
        self.assertEqual(
            uf.parse_sums(("AB" * 64 + "  a\n").encode(), "x"), {"a": "ab" * 64}
        )

    def test_bad_lines(self) -> None:
        for text in ("abc  a\n", "ab" * 64 + " a\n", "ab" * 64 + "a\n",
                     "ab" * 64 + "  \n", "xy" * 64 + "  a\n", "\\" + "ab" * 64 + "  a\n"):
            with self.assertRaises(ps.Refusal, msg=text):
                uf.parse_sums(text.encode(), "x")
        with self.assertRaises(ps.Refusal):
            uf.parse_sums(b"\xff\xfe", "x")

    def test_one_name_with_two_hashes(self) -> None:
        text = "%s  a\n%s  a\n" % ("ab" * 64, "cd" * 64)
        with self.assertRaises(ps.Refusal):
            uf.parse_sums(text.encode(), "x")
        same = "%s  a\n%s  a\n" % ("ab" * 64, "ab" * 64)
        self.assertEqual(uf.parse_sums(same.encode(), "x"), {"a": "ab" * 64})


class PayloadCommandTests(ToolTestCase):
    def stable(self, **changes: Any) -> Path:
        return self.make_release("stable", "v0.1.0", **changes)

    def preview(self, tag: str = "v0.2.0-rc.1", **changes: Any) -> Path:
        return self.make_release("preview", tag, "2026-12-09T10:00:00Z", **changes)

    def test_a_stable_release(self) -> None:
        done = self.payload("--stable", self.stable())
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stderr, "")
        document = self.read_payload()
        self.assertEqual(document["feed_version"], 1)
        self.assertEqual(document["serial"], 12)
        self.assertEqual(document["issued_at"], "2026-12-01T03:40:00Z")
        self.assertEqual(document["plugins"], [])
        (entry,) = document["studio"]
        self.assertEqual(entry["channel"], "stable")
        self.assertEqual(entry["version"], "0.1.0")
        self.assertEqual(entry["released"], "2026-12-01")
        self.assertEqual(entry["notes_url"], REPO + "/releases/tag/v0.1.0")
        self.assertNotIn("summary", entry)
        self.assertEqual(
            [(f["kind"], f["arch"]) for f in entry["files"]], EXPECTED_KINDS
        )
        self.assertEqual(uf.check_payload((self.root / "feed.payload.json").read_bytes()), [])

    def test_the_files_come_from_the_assets_and_the_checksums(self) -> None:
        self.payload("--stable", self.stable())
        files = self.read_payload()["studio"][0]["files"]
        by_kind = {f["kind"]: f for f in files}
        installer = by_kind["windows-installer"]
        name = "virtual-serial-studio-v0.1.0-windows-x86_64.exe"
        self.assertEqual(installer["name"], name)
        self.assertEqual(installer["url"], "%s/releases/download/v0.1.0/%s" % (REPO, name))
        self.assertEqual(installer["size"], 1000 + len(name))
        self.assertEqual(installer["sha512"], digest_of(name))
        self.assertEqual(
            by_kind["linux-deb"]["name"], "virtual-serial-studio_0.1.0-1_amd64.deb"
        )
        self.assertEqual(
            by_kind["linux-rpm"]["name"], "virtual-serial-studio-0.1.0-1.x86_64.rpm"
        )

    def test_the_signatures_and_the_sbom_and_the_feed_are_ignored(self) -> None:
        self.payload("--stable", self.stable())
        names = [f["name"] for f in self.read_payload()["studio"][0]["files"]]
        self.assertEqual(len(names), 7)
        for name in names:
            for ignored in (".asc", "sbom", "SHA512SUMS", "update-feed"):
                self.assertNotIn(ignored, name)

    def test_a_preview_release_like_v0_1_0_rc_5(self) -> None:
        folder = self.make_release(
            "rc5", "v0.1.0-rc.5", "2026-11-15T03:35:45Z", windows_suffix="-testsigned"
        )
        done = self.payload("--preview", folder)
        self.assertEqual(done.returncode, 0, done.stderr)
        (entry,) = self.read_payload()["studio"]
        self.assertEqual(entry["channel"], "preview")
        self.assertEqual(entry["version"], "0.1.0-rc.5")
        self.assertEqual(entry["released"], "2026-11-15")
        installer = entry["files"][0]
        self.assertEqual(installer["kind"], "windows-installer")
        self.assertTrue(installer["name"].endswith("-windows-x86_64-testsigned.exe"))
        self.assertEqual(
            entry["files"][1]["name"],
            "virtual-serial-studio-v0.1.0-rc.5-windows-x86_64-testsigned.zip",
        )

    def test_stable_and_preview(self) -> None:
        done = self.payload("--stable", self.stable(), "--preview", self.preview())
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stderr, "")
        stable, preview = self.read_payload()["studio"]
        self.assertEqual((stable["channel"], stable["version"]), ("stable", "0.1.0"))
        self.assertEqual((preview["channel"], preview["version"]), ("preview", "0.2.0-rc.1"))
        self.assertEqual(preview["released"], "2026-12-09")

    def test_a_preview_that_is_not_newer_is_left_out(self) -> None:
        for tag in ("v0.1.0-rc.5", "v0.0.9-rc.1"):
            with self.subTest(tag):
                preview = self.make_release("p-" + tag, tag, "2026-11-15T03:35:45Z")
                done = self.payload("--stable", self.stable_once(), "--preview", preview)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn("preview", done.stderr)
                self.assertIn("not newer", done.stderr)
                self.assertIn(tag[1:], done.stderr)
                studio_entries = self.read_payload()["studio"]
                self.assertEqual([e["channel"] for e in studio_entries], ["stable"])

    def stable_once(self) -> Path:
        path = self.root / "stable"
        return path if path.exists() else self.stable()

    def test_a_preview_of_the_same_version_as_the_stable_is_not_newer(self) -> None:
        # The same numbers, so a pre-release is lower than the final release.
        preview = self.make_release("p", "v0.1.0-rc.9")
        done = self.payload("--stable", self.stable(), "--preview", preview)
        self.assertIn("not newer", done.stderr)

    def test_the_bytes_are_the_same_each_time(self) -> None:
        arguments = ("--stable", self.stable(), "--preview", self.preview())
        self.assertEqual(self.payload(*arguments, out="one.json").returncode, 0)
        self.assertEqual(self.payload(*arguments, out="two.json").returncode, 0)
        first = (self.root / "one.json").read_bytes()
        self.assertEqual(first, (self.root / "two.json").read_bytes())

    def test_the_order_of_the_assets_does_not_matter(self) -> None:
        one = self.make_release("one", "v0.1.0")
        two = self.make_release("two", "v0.1.0", reverse=True)
        self.payload("--stable", one, out="one.json")
        self.payload("--stable", two, out="two.json")
        self.assertEqual(
            (self.root / "one.json").read_bytes(), (self.root / "two.json").read_bytes()
        )

    def test_the_layout_of_the_text(self) -> None:
        self.payload("--stable", self.stable(), "--preview", self.preview())
        data = (self.root / "feed.payload.json").read_bytes()
        self.assertTrue(data.endswith(b"}\n"))
        self.assertFalse(data.endswith(b"\n\n"))
        self.assertNotIn(b"\r", data)
        self.assertFalse(data.startswith(b"\xef\xbb\xbf"))
        text = data.decode("utf-8")
        self.assertTrue(
            text.startswith(
                '{\n  "feed_version": 1,\n  "serial": 12,\n'
                '  "issued_at": "2026-12-01T03:40:00Z",\n  "studio": [\n'
                '    {\n      "channel": "stable",\n      "version": "0.1.0",\n'
                '      "released": "2026-12-01",\n      "notes_url": '
            ),
            text[:300],
        )
        self.assertTrue(text.endswith('  ],\n  "plugins": []\n}\n'), text[-60:])
        for line in text.splitlines():
            self.assertEqual(line, line.rstrip(), "a space at the end of a line")

    def test_the_keys_are_in_the_order_of_the_design(self) -> None:
        self.payload("--stable", self.stable(), "--preview", self.preview())

        def keys(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
            seen.append([k for k, _ in pairs])
            return dict(pairs)

        seen: List[List[str]] = []
        json.loads((self.root / "feed.payload.json").read_bytes(), object_pairs_hook=keys)
        self.assertEqual(
            sorted({tuple(k) for k in seen}),
            sorted({
                ("feed_version", "serial", "issued_at", "studio", "plugins"),
                ("channel", "version", "released", "notes_url", "files"),
                ("kind", "arch", "name", "url", "size", "sha512"),
            }),
        )

    def test_a_summary_is_taken_as_utf8_text(self) -> None:
        text = "Zus\u00e4tzliche \u00c4nderungen \U0001f600\nZeile zwei"
        folder = self.stable(summary=(text + "\r\n").encode("utf-8").replace(b"\n", b"\r\n", 1))
        self.payload("--stable", folder)
        data = (self.root / "feed.payload.json").read_bytes()
        self.assertEqual(self.read_payload()["studio"][0]["summary"], text)
        # The characters are written as they are, not as escapes.
        self.assertIn("Zus\u00e4tzliche".encode("utf-8"), data)
        self.assertIn("\U0001f600".encode("utf-8"), data)
        self.assertNotIn(b"\\u", data)
        self.assertIn(b'"summary": "Zus', data)
        self.assertEqual(
            [k for k in self.read_payload()["studio"][0]],
            ["channel", "version", "released", "summary", "notes_url", "files"],
        )

    def test_an_empty_summary_file_leaves_the_key_out(self) -> None:
        self.payload("--stable", self.stable(summary=b"\n"))
        self.assertNotIn("summary", self.read_payload()["studio"][0])

    def test_a_summary_that_is_not_utf8_is_refused(self) -> None:
        done = self.payload("--stable", self.stable(summary=b"caf\xe9"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("UTF-8", done.stderr)
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_a_summary_with_a_control_character_or_too_long_is_refused(self) -> None:
        for number, data in enumerate((b"a\x07b", b"a\tb", b"a" * 2001)):
            folder = self.make_release("s%d" % number, "v0.1.%d" % number, summary=data)
            done = self.payload("--stable", folder)
            self.assertEqual(done.returncode, 1, data[:10])
            self.assertIn("summary", done.stderr)
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_the_binary_marker_of_sha512sum(self) -> None:
        names = release_names("v0.1.0")
        marked = tuple(n for n in names if n.endswith((".exe", ".deb")))
        folder = self.make_release("star", "v0.1.0", binary_marker=marked)
        done = self.payload("--stable", folder)
        self.assertEqual(done.returncode, 0, done.stderr)
        files = {f["kind"]: f for f in self.read_payload()["studio"][0]["files"]}
        self.assertEqual(files["windows-installer"]["sha512"], digest_of(marked[0]))

    # -- plugins

    def plugins_file(self, document: Any, name: str = "plugins.json") -> Path:
        return self.write(name, json.dumps(document).encode("utf-8"))

    def test_plugins_are_copied_in_in_the_order_of_the_design(self) -> None:
        entry = good_payload()["plugins"][0]
        shuffled = {k: entry[k] for k in reversed(list(entry))}
        shuffled["files"] = [
            {k: f[k] for k in reversed(list(f))} for f in entry["files"]
        ]
        done = self.payload(
            "--stable", self.stable(), "--plugins", self.plugins_file([shuffled])
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        document = self.read_payload()
        self.assertEqual(document["plugins"], [entry])
        self.assertEqual(list(document["plugins"][0]), list(entry))
        self.assertEqual(list(document["plugins"][0]["files"][0]), list(entry["files"][0]))
        self.assertEqual(list(document), ["feed_version", "serial", "issued_at", "studio", "plugins"])

    def test_an_empty_plugins_file(self) -> None:
        done = self.payload("--stable", self.stable(), "--plugins", self.plugins_file([]))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read_payload()["plugins"], [])

    def test_a_bad_plugin_is_refused_and_nothing_is_written(self) -> None:
        entry = good_payload()["plugins"][0]
        entry["id"] = "com.example.decoding"
        done = self.payload(
            "--stable", self.stable(), "--plugins", self.plugins_file([entry])
        )
        self.assertEqual(done.returncode, 1)
        self.assertIn("plugins[0].id", done.stderr)
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_a_plugins_file_that_is_not_a_list(self) -> None:
        for name, data in (("object", b"{}"), ("text", b"nope"), ("twice", b'[{"a":1,"a":2}]')):
            done = self.payload(
                "--stable", self.stable_once(), "--plugins", self.write(name, data)
            )
            self.assertEqual(done.returncode, 1, name)
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_a_missing_plugins_file(self) -> None:
        done = self.payload("--stable", self.stable(), "--plugins", self.root / "none.json")
        self.assertEqual(done.returncode, 2)
        self.assertIn("none.json", done.stderr)

    # -- refusals

    def refused(self, done: "subprocess.CompletedProcess[str]", code: int, text: str) -> None:
        self.assertEqual(done.returncode, code, done.stderr)
        self.assertIn(text, done.stderr)
        self.assertEqual(done.stdout, "")
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_a_pre_release_given_as_stable(self) -> None:
        folder = self.make_release("pre", "v0.2.0-rc.1")
        self.refused(self.payload("--stable", folder), 1, "--preview")

    def test_a_pre_release_flag_given_as_stable(self) -> None:
        folder = self.make_release("flag", "v0.2.0", prerelease=True)
        self.refused(self.payload("--stable", folder), 1, "pre-release")

    def test_a_final_release_given_as_preview(self) -> None:
        self.refused(self.payload("--preview", self.stable()), 1, "--stable")

    def test_a_flagged_release_with_a_final_version_given_as_preview(self) -> None:
        folder = self.make_release("flag", "v0.2.0", prerelease=True)
        self.refused(self.payload("--preview", folder), 1, "no pre-release part")

    def test_a_missing_checksum_line(self) -> None:
        name = "virtual-serial-studio-v0.1.0-macos-arm64.dmg"
        folder = self.make_release("nosum", "v0.1.0", leave_out=(name,))
        done = self.payload("--stable", folder)
        self.refused(done, 1, "has no line in SHA512SUMS")
        self.assertIn(name, done.stderr)

    def test_a_name_that_github_changed_is_found_in_the_checksums(self) -> None:
        # The deb of a pre-release has a tilde in its name. GitHub uploads it
        # with a dot in that place, and SHA512SUMS keeps the tilde.
        uploaded = "virtual-serial-studio_0.2.0.rc.1-1_amd64.deb"
        built = "virtual-serial-studio_0.2.0~rc.1-1_amd64.deb"
        self.assertEqual(uf.uploaded_name(built), uploaded)
        listed = [
            built if name == uploaded else name
            for name in release_names("v0.2.0-rc.1")
            if not name.endswith(".asc")
            and name not in ("SHA512SUMS", "update-feed.json")
        ]
        self.assertIn(built, listed)
        sums = "".join("%s  %s\n" % (digest_of(name), name) for name in listed)
        folder = self.make_release("tilde", "v0.2.0-rc.1", sums=sums.encode("utf-8"))
        done = self.payload("--preview", folder)
        self.assertEqual(done.returncode, 0, done.stderr)
        files = self.read_payload()["studio"][0]["files"]
        deb = [f for f in files if f["kind"] == "linux-deb"][0]
        self.assertEqual(deb["name"], uploaded)
        self.assertEqual(deb["sha512"], digest_of(built))

    def test_two_checksum_lines_that_github_would_give_one_name(self) -> None:
        uploaded = "virtual-serial-studio_0.2.0.rc.1-1_amd64.deb"
        listed = [
            name
            for name in release_names("v0.2.0-rc.1")
            if not name.endswith(".asc")
            and name not in ("SHA512SUMS", "update-feed.json", uploaded)
        ]
        listed += [
            "virtual-serial-studio_0.2.0~rc.1-1_amd64.deb",
            "virtual-serial-studio_0.2.0+rc.1-1_amd64.deb",
        ]
        sums = "".join("%s  %s\n" % (digest_of(name), name) for name in listed)
        folder = self.make_release("twice", "v0.2.0-rc.1", sums=sums.encode("utf-8"))
        done = self.payload("--preview", folder)
        self.refused(done, 1, "more than one line")
        self.assertFalse((self.root / "feed.payload.json").exists())

    def test_a_checksum_of_a_file_that_is_ignored_is_not_needed(self) -> None:
        folder = self.make_release(
            "ignored", "v0.1.0", leave_out=("sbom.spdx.json", "virtual-serial-studio-v0.1.0-macos-arm64.dmg.asc")
        )
        self.assertEqual(self.payload("--stable", folder).returncode, 0)

    def test_two_files_of_the_same_kind(self) -> None:
        extra = "virtual-serial-studio-v0.1.0-windows-x86_64-unsigned.exe"
        folder = self.make_release("dup", "v0.1.0", add=(extra,))
        done = self.payload("--stable", folder)
        self.refused(done, 1, "windows-installer")
        self.assertIn(extra, done.stderr)

    def test_no_file_that_matches(self) -> None:
        folder = self.root / "none"
        folder.mkdir()
        document = {
            "tag": "v0.1.0", "prerelease": False, "published_at": "2026-12-01T00:00:00Z",
            "html_url": REPO + "/releases/tag/v0.1.0",
            "assets": [
                {"name": n, "size": 5, "url": REPO + "/x/" + n}
                for n in ("SHA512SUMS", "SHA512SUMS.asc", "sbom.spdx.json", "update-feed.json")
            ],
        }
        (folder / "release.json").write_bytes(json.dumps(document).encode())
        (folder / "SHA512SUMS").write_bytes(b"")
        self.refused(self.payload("--stable", folder), 1, "no file that a Studio install uses")

    def test_neither_folder_given(self) -> None:
        done = self.payload()
        self.refused(done, 2, "--stable")

    def test_a_folder_that_is_not_there(self) -> None:
        self.refused(self.payload("--stable", self.root / "absent"), 2, "absent")

    def test_a_folder_without_its_files(self) -> None:
        for missing in ("release.json", "SHA512SUMS"):
            folder = self.make_release("m-" + missing, "v0.1.%d" % len(missing))
            (folder / missing).unlink()
            done = self.payload("--stable", folder)
            self.assertEqual(done.returncode, 2, missing)
            self.assertIn(missing, done.stderr)

    def test_a_tag_that_is_not_a_version(self) -> None:
        for number, tag in enumerate(("release-1", "v1.0", "v01.0.0", "v1.0.0+5", "v1.0.0-dev")):
            folder = self.make_release("t%d" % number, tag)
            done = self.payload("--stable" if "-" not in tag else "--preview", folder)
            self.assertEqual(done.returncode, 1, tag)
            self.assertIn("the tag", done.stderr)

    def test_a_tag_without_a_v_is_taken_as_it_is(self) -> None:
        folder = self.make_release("nov", "v0.1.0")
        document = json.loads((folder / "release.json").read_bytes())
        document["tag"] = "0.1.0"
        (folder / "release.json").write_bytes(json.dumps(document).encode())
        self.assertEqual(self.payload("--stable", folder).returncode, 0)
        self.assertEqual(self.read_payload()["studio"][0]["version"], "0.1.0")

    def test_a_release_file_of_the_wrong_shape(self) -> None:
        for number, change in enumerate((
            lambda d: d.pop("tag"),
            lambda d: d.__setitem__("prerelease", "no"),
            lambda d: d.__setitem__("published_at", "yesterday"),
            lambda d: d.__setitem__("published_at", "2026-12-01"),
            lambda d: d.__setitem__("published_at", "2026-02-30T00:00:00Z"),
            lambda d: d.__setitem__("html_url", 5),
            lambda d: d.__setitem__("assets", {}),
            lambda d: d["assets"].append(5),
        )):
            folder = self.make_release("w%d" % number, "v0.1.0")
            document = json.loads((folder / "release.json").read_bytes())
            change(document)
            (folder / "release.json").write_bytes(json.dumps(document).encode())
            done = self.payload("--stable", folder)
            self.assertEqual(done.returncode, 1, number)
            self.assertNotEqual(done.stderr, "")

    def test_an_address_in_the_release_that_the_feed_refuses(self) -> None:
        folder = self.make_release("http", "v0.1.0")
        document = json.loads((folder / "release.json").read_bytes())
        document["html_url"] = document["html_url"].replace("https", "http")
        (folder / "release.json").write_bytes(json.dumps(document).encode())
        done = self.payload("--stable", folder)
        self.refused(done, 1, "notes_url")

    def test_bad_serial_and_time(self) -> None:
        folder = self.stable()
        done = self.run_tool(
            "payload", "--serial", 0, "--issued-at", "2026-12-01T03:40:00Z",
            "--stable", folder, "--out", self.root / "feed.payload.json",
        )
        self.refused(done, 1, "serial")
        done = self.run_tool(
            "payload", "--serial", 1, "--issued-at", "2026-12-01 03:40",
            "--stable", folder, "--out", self.root / "feed.payload.json",
        )
        self.refused(done, 1, "issued_at")
        done = self.run_tool(
            "payload", "--serial", "x", "--issued-at", "2026-12-01T03:40:00Z",
            "--stable", folder, "--out", self.root / "feed.payload.json",
        )
        self.assertEqual(done.returncode, 2)

    def test_the_plugins_file_is_never_the_output(self) -> None:
        plugins = self.plugins_file([])
        done = self.run_tool(
            "payload", "--serial", 1, "--issued-at", "2026-12-01T03:40:00Z",
            "--stable", self.stable(), "--plugins", plugins, "--out", plugins,
        )
        self.assertEqual(done.returncode, 2)
        self.assertEqual(plugins.read_bytes(), b"[]")


# -- Signing and verifying: OpenSSL 3 ----------------------------------------


def big_payload() -> bytes:
    """A payload that the reader accepts and whose envelope is over 64 KiB:
    two releases with 14 files each and a 2048-character address on each."""
    document = good_payload()
    address = "https://example.com/" + "a" * (2048 - 20)
    for entry in document["studio"]:
        entry["files"] = more_files(14)
        for item in entry["files"]:
            item["url"] = address
    document["plugins"] = []
    return text_of(document).encode("utf-8")


@needs_openssl
class SigningTests(ToolTestCase):
    def payload_file(self, document: Optional[Dict[str, Any]] = None) -> Path:
        return self.write(
            "feed.payload.json", text_of(document or good_payload()).encode("utf-8")
        )

    def sign(
        self,
        payload: Optional[Path] = None,
        key: Optional[Path] = None,
        key_id: str = "upd-2026-a",
        out: str = "update-feed.json",
    ) -> "subprocess.CompletedProcess[str]":
        return self.run_tool(
            "sign", "--key", key or KEY, "--key-id", key_id,
            "--payload", payload or self.payload_file(), "--out", self.root / out,
        )

    def verify(
        self, path: Optional[Path] = None, key: str = "", *extra: Any
    ) -> "subprocess.CompletedProcess[str]":
        return self.run_tool(
            "verify", "--public-key", key or PUBLIC_KEY, *extra,
            path or (self.root / "update-feed.json"),
        )

    def signed_envelope(self, purpose: str, payload: bytes, key: Optional[Path] = None) -> Path:
        text = ps.sign_envelope(
            ps.find_openssl(), str(key or KEY), purpose, "upd-2026-a", payload
        )
        return self.write("other.json", text.encode("ascii"))

    # -- sign

    def test_sign_then_verify(self) -> None:
        payload = self.payload_file()
        done = self.sign(payload)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stderr, "")
        self.assertIn("update-feed.json", done.stdout)
        feed = (self.root / "update-feed.json").read_bytes()
        self.assertTrue(feed.endswith(b"}\n"))
        self.assertNotIn(b"\r", feed)
        found = ps.parse_envelope(feed)
        self.assertEqual(found.purpose, "update-feed")
        self.assertEqual(found.key_id, "upd-2026-a")
        self.assertEqual(found.payload, payload.read_bytes())
        self.assertEqual(
            list(json.loads(feed)),
            ["format", "purpose", "key_id", "payload", "signature"],
        )
        ok = self.verify()
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual(ok.stdout, "ok serial 12\n")
        self.assertEqual(ok.stderr, "")

    def test_the_signature_covers_the_tag_the_purpose_and_the_payload(self) -> None:
        self.sign()
        found = ps.parse_envelope((self.root / "update-feed.json").read_bytes())
        message = b"VSS-SIGNED-FILE-1\x00update-feed\x00" + found.payload
        self.assertTrue(
            ps.verify_bytes(ps.find_openssl(), bytes.fromhex(PUBLIC_KEY), message, found.signature)
        )

    def test_from_the_payload_command_to_a_verified_feed(self) -> None:
        stable = self.make_release("stable", "v0.1.0")
        preview = self.make_release("preview", "v0.2.0-rc.1")
        done = self.payload("--stable", stable, "--preview", preview)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.sign(self.root / "feed.payload.json").returncode, 0)
        ok = self.verify()
        self.assertEqual(ok.stdout, "ok serial 12\n", ok.stderr)

    def test_the_same_payload_and_key_give_the_same_feed(self) -> None:
        payload = self.payload_file()
        self.sign(payload, out="one.json")
        self.sign(payload, out="two.json")
        self.assertEqual(
            (self.root / "one.json").read_bytes(), (self.root / "two.json").read_bytes()
        )

    def test_a_bad_key_id_is_refused_before_anything_is_signed(self) -> None:
        for key_id in ("", "Upd-1", "upd_1", "upd.1", "a b", "x" * 65, "upd\u00e9"):
            done = self.sign(key_id=key_id, out="bad.json")
            self.assertEqual(done.returncode, 2, key_id)
            self.assertIn("key id", done.stderr)
            self.assertFalse((self.root / "bad.json").exists())
        self.assertEqual(self.sign(key_id="x" * 64).returncode, 0)

    def test_a_payload_that_would_be_refused_is_not_signed(self) -> None:
        document = good_payload()
        document["serial"] = 0
        done = self.sign(self.payload_file(document))
        self.assertEqual(done.returncode, 1)
        self.assertIn("serial", done.stderr)
        self.assertFalse((self.root / "update-feed.json").exists())

    def test_a_payload_that_is_not_json_is_not_signed(self) -> None:
        done = self.sign(self.write("junk.json", b"not json"))
        self.assertEqual(done.returncode, 1)
        self.assertFalse((self.root / "update-feed.json").exists())

    def test_a_feed_over_64_kib_is_not_written(self) -> None:
        payload = self.write("big.json", big_payload())
        self.assertEqual(uf.check_payload(payload.read_bytes()), [])
        done = self.sign(payload)
        self.assertEqual(done.returncode, 1)
        self.assertIn("65536", done.stderr)
        self.assertFalse((self.root / "update-feed.json").exists())

    def test_the_key_file_must_exist(self) -> None:
        done = self.sign(key=self.root / "no.key")
        self.assertEqual(done.returncode, 2)
        self.assertIn("no.key", done.stderr)

    def test_a_file_that_is_no_key(self) -> None:
        done = self.sign(key=self.write("junk.pem", b"not a key\n"))
        self.assertEqual(done.returncode, 1)
        self.assertFalse((self.root / "update-feed.json").exists())

    def test_the_output_is_never_the_key_or_the_payload(self) -> None:
        payload = self.payload_file()
        before = payload.read_bytes()
        done = self.run_tool(
            "sign", "--key", KEY, "--key-id", "upd-2026-a", "--payload", payload,
            "--out", payload,
        )
        self.assertEqual(done.returncode, 2)
        self.assertEqual(payload.read_bytes(), before)

    # -- verify

    def test_verify_refuses_a_changed_payload(self) -> None:
        self.sign()
        found = ps.parse_envelope((self.root / "update-feed.json").read_bytes())
        document = good_payload()
        document["serial"] = 13
        changed = ps.envelope_text(
            "update-feed", found.key_id, text_of(document).encode("utf-8"), found.signature
        )
        path = self.write("changed.json", changed.encode("ascii"))
        done = self.verify(path)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertIn("signature does not hold", done.stderr)
        self.assertEqual(len(done.stderr.strip().splitlines()), 1)

    def test_verify_refuses_one_changed_byte(self) -> None:
        self.sign()
        found = ps.parse_envelope((self.root / "update-feed.json").read_bytes())
        payload = bytearray(found.payload)
        payload[payload.index(b"12")] ^= 1
        path = self.write(
            "flipped.json",
            ps.envelope_text("update-feed", "upd-2026-a", bytes(payload), found.signature).encode("ascii"),
        )
        self.assertEqual(self.verify(path).returncode, 1)

    def test_verify_refuses_another_key(self) -> None:
        self.sign()
        done = self.verify(key=OTHER_PUBLIC_KEY)
        self.assertEqual(done.returncode, 1)
        self.assertIn("signature does not hold", done.stderr)
        self.assertEqual(done.stdout, "")

    def test_verify_refuses_another_purpose(self) -> None:
        payload = text_of(good_payload()).encode("utf-8")
        for purpose in ("plugin", "key-list", "licence", "update-feeds"):
            path = self.signed_envelope(purpose, payload)
            done = self.verify(path)
            self.assertEqual(done.returncode, 1, purpose)
            self.assertIn("signed as", done.stderr)
            self.assertEqual(done.stdout, "")

    def test_verify_refuses_a_purpose_that_was_rewritten(self) -> None:
        # A signature made for another purpose does not hold for this one,
        # even when the envelope says update-feed.
        payload = text_of(good_payload()).encode("utf-8")
        other = ps.parse_envelope(self.signed_envelope("plugin", payload).read_bytes())
        path = self.write(
            "rewritten.json",
            ps.envelope_text("update-feed", "upd-2026-a", payload, other.signature).encode("ascii"),
        )
        done = self.verify(path)
        self.assertEqual(done.returncode, 1)
        self.assertIn("signature does not hold", done.stderr)

    def test_verify_with_another_key_id(self) -> None:
        self.sign()
        self.assertEqual(
            self.verify(None, "", "--key-id", "upd-2026-a").stdout, "ok serial 12\n"
        )
        done = self.verify(None, "", "--key-id", "upd-2026-b")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertIn("upd-2026-b", done.stderr)

    def test_verify_with_a_serial_below_the_minimum(self) -> None:
        self.sign()
        done = self.verify(None, "", "--min-serial", 13)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertIn("serial 12", done.stderr)
        self.assertIn("13", done.stderr)
        self.assertEqual(self.verify(None, "", "--min-serial", 12).stdout, "ok serial 12\n")
        self.assertEqual(self.verify(None, "", "--min-serial", 1).returncode, 0)

    def test_verify_refuses_a_file_over_64_kib(self) -> None:
        self.sign()
        good = (self.root / "update-feed.json").read_bytes()
        padded = self.write("padded.json", good + b" " * ps.MAX_ENVELOPE_SIZE)
        done = self.verify(padded)
        self.assertEqual(done.returncode, 1)
        self.assertIn("65536", done.stderr)
        self.assertEqual(done.stdout, "")
        # Exactly 64 KiB is within the limit.
        exact = self.write("exact.json", good + b" " * (ps.MAX_ENVELOPE_SIZE - len(good)))
        done = self.verify(exact)
        self.assertNotIn("65536", done.stderr)

    def test_verify_refuses_a_signed_feed_over_the_limit_that_was_made_by_hand(self) -> None:
        text = ps.envelope_text(
            "update-feed", "upd-2026-a", big_payload(), bytes(64)
        )
        self.assertGreater(len(text), ps.MAX_ENVELOPE_SIZE)
        done = self.verify(self.write("big.json", text.encode("ascii")))
        self.assertEqual(done.returncode, 1)
        self.assertIn("65536", done.stderr)

    def test_verify_refuses_a_good_signature_over_a_bad_payload(self) -> None:
        document = good_payload()
        document["feed_version"] = 2
        document["extra"] = True
        path = self.signed_envelope("update-feed", text_of(document).encode("utf-8"))
        done = self.verify(path)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertIn("payload is not valid", done.stderr)
        self.assertEqual(len(done.stderr.strip().splitlines()), 1)

    def test_verify_refuses_what_is_not_an_envelope(self) -> None:
        for number, data in enumerate((b"", b"[]", b"not json", b'{"format":1}\n')):
            done = self.verify(self.write("e%d.json" % number, data))
            self.assertEqual(done.returncode, 1, data)
            self.assertEqual(done.stdout, "")

    def test_verify_with_a_missing_file_or_a_bad_key(self) -> None:
        self.sign()
        done = self.verify(self.root / "absent.json")
        self.assertEqual(done.returncode, 2)
        for key in ("zz" * 32, "ab" * 31, "", "ab" * 33, str(KEY)):
            done = self.run_tool(
                "verify", "--public-key", key, self.root / "update-feed.json"
            )
            self.assertEqual(done.returncode, 2, key)

    def test_the_key_is_never_shown(self) -> None:
        assert KEY is not None
        body = KEY.read_text(encoding="ascii").splitlines()[1]
        done = self.sign()
        self.assertNotIn(body, done.stdout + done.stderr)
        self.assertNotIn(body.encode(), (self.root / "update-feed.json").read_bytes())
        self.assertNotIn(
            base64.b64decode(body)[-32:], (self.root / "update-feed.json").read_bytes()
        )


# -- The keys Studio is built with -------------------------------------------

REAL_KEYS_FILE = HERE.parent / "libs" / "vss_update" / "src" / "trusted_keys_release.cpp"
LICENCE_KEYS_FILE = HERE.parent / "libs" / "vss_license" / "src" / "trusted_keys_release.cpp"

# Not keys: bytes that make the reader's tests readable. The signing tests use
# the throwaway keys that OpenSSL made in setUpModule.
BYTES_A = bytes(range(32))
BYTES_B = bytes(range(100, 132))

CPP_HEAD = """\
// SPDX-License-Identifier: GPL-3.0-or-later
#include <vss/update/feed.hpp>

namespace vss::update
{

namespace
{

"""

CPP_MAKE = """
trusted_key make(std::string_view key_id, std::string_view purpose,
                 const public_key& key)
{
    trusted_key entry;
    entry.key_id = std::string(key_id);
    entry.purpose = std::string(purpose);
    entry.key = key;
    return entry;
}

} // namespace

"""


def array_text(name: str, data: bytes) -> str:
    items = ["0x%02x" % b for b in data]
    rows = [", ".join(items[i:i + 8]) for i in range(0, len(items), 8)]
    return "constexpr public_key %s{%s};\n" % (name, ",\n    ".join(rows))


def array_name(key_id: str) -> str:
    return key_id.replace("-", "_")


def keys_cpp(keys: List[Tuple[str, bytes]]) -> str:
    """A file in the form that ``read_feed_keys`` reads, as the owner would
    fill the file in the repository."""
    text = CPP_HEAD
    for key_id, data in keys:
        text += "// base64url made up for the test\n" + array_text(array_name(key_id), data)
    text += CPP_MAKE
    text += "std::span<const vss::license::signed_file::trusted_key> builtin_feed_keys()\n{\n"
    if keys:
        entries = ",\n        ".join(
            'make("%s", feed_purpose, %s)' % (key_id, array_name(key_id)) for key_id, _ in keys
        )
        text += "    static const std::array<trusted_key, %d> keys{\n        %s};\n    return keys;\n" % (
            len(keys), entries,
        )
    else:
        text += "    return {};\n"
    return text + "}\n\n} // namespace vss::update\n"


TWO_KEYS = [("upd-2026-a", BYTES_A), ("upd-2026-b", BYTES_B)]


class ReadFeedKeysTests(ToolTestCase):
    def read(self, text: str) -> List[uf.FeedKey]:
        path = self.write("keys.cpp", text.encode("utf-8"))
        return uf.read_feed_keys(str(path))

    def refused(self, text: "str | bytes") -> str:
        path = self.write("keys.cpp", text if isinstance(text, bytes) else text.encode("utf-8"))
        with self.assertRaises(ps.Refusal) as caught:
            uf.read_feed_keys(str(path))
        self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
        self.assertNotIn("\n", caught.exception.message)
        return caught.exception.message

    def test_the_file_in_the_repository_lists_no_key(self) -> None:
        self.assertEqual(uf.read_feed_keys(str(REAL_KEYS_FILE)), [])

    def test_two_keys_in_the_order_of_the_file(self) -> None:
        keys = self.read(keys_cpp(TWO_KEYS))
        self.assertEqual(
            keys,
            [uf.FeedKey("upd-2026-a", BYTES_A), uf.FeedKey("upd-2026-b", BYTES_B)],
        )

    def test_one_key_and_an_empty_list_in_the_form_of_the_fixture(self) -> None:
        self.assertEqual(self.read(keys_cpp(TWO_KEYS[:1])), [uf.FeedKey("upd-2026-a", BYTES_A)])
        self.assertEqual(self.read(keys_cpp([])), [])

    def test_the_layout_of_the_licence_file_is_read(self) -> None:
        # The owner fills the file in the way the licence file is filled: the
        # arrays in the licence file's layout (eight bytes to a line), the
        # same `make`, the same list. Only the purposes and the names change.
        text = LICENCE_KEYS_FILE.read_text(encoding="utf-8")
        for old, new in (
            ('make("lic-2026-a", licence_purpose, lic_2026_a),\n'
             '        make("lic-2026-a", revocations_purpose, lic_2026_a),\n'
             '        make("lic-2026-b", licence_purpose, lic_2026_b),\n'
             '        make("lic-2026-b", revocations_purpose, lic_2026_b)};',
             'make("lic-2026-a", feed_purpose, lic_2026_a),\n'
             '        make("lic-2026-b", feed_purpose, lic_2026_b)};'),
            ("std::array<trusted_key, 4>", "std::array<trusted_key, 2>"),
        ):
            self.assertIn(old, text)
            text = text.replace(old, new)
        keys = self.read(text)
        self.assertEqual([k.key_id for k in keys], ["lic-2026-a", "lic-2026-b"])
        self.assertEqual(keys[0].public_key[:4], bytes([0xEF, 0x26, 0x8C, 0xD4]))
        self.assertEqual(len(keys[1].public_key), 32)

    def test_a_namespace_in_front_of_the_names_is_allowed(self) -> None:
        text = keys_cpp(TWO_KEYS)
        text = text.replace("constexpr public_key", "constexpr vss::license::signed_file::public_key")
        text = text.replace("feed_purpose,", "vss::update::feed_purpose,")
        text = text.replace("std::array<trusted_key", "std::array<vss::license::signed_file::trusted_key")
        self.assertEqual(len(self.read(text)), 2)

    def test_comments_are_not_read(self) -> None:
        text = keys_cpp(TWO_KEYS)
        text = text.replace(
            "namespace\n{",
            '// make("upd-2026-c", feed_purpose, upd_2026_c)\n'
            "/* constexpr public_key upd_2026_c{0x00}; */\n"
            "// not_after\nnamespace\n{",
        )
        self.assertEqual(len(self.read(text)), 2)
        # A comment sign inside a string is not a comment.
        self.assertEqual(len(self.read(keys_cpp(TWO_KEYS) + 'const char* x = "//";\n')), 2)

    def test_a_key_of_another_length_is_refused(self) -> None:
        for size in (0, 1, 31, 33, 64):
            with self.subTest(size=size):
                message = self.refused(keys_cpp([("upd-2026-a", bytes(range(size)))]))
                self.assertIn("upd_2026_a", message)
                self.assertIn("32 bytes", message)

    def test_a_key_written_in_another_way_is_refused(self) -> None:
        good = keys_cpp(TWO_KEYS[:1])
        for label, text in (
            ("decimal", good.replace("0x00", "0")),
            ("one digit", good.replace("0x01,", "0x1,")),
            ("trailing comma", good.replace("0x1f}", "0x1f,}")),
            ("a number that is not a byte", good.replace("0x01,", "0x100,")),
            ("no constexpr", good.replace("constexpr public_key", "const public_key")),
            ("an equals sign", good.replace("upd_2026_a{", "upd_2026_a = {")),
        ):
            with self.subTest(label):
                self.refused(text)

    def test_an_id_with_no_array_is_refused(self) -> None:
        text = keys_cpp(TWO_KEYS).replace("feed_purpose, upd_2026_b", "feed_purpose, upd_2026_c")
        message = self.refused(text)
        self.assertIn("upd-2026-b", message)
        self.assertIn("not a key array", message)

    def test_an_array_with_no_entry_is_refused(self) -> None:
        text = keys_cpp(TWO_KEYS)
        text = text.replace(CPP_MAKE, array_text("upd_2026_c", BYTES_A) + CPP_MAKE)
        self.assertIn("upd_2026_c", self.refused(text))

    def test_a_key_listed_twice_is_refused(self) -> None:
        both = keys_cpp(TWO_KEYS)
        self.assertIn(
            "twice",
            self.refused(both.replace('make("upd-2026-b", feed_purpose, upd_2026_b)',
                                      'make("upd-2026-a", feed_purpose, upd_2026_b)')),
        )
        self.assertIn(
            "twice",
            self.refused(both.replace("feed_purpose, upd_2026_b", "feed_purpose, upd_2026_a")),
        )
        self.assertIn("twice", self.refused(both.replace(CPP_MAKE, array_text("upd_2026_a", BYTES_B) + CPP_MAKE)))

    def test_a_bad_id_or_another_purpose_is_refused(self) -> None:
        good = keys_cpp(TWO_KEYS)
        for label, text in (
            ("capital", good.replace('"upd-2026-a"', '"Upd-2026-a"')),
            ("underscore", good.replace('"upd-2026-a"', '"upd_2026_a"')),
            ("empty", good.replace('"upd-2026-a"', '""')),
            ("too long", good.replace('"upd-2026-a"', '"%s"' % ("a" * 65))),
            ("licence", good.replace("feed_purpose, upd_2026_a", "licence_purpose, upd_2026_a")),
            ("a string", good.replace("feed_purpose, upd_2026_a", '"update-feed", upd_2026_a')),
            ("an expression", good.replace("upd_2026_a)", "upd_2026_a + 1)")),
        ):
            with self.subTest(label):
                self.refused(text)

    def test_the_size_of_the_list_must_be_the_number_of_entries(self) -> None:
        good = keys_cpp(TWO_KEYS)
        self.assertIn("std::array<trusted_key, 2>", good)
        for count in (1, 3, 0):
            with self.subTest(count=count):
                self.refused(good.replace("std::array<trusted_key, 2>", "std::array<trusted_key, %d>" % count))
        self.refused(good.replace("std::array<trusted_key, 2> keys{", "std::array<int, 2> keys{"))

    def test_a_key_window_is_refused(self) -> None:
        for name in ("not_before", "not_after", "revoked_from"):
            with self.subTest(name):
                text = keys_cpp(TWO_KEYS).replace("entry.key = key;", "entry.key = key;\n    entry.%s = {};" % name)
                self.assertIn(name, self.refused(text))

    def test_a_file_with_no_key_must_be_the_empty_list(self) -> None:
        self.refused(CPP_HEAD)
        self.refused(keys_cpp([]).replace("return {};", "return std::span<const int>{};"))
        self.refused(
            keys_cpp([]).replace(
                "return {};", "static const std::array<trusted_key, 0> keys{};\n    return keys;"
            )
        )

    def test_an_entry_made_another_way_is_not_taken_for_an_empty_list(self) -> None:
        text = keys_cpp([]).replace("return {};", "return {add(feed_purpose, k)};")
        self.refused(text.replace(CPP_MAKE, array_text("k", BYTES_A) + CPP_MAKE))

    def test_a_file_that_cannot_be_read(self) -> None:
        self.refused(keys_cpp(TWO_KEYS) + "/* not closed\n")
        self.refused(b"\xff\xfe" + keys_cpp(TWO_KEYS).encode("utf-8"))
        self.refused(keys_cpp(TWO_KEYS) + "//" + "x" * uf.KEYS_FILE_LIMIT)
        with self.assertRaises(ps.Refusal) as caught:
            uf.read_feed_keys(str(self.root / "missing.cpp"))
        self.assertEqual(caught.exception.code, ps.EXIT_USAGE)

    def test_the_keys_command(self) -> None:
        path = self.write("keys.cpp", keys_cpp(TWO_KEYS).encode("utf-8"))
        done = self.run_tool("keys", "--keys-from", path)
        self.assertEqual((done.returncode, done.stdout, done.stderr), (0, "upd-2026-a\nupd-2026-b\n", ""))

    def test_the_keys_command_in_the_repository_prints_nothing(self) -> None:
        done = self.run_tool("keys", "--keys-from", REAL_KEYS_FILE)
        self.assertEqual((done.returncode, done.stdout, done.stderr), (0, "", ""))

    def test_the_keys_command_refuses_a_file_it_cannot_read(self) -> None:
        path = self.write("keys.cpp", keys_cpp([("upd-2026-a", bytes(31))]).encode("utf-8"))
        done = self.run_tool("keys", "--keys-from", path)
        self.assertEqual((done.returncode, done.stdout), (2, ""))
        self.assertEqual(len(done.stderr.strip().splitlines()), 1)
        self.assertEqual(self.run_tool("keys", "--keys-from", self.root / "no.cpp").returncode, 2)
        self.assertEqual(self.run_tool("keys").returncode, 2)

    def test_the_comment_of_the_file_in_the_repository_tells_the_form(self) -> None:
        text = REAL_KEYS_FILE.read_text(encoding="utf-8")
        for words in ("constexpr public_key", "make(", "feed_purpose", "std::array<trusted_key, N>", "return {};"):
            self.assertIn(words, text)


@needs_openssl
class VerifyWithKeysTests(ToolTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.payload = self.write("feed.payload.json", text_of(good_payload()).encode("utf-8"))
        self.keys = self.write(
            "keys.cpp",
            keys_cpp(
                [("upd-2026-a", bytes.fromhex(PUBLIC_KEY)), ("upd-2026-b", bytes.fromhex(OTHER_PUBLIC_KEY))]
            ).encode("utf-8"),
        )

    def sign(self, key: Optional[Path], key_id: str, out: str = "update-feed.json") -> Path:
        done = self.run_tool(
            "sign", "--key", key, "--key-id", key_id, "--payload", self.payload, "--out", self.root / out
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        return self.root / out

    def verify(self, feed: Path, *extra: Any, keys: Optional[Path] = None) -> "subprocess.CompletedProcess[str]":
        return self.run_tool("verify", "--keys-from", keys or self.keys, *extra, feed)

    def test_each_key_verifies_the_feed_that_names_it(self) -> None:
        for key, key_id in ((KEY, "upd-2026-a"), (OTHER_KEY, "upd-2026-b")):
            with self.subTest(key_id):
                done = self.verify(self.sign(key, key_id))
                self.assertEqual((done.returncode, done.stdout, done.stderr), (0, "ok serial 12\n", ""))

    def test_the_right_key_under_the_wrong_id_does_not_verify(self) -> None:
        done = self.verify(self.sign(OTHER_KEY, "upd-2026-a"))
        self.assertEqual((done.returncode, done.stdout), (1, ""))
        self.assertIn("signature does not hold", done.stderr)
        done = self.verify(self.sign(KEY, "upd-2026-b"))
        self.assertEqual(done.returncode, 1)

    def test_a_key_that_is_not_in_the_file(self) -> None:
        feed = self.sign(KEY, "upd-2026-c")
        done = self.verify(feed)
        self.assertEqual((done.returncode, done.stdout), (1, ""))
        self.assertIn("upd-2026-c", done.stderr)
        self.assertIn("does not trust", done.stderr)

    def test_a_secret_that_is_not_a_key_studio_trusts_fails(self) -> None:
        # What the job guards against: the key that signed is not any of the
        # public keys that the file lists.
        keys = self.write(
            "other.cpp", keys_cpp([("upd-2026-a", BYTES_A), ("upd-2026-b", BYTES_B)]).encode("utf-8")
        )
        done = self.verify(self.sign(KEY, "upd-2026-a"), keys=keys)
        self.assertEqual(done.returncode, 1)

    def test_a_build_with_no_key_has_its_own_exit_status(self) -> None:
        feed = self.sign(KEY, "upd-2026-a")
        empty = self.write("empty.cpp", keys_cpp([]).encode("utf-8"))
        bad = self.write("bad.json", feed.read_bytes().replace(b"serial", b"seria1"))
        for keys in (REAL_KEYS_FILE, empty):
            done = self.verify(feed, keys=keys)
            self.assertEqual((done.returncode, done.stdout), (3, ""), keys)
            self.assertIn("trusts none", done.stderr)
            self.assertEqual(len(done.stderr.strip().splitlines()), 1)
            # It is the answer for a file that does not verify as well.
            self.assertEqual(self.verify(bad, keys=keys).returncode, 3)
            self.assertEqual(self.verify(self.root / "none.json", keys=keys).returncode, 3)
        self.assertNotEqual(self.verify(bad).returncode, 3)
        self.assertEqual(uf.EXIT_NO_KEYS, 3)

    def test_a_keys_file_that_cannot_be_read_is_a_usage_error(self) -> None:
        feed = self.sign(KEY, "upd-2026-a")
        bad = self.write("bad.cpp", keys_cpp([("upd-2026-a", bytes(30))]).encode("utf-8"))
        self.assertEqual(self.verify(feed, keys=bad).returncode, 2)
        self.assertEqual(self.verify(feed, keys=self.root / "none.cpp").returncode, 2)

    def test_one_way_of_giving_the_key(self) -> None:
        feed = self.sign(KEY, "upd-2026-a")
        both = self.run_tool("verify", "--keys-from", self.keys, "--public-key", PUBLIC_KEY, feed)
        self.assertEqual(both.returncode, 2)
        neither = self.run_tool("verify", feed)
        self.assertEqual(neither.returncode, 2)

    def test_the_other_options_still_apply(self) -> None:
        feed = self.sign(KEY, "upd-2026-a")
        self.assertEqual(self.verify(feed, "--key-id", "upd-2026-a").returncode, 0)
        self.assertEqual(self.verify(feed, "--key-id", "upd-2026-b").returncode, 1)
        self.assertEqual(self.verify(feed, "--min-serial", "12").returncode, 0)
        self.assertEqual(self.verify(feed, "--min-serial", "13").returncode, 1)

    def test_the_function_the_website_check_calls(self) -> None:
        feed = self.sign(KEY, "upd-2026-a")
        data = feed.read_bytes()
        self.assertEqual(uf.verify_with_keys_file(data, "feed.json", str(self.keys)), 12)
        with self.assertRaises(uf.NoKeys):
            uf.verify_with_keys_file(data, "feed.json", str(REAL_KEYS_FILE))
        other = self.write(
            "other.cpp", keys_cpp([("upd-2026-a", BYTES_A)]).encode("utf-8")
        )
        with self.assertRaises(ps.Refusal) as caught:
            uf.verify_with_keys_file(data, "feed.json", str(other))
        self.assertEqual(caught.exception.code, ps.EXIT_FAILED)
        self.assertNotIsInstance(caught.exception, uf.NoKeys)


# -- Collecting the inputs from the releases ---------------------------------

REPO_NAME = "iainchesworthlabs/virtual-serial-studio"


def api_release(
    tag: str,
    prerelease: Optional[bool] = None,
    draft: bool = False,
    published_at: str = "2026-12-01T03:35:45Z",
    body: Any = "Notes of the release.",
    names: Optional[List[str]] = None,
    **changes: Any,
) -> Dict[str, Any]:
    """A release as ``GET /repos/{owner}/{repo}/releases`` gives it."""
    entry: Dict[str, Any] = {
        "tag_name": tag,
        "name": tag,
        "draft": draft,
        "prerelease": ("-" in tag) if prerelease is None else prerelease,
        "published_at": None if draft else published_at,
        "html_url": "%s/releases/tag/%s" % (REPO, tag),
        "body": body,
        "assets": [
            {
                "id": index,
                "name": name,
                "size": 1000 + len(name),
                "browser_download_url": "%s/releases/download/%s/%s" % (REPO, tag, name),
            }
            for index, name in enumerate(release_names(tag) if names is None else names)
        ],
    }
    entry.update(changes)
    return entry


class FakeDownload:
    """What ``download_bytes`` is replaced with: the SHA512SUMS of a release,
    made from the names of its files, and a record of what was asked for."""

    def __init__(self) -> None:
        self.urls: List[str] = []

    def __call__(self, url: str, limit: int) -> bytes:
        self.urls.append(url)
        tag = url.split("/download/")[1].split("/")[0]
        names = [
            name
            for name in release_names(tag)
            if not name.endswith(".asc") and name not in ("SHA512SUMS", "update-feed.json")
        ]
        return "".join("%s  %s\n" % (digest_of(name), name) for name in sorted(names)).encode("utf-8")


class CollectTests(ToolTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.out = self.root / "feed-input"
        self.download = FakeDownload()

    def collect(self, entries: List[Any]) -> int:
        with contextlib.redirect_stderr(io.StringIO()):
            return uf.collect(REPO_NAME, entries, str(self.out), self.download)

    def collect_with_notes(self, entries: List[Any]) -> Tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            serial = uf.collect(REPO_NAME, entries, str(self.out), self.download)
        return serial, err.getvalue()

    def tag_of(self, folder: str) -> str:
        document = json.loads((self.out / folder / "release.json").read_bytes())
        return str(document["tag"])

    def test_the_newest_by_version_and_not_by_date_or_place(self) -> None:
        entries = [
            api_release("v0.1.0", published_at="2026-12-09T00:00:00Z"),
            api_release("v0.2.0", published_at="2026-12-01T00:00:00Z"),
            api_release("v0.3.0-rc.2", published_at="2026-11-01T00:00:00Z"),
            api_release("v0.3.0-rc.10", published_at="2026-10-01T00:00:00Z"),
            api_release("v0.3.0-rc.9", published_at="2026-12-20T00:00:00Z"),
            api_release("v0.1.5", published_at="2026-12-30T00:00:00Z"),
        ]
        self.collect(entries)
        self.assertEqual(self.tag_of("stable"), "v0.2.0")
        self.assertEqual(self.tag_of("preview"), "v0.3.0-rc.10")

    def test_a_draft_is_ignored_and_not_counted(self) -> None:
        entries = [
            api_release("v0.9.0", draft=True),
            api_release("v0.8.0-rc.1", draft=True),
            api_release("v0.2.0"),
            api_release("v0.3.0-rc.1"),
        ]
        serial = self.collect(entries)
        self.assertEqual(serial, 2)
        self.assertEqual((self.tag_of("stable"), self.tag_of("preview")), ("v0.2.0", "v0.3.0-rc.1"))

    def test_a_pre_release_that_is_not_newer_is_left_out_and_says_so(self) -> None:
        for older in ("v0.2.0-rc.3", "v0.1.9-rc.1"):
            with self.subTest(older):
                serial, notes = self.collect_with_notes([api_release(older), api_release("v0.2.0")])
                self.assertEqual(serial, 2)
                self.assertTrue((self.out / "stable").is_dir())
                self.assertFalse((self.out / "preview").exists())
                self.assertIn("left out", notes)

    def test_only_pre_releases_make_a_preview_folder_only(self) -> None:
        self.collect([api_release("v0.1.0-rc.5"), api_release("v0.1.0-rc.4")])
        self.assertFalse((self.out / "stable").exists())
        self.assertEqual(self.tag_of("preview"), "v0.1.0-rc.5")

    def test_nothing_to_publish_is_refused(self) -> None:
        for entries in ([], [api_release("v0.1.0", draft=True)], [api_release("nightly")]):
            with self.subTest(entries=len(entries)), self.assertRaises(ps.Refusal) as caught:
                self.collect(entries)
            self.assertIn("no published release", caught.exception.message)
            self.assertEqual(caught.exception.code, ps.EXIT_FAILED)

    def test_the_serial_is_the_number_of_published_releases(self) -> None:
        entries = [
            api_release("v0.2.0"),
            api_release("v0.2.1", draft=True),
            api_release("v0.2.0-rc.1"),
            api_release("v0.1.0"),
            api_release("not-a-version"),
            api_release("v0.1.0-rc.9"),
        ]
        self.assertEqual(self.collect(entries), 5)
        self.assertEqual(json.loads((self.out / "serial.json").read_bytes()), {"serial": 5})
        self.assertTrue((self.out / "serial.json").read_bytes().endswith(b"\n"))

    def test_the_serial_goes_up_with_a_release(self) -> None:
        entries = [api_release("v0.1.0")]
        first = self.collect(entries)
        second = self.collect([api_release("v0.2.0")] + entries)
        self.assertEqual(second, first + 1)

    def test_a_folder_of_the_tool_s_own_shape(self) -> None:
        self.collect([api_release("v0.1.0-rc.5")])
        folder = self.out / "preview"
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["SHA512SUMS", "release.json", "summary.txt"])
        document = json.loads((folder / "release.json").read_bytes())
        self.assertEqual(
            sorted(document), ["assets", "html_url", "prerelease", "published_at", "tag"]
        )
        self.assertEqual(document["html_url"], "%s/releases/tag/v0.1.0-rc.5" % REPO)
        self.assertEqual([a["name"] for a in document["assets"]], sorted(a["name"] for a in document["assets"]))
        self.assertEqual(sorted(document["assets"][0]), ["name", "size", "url"])
        self.assertTrue((folder / "release.json").read_bytes().endswith(b"\n"))
        self.assertNotIn(b"\r", (folder / "release.json").read_bytes())
        # Only the file of the sums is downloaded, from the release's own address.
        self.assertEqual(
            self.download.urls, ["%s/releases/download/v0.1.0-rc.5/SHA512SUMS" % REPO]
        )

    def test_a_folder_that_payload_reads(self) -> None:
        self.collect(
            [api_release("v0.1.0"), api_release("v0.2.0-rc.1", body="What is new.\r\n\r\nSecond line.\r\n")]
        )
        done = self.run_tool(
            "payload", "--serial", 2, "--issued-at", "2026-12-01T03:40:00Z",
            "--stable", self.out / "stable", "--preview", self.out / "preview",
            "--out", self.root / "feed.payload.json",
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        document = self.read_payload()
        self.assertEqual(
            [(e["channel"], e["version"]) for e in document["studio"]],
            [("stable", "0.1.0"), ("preview", "0.2.0-rc.1")],
        )
        self.assertEqual(document["studio"][1]["summary"], "What is new.\n\nSecond line.")
        self.assertEqual(len(document["studio"][0]["files"]), 7)

    @needs_openssl
    def test_from_the_releases_to_a_feed_that_verifies_with_the_keys_file(self) -> None:
        self.collect([api_release("v0.1.0"), api_release("v0.2.0-rc.1")])
        keys = self.write(
            "keys.cpp", keys_cpp([("upd-2026-a", bytes.fromhex(PUBLIC_KEY))]).encode("utf-8")
        )
        serial = json.loads((self.out / "serial.json").read_bytes())["serial"]
        steps = (
            ("payload", "--serial", serial, "--issued-at", "2026-12-01T03:40:00Z",
             "--stable", self.out / "stable", "--preview", self.out / "preview",
             "--out", self.root / "p.json"),
            ("sign", "--key", KEY, "--key-id", "upd-2026-a", "--payload", self.root / "p.json",
             "--out", self.root / "update-feed.json"),
            ("verify", "--keys-from", keys, self.root / "update-feed.json"),
        )
        for step in steps:
            done = self.run_tool(*step)
            self.assertEqual(done.returncode, 0, "%s: %s" % (step[0], done.stderr))
        self.assertEqual(done.stdout, "ok serial 2\n")

    def test_an_old_run_leaves_nothing_behind(self) -> None:
        self.collect([api_release("v0.1.0"), api_release("v0.2.0-rc.1")])
        (self.out / "stable" / "stale.txt").write_text("x")
        (self.out / "other.txt").write_text("kept")
        self.collect([api_release("v0.3.0-rc.1")])
        self.assertFalse((self.out / "stable").exists())
        self.assertEqual(self.tag_of("preview"), "v0.3.0-rc.1")
        self.assertEqual((self.out / "other.txt").read_text(), "kept")

    def test_a_tag_that_is_not_a_version_is_not_picked(self) -> None:
        entries = [api_release("nightly"), api_release("v0.1.0-dev", prerelease=True), api_release("v0.1.0")]
        self.collect(entries)
        self.assertEqual(self.tag_of("stable"), "v0.1.0")
        self.assertFalse((self.out / "preview").exists())

    def test_what_is_not_a_release_is_refused(self) -> None:
        with self.assertRaises(ps.Refusal):
            self.collect(["v0.1.0"])

    def test_an_address_of_another_place_is_refused(self) -> None:
        entry = api_release("v0.1.0")
        entry["assets"][3]["browser_download_url"] = "https://example.com/x.deb"
        with self.assertRaises(ps.Refusal) as caught:
            self.collect([entry])
        self.assertIn("does not start with", caught.exception.message)
        self.assertEqual(self.download.urls, [])

    def test_a_release_without_sha512sums_is_refused(self) -> None:
        names = [name for name in release_names("v0.1.0") if name != "SHA512SUMS"]
        with self.assertRaises(ps.Refusal) as caught:
            self.collect([api_release("v0.1.0", names=names)])
        self.assertIn("SHA512SUMS", caught.exception.message)

    def test_a_download_that_fails_is_refused(self) -> None:
        def broken(url: str, limit: int) -> bytes:
            raise ps.Refusal("could not download the file")

        with self.assertRaises(ps.Refusal), contextlib.redirect_stderr(io.StringIO()):
            uf.collect(REPO_NAME, [api_release("v0.1.0")], str(self.out), broken)

    # -- The summary

    def test_no_body_no_summary_file(self) -> None:
        for body in ("", "  \n\r\n\t ", None, 7, "\x00\x01"):
            with self.subTest(body=body):
                self.collect([api_release("v0.1.0", body=body)])
                self.assertFalse((self.out / "stable" / "summary.txt").exists())

    def test_the_body_as_plain_text(self) -> None:
        body = "## Heading\r\n\r\nA line.\tTabbed.\x00\x1b\x7f\x85 End.\r\n\r\n"
        self.collect([api_release("v0.1.0", body=body)])
        text = (self.out / "stable" / "summary.txt").read_bytes()
        self.assertEqual(text, b"Heading\n\nA line. Tabbed. End.\n")
        self.assertIsNone(uf.summary_problem(text.decode("utf-8").rstrip("\n")))

    def test_the_marks_of_markdown_are_taken_out(self) -> None:
        body = (
            "## What is in it\n\n"
            "It replaces [rc.4](https://example.com/releases/tag/v0.1.0-rc.4), "
            "and [the notes](https://example.com/a_(b)) say more.\n\n"
            "**Read this first.** Remove the key with `reg delete \"HKCU\\Old\" /f`.\n\n"
            "### Fixes\n"
            "- A number # in a line stays, and so does a [bracket] with no address.\n"
            "#not a heading\n"
        )
        self.assertEqual(
            uf.summary_of(body),
            "What is in it\n\n"
            "It replaces rc.4, and the notes) say more.\n\n"
            "Read this first. Remove the key with reg delete \"HKCU\\Old\" /f.\n\n"
            "Fixes\n"
            "- A number # in a line stays, and so does a [bracket] with no address.\n"
            "#not a heading",
        )

    def test_a_long_body_is_cut_at_the_end_of_a_line_near_the_limit(self) -> None:
        line = "A line of the notes that is fifty bytes long, yes.\n"
        text = uf.summary_of(line * 100)
        assert text is not None
        self.assertLessEqual(len(text.encode("utf-8")), uf.MAX_SUMMARY_BYTES)
        self.assertTrue(text.endswith("yes.\n..."))
        self.assertIsNone(uf.summary_problem(text))

    def test_a_body_over_the_limit_is_cut_at_a_character(self) -> None:
        for unit in ("a", "é", "€", "\U0001F600"):
            with self.subTest(unit=unit.encode("unicode_escape")):
                text = uf.summary_of("Start\n" + unit * 3000)
                assert text is not None
                self.assertLessEqual(len(text.encode("utf-8")), uf.MAX_SUMMARY_BYTES)
                self.assertGreater(len(text.encode("utf-8")), uf.MAX_SUMMARY_BYTES - 4)
                self.assertTrue(text.endswith("..."))
                self.assertTrue(text.startswith("Start\n"))
                self.assertIsNone(uf.summary_problem(text))

    def test_a_body_at_the_limit_is_not_cut(self) -> None:
        body = "x" * uf.MAX_SUMMARY_BYTES
        self.assertEqual(uf.summary_of(body), body)
        self.assertEqual(uf.summary_of(body + "y"), "x" * (uf.MAX_SUMMARY_BYTES - 3) + "...")

    def test_a_lone_surrogate_is_dropped(self) -> None:
        self.assertEqual(uf.summary_of("a\ud800b"), "ab")

    # -- The command

    def releases_file(self, entries: Any) -> Path:
        return self.write("releases.json", json.dumps(entries).encode("utf-8"))

    def run_main(self, *arguments: Any) -> "Tuple[int, str, str]":
        out, err = io.StringIO(), io.StringIO()
        with unittest.mock.patch.object(uf, "download_bytes", self.download), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = uf.main([str(a) for a in arguments])
        return code, out.getvalue(), err.getvalue()

    def test_the_command_prints_the_serial_and_nothing_else(self) -> None:
        releases = self.releases_file([api_release("v0.1.0"), api_release("v0.2.0-rc.1")])
        code, out, err = self.run_main(
            "collect", "--repo", REPO_NAME, "--releases-json", releases, "--out", self.out
        )
        self.assertEqual((code, out), (0, "2\n"))
        self.assertIn("v0.1.0", err)
        self.assertEqual(self.tag_of("stable"), "v0.1.0")

    def test_the_command_makes_the_folder_it_is_given(self) -> None:
        releases = self.releases_file([api_release("v0.1.0")])
        out = self.root / "a" / "b"
        code, _, _ = self.run_main("collect", "--repo", REPO_NAME, "--releases-json", releases, "--out", out)
        self.assertEqual(code, 0)
        self.assertTrue((out / "stable" / "release.json").is_file())

    def test_the_command_refuses_with_one_sentence(self) -> None:
        cases = (
            ("a bad name", ("--repo", "nobody", "--releases-json", "x"), 2),
            ("a name with a slash", ("--repo", "a/b/c", "--releases-json", "x"), 2),
            ("no file", ("--repo", REPO_NAME, "--releases-json", str(self.root / "none.json")), 2),
            ("not a list", ("--repo", REPO_NAME, "--releases-json", str(self.releases_file({"a": 1}))), 1),
            ("not json", ("--repo", REPO_NAME, "--releases-json", str(self.write("x.json", b"{"))), 1),
            ("no release", ("--repo", REPO_NAME, "--releases-json", str(self.releases_file([]))), 1),
        )
        for label, arguments, status in cases:
            with self.subTest(label):
                code, out, err = self.run_main("collect", *arguments, "--out", self.out)
                self.assertEqual((code, out), (status, ""))
                self.assertEqual(len(err.strip().splitlines()), 1, err)
                self.assertTrue(err.startswith("update_feed: "))

    def test_the_options_are_needed(self) -> None:
        for arguments in (("collect",), ("collect", "--repo", REPO_NAME), ("collect", "--out", "x")):
            self.assertEqual(self.run_tool(*arguments).returncode, 2, arguments)


class FakeResponse(io.BytesIO):
    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


class NetworkTests(ToolTestCase):
    """The two requests of ``collect``, with ``urlopen`` replaced: nothing here
    opens a socket."""

    def fetch(self, pages: List[Any], environment: Dict[str, str]) -> Tuple[List[Any], List[Any]]:
        seen: List[Any] = []

        def fake(request: Any, timeout: float) -> FakeResponse:
            seen.append((request, timeout))
            number = int(request.full_url.rsplit("page=", 1)[1])
            page = pages[number - 1]
            if isinstance(page, Exception):
                raise page
            return FakeResponse(json.dumps(page).encode("utf-8"))

        with unittest.mock.patch.dict(os.environ, environment), \
                unittest.mock.patch("urllib.request.urlopen", fake):
            found = uf.fetch_releases(REPO_NAME)
        return found, seen

    def test_every_page_is_read(self) -> None:
        page_one = [{"tag_name": "v1.%d.0" % i} for i in range(100)]
        found, seen = self.fetch([page_one, [{"tag_name": "v0.1.0"}]], {"GH_TOKEN": "", "GITHUB_TOKEN": ""})
        self.assertEqual(len(found), 101)
        self.assertEqual(
            [request.full_url for request, _ in seen],
            [
                "https://api.github.com/repos/%s/releases?per_page=100&page=%d" % (REPO_NAME, n)
                for n in (1, 2)
            ],
        )
        self.assertEqual({timeout for _, timeout in seen}, {30})

    def test_one_short_page_is_one_request(self) -> None:
        found, seen = self.fetch([[{"tag_name": "v1.0.0"}]], {"GH_TOKEN": "", "GITHUB_TOKEN": ""})
        self.assertEqual((len(found), len(seen)), (1, 1))

    def test_the_token_goes_in_a_header_and_nowhere_else(self) -> None:
        for variables, expected in (
            ({"GH_TOKEN": "s3cret", "GITHUB_TOKEN": ""}, "Bearer s3cret"),
            ({"GH_TOKEN": "", "GITHUB_TOKEN": "t0ken"}, "Bearer t0ken"),
            ({"GH_TOKEN": "", "GITHUB_TOKEN": ""}, None),
        ):
            with self.subTest(variables=variables):
                _, seen = self.fetch([[]], variables)
                headers = {k.lower(): v for k, v in seen[0][0].header_items()}
                self.assertEqual(headers.get("authorization"), expected)
                self.assertEqual(headers["accept"], "application/vnd.github+json")
                self.assertEqual(headers["user-agent"], "virtual-serial-studio-update-feed")
                self.assertTrue(seen[0][0].full_url.startswith("https://api.github.com/"))
                for secret in ("s3cret", "t0ken"):
                    self.assertNotIn(secret, seen[0][0].full_url)

    def test_a_failure_is_one_sentence_without_the_token(self) -> None:
        error = urllib.error.URLError("no route to host")
        with self.assertRaises(ps.Refusal) as caught:
            self.fetch([error], {"GH_TOKEN": "s3cret", "GITHUB_TOKEN": ""})
        self.assertIn("no route to host", caught.exception.message)
        self.assertNotIn("s3cret", caught.exception.message)
        self.assertNotIn("\n", caught.exception.message)

    def test_an_answer_that_is_not_a_list_is_refused(self) -> None:
        with self.assertRaises(ps.Refusal):
            self.fetch([{"message": "Not Found"}], {"GH_TOKEN": "", "GITHUB_TOKEN": ""})

    def test_an_answer_that_is_not_json_is_refused(self) -> None:
        def fake(request: Any, timeout: float) -> FakeResponse:
            return FakeResponse(b"<html>")

        with unittest.mock.patch("urllib.request.urlopen", fake), self.assertRaises(ps.Refusal):
            uf.fetch_releases(REPO_NAME)

    def test_a_release_file_is_downloaded_without_the_token(self) -> None:
        seen: List[Any] = []

        def fake(request: Any, timeout: float) -> FakeResponse:
            seen.append(request)
            return FakeResponse(b"the sums")

        url = "%s/releases/download/v0.1.0/SHA512SUMS" % REPO
        with unittest.mock.patch.dict(os.environ, {"GH_TOKEN": "s3cret", "GITHUB_TOKEN": "s3cret"}), \
                unittest.mock.patch("urllib.request.urlopen", fake):
            self.assertEqual(uf.download_bytes(url, 100), b"the sums")
        self.assertEqual(seen[0].full_url, url)
        self.assertNotIn("authorization", {k.lower() for k, _ in seen[0].header_items()})

    def test_a_download_is_limited_and_https(self) -> None:
        def fake(request: Any, timeout: float) -> FakeResponse:
            return FakeResponse(b"x" * 11)

        with unittest.mock.patch("urllib.request.urlopen", fake):
            self.assertEqual(uf.download_bytes(REPO + "/a", 11), b"x" * 11)
            with self.assertRaises(ps.Refusal):
                uf.download_bytes(REPO + "/a", 10)
        with self.assertRaises(ps.Refusal):
            uf.download_bytes("http://github.com/a", 10)

    def test_a_download_that_fails_is_one_sentence(self) -> None:
        def fake(request: Any, timeout: float) -> FakeResponse:
            raise urllib.error.URLError("timed out")

        with unittest.mock.patch("urllib.request.urlopen", fake), self.assertRaises(ps.Refusal) as caught:
            uf.download_bytes(REPO + "/a", 10)
        self.assertIn("timed out", caught.exception.message)


class UsageTests(ToolTestCase):
    def test_no_command(self) -> None:
        done = self.run_tool()
        self.assertEqual(done.returncode, 2)

    def test_the_help_of_every_command(self) -> None:
        for command in ("collect", "payload", "sign", "verify", "keys"):
            done = self.run_tool(command, "--help")
            self.assertEqual(done.returncode, 0, command)
            self.assertIn("usage:", done.stdout)

    def test_missing_options(self) -> None:
        for arguments in (
            ("payload", "--serial", "1"),
            ("sign", "--key", "k"),
            ("verify",),
            ("verify", "--public-key", "ab" * 32),
            ("collect",),
            ("keys",),
        ):
            self.assertEqual(self.run_tool(*arguments).returncode, 2, arguments)


if __name__ == "__main__":
    unittest.main()
