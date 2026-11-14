#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/plugin_sign.py.

Run with:

    python -m unittest discover -s tools -p "test_plugin_sign.py" -v

The tests that sign or verify call OpenSSL 3 and are skipped, with a message,
when it is not on the PATH. The rest is plain Python and always runs. The key
is a THROWAWAY TEST KEY: the one whose seed is the byte 0x02 repeated (the
plugins key of the C++ tests). Its private half is written down below for
anyone to read, and it must never sign anything real. The test writes it to
a temporary key file when it starts, so that no key file is in the repository.
"""

from __future__ import annotations

import atexit
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List
from unittest import mock

HERE = Path(__file__).resolve().parent
TOOL = HERE / "plugin_sign.py"
_KEY_DIR = Path(tempfile.mkdtemp(prefix="plugin-sign-key-"))
atexit.register(shutil.rmtree, _KEY_DIR, True)
KEY = _KEY_DIR / "test-key-02.pem"

sys.path.insert(0, str(HERE))

import plugin_sign as ps  # noqa: E402

# The public key of the seed 0x02 repeated 32 times. It came from the command
# of docs/plugins.md, section 8.9, run once on the fixture with OpenSSL 3.2.1:
#     openssl pkey -in test-key-02.pem -pubout -outform DER | tail -c 32 | xxd -p -c 32
# and was checked with a separate RFC 8032 implementation.
PUBLIC_KEY = "8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394"

# The base64 body of the throwaway key file: the PKCS#8 form of the seed 0x02
# repeated 32 times. It must never reach standard output or standard error.
KEY_BODY = "MC4CAQAwBQYDK2VwBCIEIAICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
KEY.write_text(
    "-----BEGIN PRIVATE KEY-----\n" + KEY_BODY + "\n-----END PRIVATE KEY-----\n",
    encoding="ascii",
)

# The vector for a C++ test: purpose plugin, key id plugins-2026, the fixture
# key, plugin id com.example.plugin, version 1.2.0 and a plugin.json of these
# 30 bytes.
VECTOR_MANIFEST = b"the exact bytes of plugin.json"
VECTOR_ENVELOPE = (
    '{"format":1,"purpose":"plugin","key_id":"plugins-2026","payload":'
    '"eyJwbHVnaW5faWQiOiJjb20uZXhhbXBsZS5wbHVnaW4iLCJ2ZXJzaW9uIjoiMS4yLjAiLCJ'
    "tYW5pZmVzdF9zaGE1MTIiOiJjN2RjMzFhNWU4MDUxNGUxNWQyMjljNTk5MTUxNGIxYjFmNjN"
    "iYzIwODk2NmQ4YTQxMTAwYTUwZWUyYzkxMWU0MGIzYjYyYzBlZmFmYTU1MDUwNTVlNmU2Mj"
    "NkY2QzNzU0NDM3NzdkN2FjNzEzOGFiNzYzMTNiNTczZWExNGNjOSJ9\","
    '"signature":"nYUUXb6INV8HqenR8Cga7kIe2_5gf771OxWCG3okcgVFMDbAyx0FhGbFiXEc'
    'MF9J6ecFAfwCN6dNzgMCw234AQ"}\n'
)


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


def key_entry(
    key_id: str = "plugins-2026",
    purpose: str = "plugin",
    public_key: str = PUBLIC_KEY,
    valid_from: str = "2026-11-01",
    valid_to: str = "2028-11-01",
) -> Dict[str, Any]:
    return {
        "key_id": key_id,
        "public_key": public_key,
        "purpose": purpose,
        "name": "Virtual Serial Studio plugins",
        "valid_from": valid_from,
        "valid_to": valid_to,
    }


def key_list_document(**changes: Any) -> Dict[str, Any]:
    document: Dict[str, Any] = {
        "key_list_version": 1,
        "serial": 4,
        "issued": "2026-11-13",
        "keys": [
            key_entry(),
            key_entry("revocations-2026", "plugin-revocations"),
        ],
        "revoked": [],
    }
    document.update(changes)
    return document


def revocations_document(**changes: Any) -> Dict[str, Any]:
    document: Dict[str, Any] = {
        "revocations_version": 1,
        "serial": 2,
        "issued_at": "2026-11-13",
        "key_ids": ["old-key"],
        "plugin_hashes": ["ab" * 64],
        "plugins": [{"id": "com.example.plugin", "max_version": "1.2.0"}],
    }
    document.update(changes)
    return document


def as_bytes(document: Any) -> bytes:
    return json.dumps(document).encode("utf-8")


# -- Plain Python: no OpenSSL ------------------------------------------------


class Base64UrlTests(unittest.TestCase):
    def test_vectors_of_rfc_4648(self) -> None:
        for data, text in (
            (b"", ""),
            (b"f", "Zg"),
            (b"fo", "Zm8"),
            (b"foo", "Zm9v"),
            (b"foob", "Zm9vYg"),
            (b"fooba", "Zm9vYmE"),
            (b"foobar", "Zm9vYmFy"),
        ):
            self.assertEqual(ps.base64url_encode(data), text)
            self.assertEqual(ps.base64url_decode(text), data)

    def test_url_alphabet_and_no_padding(self) -> None:
        self.assertEqual(ps.base64url_encode(b"\xfb\xff"), "-_8")
        self.assertEqual(ps.base64url_encode(b"\xff" * 64), "_" * 85 + "w")

    def test_decode_refuses_what_the_reader_refuses(self) -> None:
        for text in ("Zg==", "Zg ", "Zm9v\n", "+/8", "A", "Zh", "Zm9vYh"):
            with self.assertRaises(ValueError, msg=text):
                ps.base64url_decode(text)


class MessageTests(unittest.TestCase):
    def test_signed_message(self) -> None:
        self.assertEqual(
            ps.signed_message("plugin", b"abc"),
            b"VSS-SIGNED-FILE-1\x00plugin\x00abc",
        )

    def test_the_purpose_is_inside_the_message(self) -> None:
        self.assertNotEqual(
            ps.signed_message("plugin", b"x"),
            ps.signed_message("key-list", b"x"),
        )

    def test_the_payload_may_hold_a_zero_byte(self) -> None:
        self.assertEqual(
            ps.signed_message("key-list", b"\x00\xff"),
            b"VSS-SIGNED-FILE-1\x00key-list\x00\x00\xff",
        )


class EnvelopeTests(unittest.TestCase):
    def test_text_for_fixed_inputs(self) -> None:
        # signed_file::write: the five members in this order, no spaces and
        # one line feed at the end.
        expected = (
            '{"format":1,"purpose":"plugin","key_id":"plugins-2026",'
            '"payload":"YWJj","signature":"' + "_" * 85 + 'w"}\n'
        )
        self.assertEqual(
            ps.envelope_text("plugin", "plugins-2026", b"abc", b"\xff" * 64),
            expected,
        )

    def test_the_text_is_read_back(self) -> None:
        text = ps.envelope_text("key-list", "root-1", b"{}\x00", bytes(range(64)))
        read = ps.parse_envelope(text.encode("ascii"))
        self.assertEqual(
            read, ps.Envelope("key-list", "root-1", b"{}\x00", bytes(range(64)))
        )

    def test_the_reader_refuses_a_bad_file(self) -> None:
        good = ps.envelope_text("plugin", "k", b"x", bytes(64))
        refused = [
            b"",
            b"[]",
            b"not json",
            good.replace('"format":1', '"format":2').encode(),
            good.replace('"format":1,', "").encode(),
            good.replace("\n", ',"extra":1}\n').replace("}", "", 1).encode(),
            good.replace('"purpose":"plugin"', '"purpose":"Plugin"').encode(),
            good.replace('"key_id":"k"', '"key_id":"K"').encode(),
            good.replace('"payload":"eA"', '"payload":"eA=="').encode(),
            good.replace('"format":1,', '"format":1,"format":1,').encode(),
            (good + " " * ps.MAX_ENVELOPE_SIZE).encode(),
        ]
        for data in refused:
            with self.assertRaises(ValueError, msg=data[:60]):
                ps.parse_envelope(data)


class ManifestPayloadTests(unittest.TestCase):
    def test_compact_json_in_this_order(self) -> None:
        digest = hashlib.sha512(VECTOR_MANIFEST).hexdigest()
        self.assertEqual(
            ps.manifest_payload("com.example.plugin", "1.2.0", VECTOR_MANIFEST),
            (
                '{"plugin_id":"com.example.plugin","version":"1.2.0",'
                '"manifest_sha512":"' + digest + '"}'
            ).encode("ascii"),
        )

    def test_one_changed_byte_changes_the_digest(self) -> None:
        first = ps.manifest_payload("a.b", "1.0.0", b"abc")
        second = ps.manifest_payload("a.b", "1.0.0", b"abd")
        self.assertNotEqual(first, second)

    def test_plugin_ids_and_versions(self) -> None:
        for good in ("com.example.plugin", "a.b", "com.example.my-plugin"):
            self.assertTrue(ps.is_plugin_id(good), good)
        for bad in ("", "ab", "plugin", "Com.example", "a..b", "a.b-", ".a.b"):
            self.assertFalse(ps.is_plugin_id(bad), bad)
        for good in ("0.0.0", "1.2.0", "10.20.30-beta.1", "1.0.0-rc-1"):
            self.assertTrue(ps.is_plugin_version(good), good)
        for bad in ("1.2", "1.2.3.4", "01.2.3", "1.2.3-", "1.2.3-a..b", "v1.2.3",
                    "1.2.3+build", "1.2.x", "9999999.0.0"):
            self.assertFalse(ps.is_plugin_version(bad), bad)


class KeyIdTests(unittest.TestCase):
    def test_good_ids(self) -> None:
        for good in ("plugins-2026", "a", "root-1", "x" * 64, "0-9"):
            ps.check_key_id(good)

    def test_bad_ids_are_refused_as_a_usage_error(self) -> None:
        for bad in ("", "has space", "a/b", "x" * 65, "café", "a\nb", "-x;y"):
            with self.assertRaises(ps.Refusal, msg=bad) as caught:
                ps.check_key_id(bad)
            self.assertEqual(caught.exception.code, ps.EXIT_USAGE)

    def test_an_id_the_envelope_reader_refuses_is_refused(self) -> None:
        # signed_file::parse takes lower-case letters, digits and hyphen only.
        for bad in ("Plugins", "a.b", "a_b"):
            with self.assertRaises(ps.Refusal, msg=bad) as caught:
                ps.check_key_id(bad)
            self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
            self.assertIn("lower-case", caught.exception.message)


class KeyListChecks(unittest.TestCase):
    def problems(self, **changes: Any) -> List[str]:
        return ps.validate_key_list(as_bytes(key_list_document(**changes)))

    def assertRefused(self, text: str, problems: List[str]) -> None:
        self.assertTrue(problems, "the payload was accepted")
        self.assertTrue(any(text in line for line in problems), problems)

    def test_a_good_list(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_list_without_keys_and_revoked(self) -> None:
        document = key_list_document()
        del document["keys"], document["revoked"]
        self.assertEqual(ps.validate_key_list(as_bytes(document)), [])

    def test_the_licence_purpose_is_refused(self) -> None:
        self.assertRefused(
            "purpose", self.problems(keys=[key_entry(purpose="licence")])
        )

    def test_an_upper_case_public_key_is_refused(self) -> None:
        self.assertRefused(
            "public_key",
            self.problems(keys=[key_entry(public_key=PUBLIC_KEY.upper())]),
        )

    def test_a_short_public_key_is_refused(self) -> None:
        self.assertRefused(
            "public_key", self.problems(keys=[key_entry(public_key="ab")])
        )

    def test_a_repeated_key_id_is_refused(self) -> None:
        self.assertRefused(
            "twice",
            self.problems(
                keys=[key_entry(), key_entry(purpose="plugin-revocations")]
            ),
        )

    def test_valid_to_before_valid_from_is_refused(self) -> None:
        self.assertRefused(
            "valid_to",
            self.problems(
                keys=[key_entry(valid_from="2028-11-01", valid_to="2026-11-01")]
            ),
        )

    def test_the_same_day_is_allowed(self) -> None:
        self.assertEqual(
            self.problems(
                keys=[key_entry(valid_from="2027-01-01", valid_to="2027-01-01")]
            ),
            [],
        )

    def test_the_version_the_serial_and_the_dates(self) -> None:
        self.assertRefused("key_list_version", self.problems(key_list_version=2))
        self.assertRefused("key_list_version", self.problems(key_list_version=True))
        for serial in (0, -1, 1.5, "4", True, 1 << 53):
            self.assertRefused("serial", self.problems(serial=serial))
        self.assertEqual(self.problems(serial=1), [])
        self.assertEqual(self.problems(serial=(1 << 53) - 1), [])
        for issued in ("2026-13-01", "2026-02-30", "26-11-13", "2026-11-13 ", 5):
            self.assertRefused("issued", self.problems(issued=issued))

    def test_unknown_and_missing_members(self) -> None:
        self.assertRefused("unknown key", self.problems(extra=1))
        document = key_list_document()
        del document["serial"]
        self.assertRefused("serial", ps.validate_key_list(as_bytes(document)))
        bad = key_entry()
        bad["colour"] = "red"
        self.assertRefused("unknown key", self.problems(keys=[bad]))
        short = key_entry()
        del short["name"]
        self.assertRefused("name", self.problems(keys=[short]))

    def test_names_and_key_ids(self) -> None:
        for name in ("", "x" * 101, "bell\x07", "a‮b", "﻿name"):
            entry = key_entry()
            entry["name"] = name
            self.assertRefused("name", self.problems(keys=[entry]))
        entry = key_entry()
        entry["name"] = "Ü" * 50
        self.assertEqual(self.problems(keys=[entry]), [])
        entry["name"] = "Ü" * 51
        self.assertRefused("name", self.problems(keys=[entry]))
        self.assertRefused("key_id", self.problems(keys=[key_entry("bad id")]))
        self.assertRefused("revoked", self.problems(revoked=["bad id"]))
        self.assertRefused("revoked", self.problems(revoked="old"))
        self.assertEqual(self.problems(revoked=["old-key-1"]), [])
        # Only what a signed file can name: no upper case, dot or underscore.
        self.assertRefused("revoked", self.problems(revoked=["Old.Key_1"]))

    def test_limits(self) -> None:
        many = [key_entry("key-%d" % n) for n in range(65)]
        self.assertRefused("keys", self.problems(keys=many))
        self.assertEqual(self.problems(keys=many[:64]), [])
        self.assertRefused(
            "revoked", self.problems(revoked=["k%d" % n for n in range(257)])
        )

    def test_text_that_is_not_json(self) -> None:
        for data in (b"", b"{", b"[]", b"\xff\xfe", b"{} {}", b"\xef\xbb\xbf{}",
                     b'{"serial": NaN}',
                     b'{"serial": 1, "serial": 2}'):
            self.assertTrue(ps.validate_key_list(data), data)

    def test_the_text_of_a_key_is_never_echoed(self) -> None:
        problems = ps.validate_key_list(KEY.read_bytes())
        self.assertTrue(problems)
        self.assertNotIn(KEY_BODY, "\n".join(problems))


class RevocationChecks(unittest.TestCase):
    def problems(self, **changes: Any) -> List[str]:
        return ps.validate_revocations(as_bytes(revocations_document(**changes)))

    def assertRefused(self, text: str, problems: List[str]) -> None:
        self.assertTrue(problems, "the payload was accepted")
        self.assertTrue(any(text in line for line in problems), problems)

    def test_a_good_list(self) -> None:
        self.assertEqual(self.problems(), [])
        self.assertEqual(
            self.problems(key_ids=[], plugin_hashes=[], plugins=[]), []
        )

    def test_a_serial_of_zero_is_refused(self) -> None:
        self.assertRefused("serial", self.problems(serial=0))

    def test_other_members(self) -> None:
        self.assertRefused("revocations_version", self.problems(revocations_version=2))
        self.assertRefused("issued_at", self.problems(issued_at="yesterday"))
        self.assertRefused("unknown key", self.problems(extra=[]))
        self.assertRefused("key_ids", self.problems(key_ids=["bad id"]))
        self.assertRefused("key_ids", self.problems(key_ids="old-key"))
        self.assertRefused("plugin_hashes", self.problems(plugin_hashes=["AB" * 64]))
        self.assertRefused("plugin_hashes", self.problems(plugin_hashes=["ab" * 63]))
        self.assertRefused(
            "plugins[0].id", self.problems(plugins=[{"id": "nodot", "max_version": "1.0.0"}])
        )
        self.assertRefused(
            "max_version", self.problems(plugins=[{"id": "a.b", "max_version": "1.0"}])
        )
        self.assertRefused("unknown key", self.problems(plugins=[{"id": "a.b", "max_version": "1.0.0", "x": 1}]))
        self.assertRefused("max_version", self.problems(plugins=[{"id": "a.b"}]))
        self.assertRefused(
            "key_ids", self.problems(key_ids=["k"] * (ps.MAX_REVOKED_ENTRIES + 1))
        )

    def test_issued_at_is_required_here(self) -> None:
        document = revocations_document()
        del document["issued_at"]
        self.assertRefused("issued_at", ps.validate_revocations(as_bytes(document)))


class FindOpenSslTests(unittest.TestCase):
    def test_a_missing_program_is_a_usage_error(self) -> None:
        with mock.patch.object(ps.shutil, "which", return_value=None):
            with self.assertRaises(ps.Refusal) as caught:
                ps.find_openssl()
        self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
        self.assertIn("OpenSSL was not found", caught.exception.message)

    def test_a_version_before_3_is_refused(self) -> None:
        for line in (b"OpenSSL 1.1.1w  11 Sep 2023\n", b"LibreSSL 3.8.2\n", b""):
            done = subprocess.CompletedProcess([], 0, stdout=line, stderr=b"")
            with mock.patch.object(ps.shutil, "which", return_value="openssl"):
                with mock.patch.object(ps.subprocess, "run", return_value=done):
                    with self.assertRaises(ps.Refusal, msg=line) as caught:
                        ps.find_openssl()
            self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
            self.assertIn("OpenSSL 3.0", caught.exception.message)

    def test_version_3_is_accepted(self) -> None:
        line = b"OpenSSL 3.0.0 7 Sep 2021 (Library: OpenSSL 3.0.0 7 Sep 2021)\n"
        done = subprocess.CompletedProcess([], 0, stdout=line, stderr=b"")
        with mock.patch.object(ps.shutil, "which", return_value="/x/openssl"):
            with mock.patch.object(ps.subprocess, "run", return_value=done):
                self.assertEqual(ps.find_openssl(), "/x/openssl")


# -- The command line --------------------------------------------------------


class ToolTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="plugin-sign-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def run_tool(self, *arguments: Any) -> subprocess.CompletedProcess:
        done = subprocess.run(
            [sys.executable, str(TOOL)] + [str(a) for a in arguments],
            capture_output=True,
            text=True,
            check=False,
        )
        # No command ever shows the key.
        for output in (done.stdout, done.stderr):
            self.assertNotIn(KEY_BODY, output)
            self.assertNotIn("PRIVATE KEY", output)
        return done

    def make_plugin(self, name: str = "plugin", **manifest: Any) -> Path:
        folder = self.root / name
        folder.mkdir()
        document = {
            "id": "com.example.plugin",
            "version": "1.2.0",
            "name": "Example",
        }
        document.update(manifest)
        (folder / "plugin.json").write_bytes(
            json.dumps(document, indent=2).encode("utf-8")
        )
        return folder

    def write(self, name: str, data: bytes) -> Path:
        path = self.root / name
        path.write_bytes(data)
        return path


class UsageTests(ToolTestCase):
    """Refusals that come before OpenSSL is needed."""

    def test_a_missing_key_file_is_a_usage_error_everywhere(self) -> None:
        missing = self.root / "no-such.key"
        folder = self.make_plugin()
        payload = self.write("payload.json", as_bytes(key_list_document()))
        for command in (
            ["public-key", "--key", missing],
            ["plugin", "--key", missing, "--key-id", "k", "--plugin", folder],
            ["key-list", "--key", missing, "--key-id", "k", "--payload",
             payload, "--out", self.root / "out.json"],
            ["revocations", "--key", missing, "--key-id", "k", "--payload",
             payload, "--out", self.root / "out.json"],
        ):
            done = self.run_tool(*command)
            self.assertEqual(done.returncode, 2, command[0])
            self.assertIn("was not found", done.stderr)
            self.assertEqual(done.stdout, "")
        self.assertFalse((self.root / "out.json").exists())
        self.assertFalse((folder / "plugin.sig").exists())

    def test_a_bad_key_id_is_a_usage_error(self) -> None:
        folder = self.make_plugin()
        for bad in ("", "has space", "a/b", "x" * 65, "Upper"):
            done = self.run_tool(
                "plugin", "--key", KEY, "--key-id", bad, "--plugin", folder
            )
            self.assertEqual(done.returncode, 2, repr(bad))
            self.assertIn("key id", done.stderr)
            self.assertEqual(done.stdout, "")
        self.assertFalse((folder / "plugin.sig").exists())

    def test_no_command_and_an_unknown_option(self) -> None:
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertEqual(self.run_tool("public-key").returncode, 2)
        self.assertEqual(
            self.run_tool("public-key", "--key", KEY, "--nonsense").returncode, 2
        )
        self.assertEqual(
            self.run_tool("verify", "--public-key", PUBLIC_KEY, "--purpose",
                          "licence", self.root).returncode,
            2,
        )

    def test_a_missing_plugin_folder_or_manifest(self) -> None:
        done = self.run_tool(
            "plugin", "--key", KEY, "--key-id", "k", "--plugin", self.root / "x"
        )
        self.assertEqual(done.returncode, 2)
        empty = self.root / "empty"
        empty.mkdir()
        done = self.run_tool("plugin", "--key", KEY, "--key-id", "k", "--plugin", empty)
        self.assertEqual(done.returncode, 2)
        self.assertIn("plugin.json", done.stderr)

    def test_openssl_that_is_not_there_is_a_usage_error(self) -> None:
        done = self.run_tool(
            "public-key", "--key", KEY, "--openssl", self.root / "no-openssl"
        )
        self.assertEqual(done.returncode, 2)
        self.assertIn("OpenSSL was not found", done.stderr)

    def test_refusals_of_a_payload_need_no_openssl(self) -> None:
        payload = self.write(
            "payload.json", as_bytes(key_list_document(serial=0))
        )
        out = self.root / "out.json"
        done = self.run_tool(
            "key-list", "--key", KEY, "--key-id", "root-1", "--payload",
            payload, "--out", out, "--openssl", self.root / "no-openssl",
        )
        self.assertEqual(done.returncode, 1)
        self.assertIn("serial", done.stderr)
        self.assertFalse(out.exists())

    def test_a_manifest_that_is_not_a_plugin_manifest(self) -> None:
        for name, data, text in (
            ("a", b"not json", "plugin.json"),
            ("b", b"[]", "object"),
            ("c", b'{"version": "1.0.0"}', "plugin id"),
            ("d", b'{"id": "com.example.x"}', "version"),
            ("e", b'{"id": "com.example.x", "version": "1.0"}', "version"),
            ("f", b'{"id": "Bad", "version": "1.0.0"}', "plugin id"),
            ("g", b'{"id": "a.b", "id": "a.c", "version": "1.0.0"}', "twice"),
        ):
            folder = self.root / name
            folder.mkdir()
            (folder / "plugin.json").write_bytes(data)
            done = self.run_tool(
                "plugin", "--key", KEY, "--key-id", "k", "--plugin", folder,
                "--openssl", self.root / "no-openssl",
            )
            self.assertEqual(done.returncode, 1, name)
            self.assertIn(text, done.stderr)
            self.assertFalse((folder / "plugin.sig").exists())

    def test_the_output_is_never_the_key_or_the_payload(self) -> None:
        # The key is a copy: the fixture itself is never at risk.
        key = self.write("copy.pem", KEY.read_bytes())
        payload = self.write("payload.json", as_bytes(key_list_document()))
        for target in (payload, key):
            done = self.run_tool(
                "key-list", "--key", key, "--key-id", "root-1", "--payload",
                payload, "--out", target,
                "--openssl", self.root / "no-openssl",
            )
            self.assertEqual(done.returncode, 2)
            self.assertIn("is an input file", done.stderr)
        self.assertEqual(payload.read_bytes(), as_bytes(key_list_document()))
        self.assertEqual(key.read_bytes(), KEY.read_bytes())
        folder = self.make_plugin()
        done = self.run_tool(
            "plugin", "--key", key, "--key-id", "k", "--plugin", folder,
            "--out", folder / "plugin.json",
            "--openssl", self.root / "no-openssl",
        )
        self.assertEqual(done.returncode, 2)


@needs_openssl
class SigningTests(ToolTestCase):
    def public_key_file(self) -> Path:
        path = self.root / "public.pem"
        subprocess.run(
            [ps.find_openssl(), "pkey", "-in", str(KEY), "-pubout", "-out",
             str(path)],
            check=True,
            capture_output=True,
        )
        return path

    def sign_plugin(self, folder: Path, out: Any = None) -> subprocess.CompletedProcess:
        arguments: List[Any] = [
            "plugin", "--key", KEY, "--key-id", "plugins-2026", "--plugin", folder,
        ]
        if out is not None:
            arguments += ["--out", out]
        return self.run_tool(*arguments)

    def verify(self, path: Path, purpose: str = "plugin", key: Any = PUBLIC_KEY):
        return self.run_tool(
            "verify", "--public-key", key, "--purpose", purpose, path
        )

    # -- public-key

    def test_public_key_of_the_fixture(self) -> None:
        done = self.run_tool("public-key", "--key", KEY)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, PUBLIC_KEY + "\n")
        self.assertEqual(done.stderr, "")

    def test_public_key_of_a_public_key_file(self) -> None:
        done = self.run_tool("public-key", "--key", self.public_key_file())
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, PUBLIC_KEY + "\n")

    def test_public_key_of_a_file_that_is_no_key(self) -> None:
        junk = self.write("junk.pem", b"not a key\n")
        done = self.run_tool("public-key", "--key", junk)
        self.assertEqual(done.returncode, 1)
        self.assertIn("could not read an Ed25519 key", done.stderr)
        self.assertEqual(done.stdout, "")

    # -- plugin

    def test_the_vector_for_a_cpp_test(self) -> None:
        payload = ps.manifest_payload(
            "com.example.plugin", "1.2.0", VECTOR_MANIFEST
        )
        text = ps.sign_envelope(
            ps.find_openssl(), str(KEY), "plugin", "plugins-2026", payload
        )
        self.assertEqual(text, VECTOR_ENVELOPE)

    def test_plugin_writes_plugin_sig_and_verify_accepts_it(self) -> None:
        folder = self.make_plugin()
        done = self.sign_plugin(folder)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stderr, "")
        sig = folder / "plugin.sig"
        self.assertTrue(sig.is_file())
        self.assertIn("plugin.sig", done.stdout)

        data = sig.read_bytes()
        self.assertTrue(data.endswith(b"}\n"))
        self.assertNotIn(b"\r", data)
        found = ps.parse_envelope(data)
        self.assertEqual(found.purpose, "plugin")
        self.assertEqual(found.key_id, "plugins-2026")
        claim = json.loads(found.payload)
        self.assertEqual(list(claim), ["plugin_id", "version", "manifest_sha512"])
        self.assertEqual(claim["plugin_id"], "com.example.plugin")
        self.assertEqual(claim["version"], "1.2.0")
        self.assertEqual(
            claim["manifest_sha512"],
            hashlib.sha512((folder / "plugin.json").read_bytes()).hexdigest(),
        )

        ok = self.verify(sig)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual(ok.stdout, "ok\n")
        self.assertEqual(ok.stderr, "")
        # By a public key file as well as by the hex.
        self.assertEqual(self.verify(sig, key=self.public_key_file()).returncode, 0)
        self.assertEqual(self.verify(sig, key=PUBLIC_KEY.upper()).returncode, 0)

    def test_out_names_another_file(self) -> None:
        folder = self.make_plugin()
        out = self.root / "elsewhere.sig"
        done = self.sign_plugin(folder, out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(out.is_file())
        self.assertFalse((folder / "plugin.sig").exists())

    def test_one_changed_byte_gives_another_payload(self) -> None:
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        first = ps.parse_envelope((folder / "plugin.sig").read_bytes())

        manifest = folder / "plugin.json"
        data = bytearray(manifest.read_bytes())
        at = data.index(b"Example")
        data[at] ^= 0x01  # "Example" becomes "Dxample"
        manifest.write_bytes(bytes(data))
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        second = ps.parse_envelope((folder / "plugin.sig").read_bytes())

        self.assertNotEqual(first.payload, second.payload)
        self.assertNotEqual(first.signature, second.signature)
        self.assertEqual(
            json.loads(second.payload)["manifest_sha512"],
            hashlib.sha512(bytes(data)).hexdigest(),
        )
        self.assertEqual(self.verify(folder / "plugin.sig").returncode, 0)

    def test_the_same_input_gives_the_same_bytes(self) -> None:
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        first = (folder / "plugin.sig").read_bytes()
        (folder / "plugin.sig").unlink()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        self.assertEqual((folder / "plugin.sig").read_bytes(), first)

    # -- verify

    def test_verify_with_the_wrong_purpose_fails(self) -> None:
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        for purpose in ("key-list", "plugin-revocations"):
            done = self.verify(folder / "plugin.sig", purpose)
            self.assertEqual(done.returncode, 1, purpose)
            self.assertEqual(done.stdout, "")
            self.assertIn("signed as", done.stderr)

    def test_a_signature_for_another_purpose_does_not_pass(self) -> None:
        # The purpose is inside the signed message: the same signature in an
        # envelope that says another purpose does not hold.
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        found = ps.parse_envelope((folder / "plugin.sig").read_bytes())
        moved = self.write(
            "moved.sig",
            ps.envelope_text(
                "key-list", found.key_id, found.payload, found.signature
            ).encode("ascii"),
        )
        done = self.verify(moved, "key-list")
        self.assertEqual(done.returncode, 1)
        self.assertIn("does not hold", done.stderr)

    def test_a_changed_signature_or_payload_fails(self) -> None:
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        found = ps.parse_envelope((folder / "plugin.sig").read_bytes())

        flipped = bytearray(found.signature)
        flipped[10] ^= 0x04
        changed = self.write(
            "signature.sig",
            ps.envelope_text(
                "plugin", found.key_id, found.payload, bytes(flipped)
            ).encode("ascii"),
        )
        done = self.verify(changed)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertIn("does not hold", done.stderr)

        other = self.write(
            "payload.sig",
            ps.envelope_text(
                "plugin", found.key_id, found.payload + b" ", found.signature
            ).encode("ascii"),
        )
        self.assertEqual(self.verify(other).returncode, 1)

    def test_verify_with_another_public_key_fails(self) -> None:
        folder = self.make_plugin()
        self.assertEqual(self.sign_plugin(folder).returncode, 0)
        other = "00" * 31 + "01"
        done = self.verify(folder / "plugin.sig", key=other)
        self.assertEqual(done.returncode, 1)
        self.assertIn("does not hold", done.stderr)

    def test_verify_of_a_file_that_is_not_an_envelope(self) -> None:
        junk = self.write("junk.sig", b'{"format": 1}\n')
        done = self.verify(junk)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertNotEqual(done.stderr, "")
        self.assertEqual(self.verify(self.root / "missing.sig").returncode, 2)
        done = self.verify(junk, key=str(self.root / "no-key"))
        self.assertEqual(done.returncode, 2)

    # -- key-list and revocations

    def sign_payload(self, command: str, key_id: str, document: Any) -> Any:
        payload = self.write("payload.json", as_bytes(document))
        out = self.root / "signed.json"
        if out.exists():
            out.unlink()
        done = self.run_tool(
            command, "--key", KEY, "--key-id", key_id, "--payload", payload,
            "--out", out,
        )
        return done, out

    def test_key_list_refuses_what_studio_refuses(self) -> None:
        cases = (
            ("purpose", key_list_document(keys=[key_entry(purpose="licence")])),
            ("public_key", key_list_document(
                keys=[key_entry(public_key=PUBLIC_KEY.upper())])),
            ("twice", key_list_document(
                keys=[key_entry(), key_entry(purpose="plugin-revocations")])),
            ("valid_to", key_list_document(
                keys=[key_entry(valid_from="2028-01-01", valid_to="2027-01-01")])),
        )
        for text, document in cases:
            done, out = self.sign_payload("key-list", "root-2026", document)
            self.assertEqual(done.returncode, 1, text)
            self.assertIn(text, done.stderr)
            self.assertEqual(done.stdout, "")
            self.assertFalse(out.exists(), text)

    def test_key_list_signs_a_good_payload_as_it_is_on_disk(self) -> None:
        # Indented, with a trailing line feed: the bytes are signed as they
        # are, not written again.
        payload = (json.dumps(key_list_document(), indent=2) + "\n").encode()
        path = self.write("payload.json", payload)
        out = self.root / "official-keys.json"
        done = self.run_tool(
            "key-list", "--key", KEY, "--key-id", "root-2026", "--payload",
            path, "--out", out,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        found = ps.parse_envelope(out.read_bytes())
        self.assertEqual(found.purpose, "key-list")
        self.assertEqual(found.key_id, "root-2026")
        self.assertEqual(found.payload, payload)
        self.assertEqual(self.verify(out, "key-list").returncode, 0)
        self.assertEqual(self.verify(out, "plugin").returncode, 1)

    def test_revocations_refuses_a_serial_of_zero(self) -> None:
        done, out = self.sign_payload(
            "revocations", "revocations-2026", revocations_document(serial=0)
        )
        self.assertEqual(done.returncode, 1)
        self.assertIn("serial", done.stderr)
        self.assertFalse(out.exists())

    def test_revocations_signs_a_good_payload(self) -> None:
        document = revocations_document()
        done, out = self.sign_payload(
            "revocations", "revocations-2026", document
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        found = ps.parse_envelope(out.read_bytes())
        self.assertEqual(found.purpose, "plugin-revocations")
        self.assertEqual(found.payload, as_bytes(document))
        self.assertEqual(self.verify(out, "plugin-revocations").returncode, 0)

    def test_a_payload_that_is_too_large_for_the_envelope(self) -> None:
        document = revocations_document(
            key_ids=["k%063d" % n for n in range(1000)]
        )
        done, out = self.sign_payload("revocations", "revocations-2026", document)
        self.assertEqual(done.returncode, 1)
        self.assertIn("at most", done.stderr)
        self.assertFalse(out.exists())

    # -- no key material

    def test_no_command_prints_the_key(self) -> None:
        # run_tool checks every output for the key body; this runs the
        # commands that fail as well as those that succeed, with the key
        # file given where a payload or a public key is expected.
        folder = self.make_plugin()
        out = self.root / "out"
        for command in (
            ["public-key", "--key", KEY],
            ["plugin", "--key", KEY, "--key-id", "plugins-2026", "--plugin", folder],
            ["key-list", "--key", KEY, "--key-id", "r", "--payload", KEY, "--out", out],
            ["revocations", "--key", KEY, "--key-id", "r", "--payload", KEY,
             "--out", out],
            ["verify", "--public-key", KEY, "--purpose", "plugin", KEY],
            ["verify", "--public-key", KEY, "--purpose", "plugin",
             folder / "plugin.sig"],
            ["public-key", "--key", folder / "plugin.json"],
            ["plugin", "--key", folder / "plugin.json", "--key-id", "k",
             "--plugin", folder],
            ["plugin", "--key", KEY, "--key-id", "k", "--plugin", KEY],
        ):
            done = self.run_tool(*command)
            self.assertIn(done.returncode, (0, 1, 2), command)
        self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
