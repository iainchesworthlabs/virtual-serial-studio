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
import base64
import contextlib
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
from pathlib import Path
from typing import Any, Dict, List, Optional

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
        with unittest.mock.patch.object(ps.shutil, "which", return_value=None):
            with self.assertRaises(ps.Refusal) as caught:
                ps.find_openssl()
        self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
        self.assertIn("OpenSSL was not found", caught.exception.message)

    def test_a_version_before_3_is_refused(self) -> None:
        for line in (b"OpenSSL 1.1.1w  11 Sep 2023\n", b"LibreSSL 3.8.2\n", b""):
            done = subprocess.CompletedProcess([], 0, stdout=line, stderr=b"")
            with unittest.mock.patch.object(ps.shutil, "which", return_value="openssl"):
                with unittest.mock.patch.object(ps.subprocess, "run", return_value=done):
                    with self.assertRaises(ps.Refusal, msg=line) as caught:
                        ps.find_openssl()
            self.assertEqual(caught.exception.code, ps.EXIT_USAGE)
            self.assertIn("OpenSSL 3.0", caught.exception.message)

    def test_version_3_is_accepted(self) -> None:
        line = b"OpenSSL 3.0.0 7 Sep 2021 (Library: OpenSSL 3.0.0 7 Sep 2021)\n"
        done = subprocess.CompletedProcess([], 0, stdout=line, stderr=b"")
        with unittest.mock.patch.object(ps.shutil, "which", return_value="/x/openssl"):
            with unittest.mock.patch.object(ps.subprocess, "run", return_value=done):
                self.assertEqual(ps.find_openssl(), "/x/openssl")


# -- The command line --------------------------------------------------------


class ToolTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="plugin-sign-test-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def run_tool(
        self,
        *arguments: Any,
        env: Optional[Dict[str, str]] = None,
        timeout: float = 120,
        secrets: Any = (),
    ) -> subprocess.CompletedProcess:
        # Standard input is the null device, so that no test can wait for a
        # terminal; ``timeout`` ends one that waits for anything else.
        done = subprocess.run(
            [sys.executable, str(TOOL)] + [str(a) for a in arguments],
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
            env=env,
            timeout=timeout,
        )
        # No command ever shows the key, or a secret that the test names.
        for output in (done.stdout, done.stderr):
            self.assertNotIn(KEY_BODY, output)
            self.assertNotIn("PRIVATE KEY", output)
            for secret in secrets:
                self.assertNotIn(secret, output)
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


# -- keygen and passphrases ----------------------------------------------------

PASSPHRASE = "correct horse battery 7491"
WRONG_PASSPHRASE = "incorrect horse battery 1234"
ENCRYPTED_HEADER = b"-----BEGIN ENCRYPTED PRIVATE KEY-----"


def key_body(path: Path) -> str:
    """The base64 text between the BEGIN and END lines of a key file."""
    lines = path.read_text(encoding="ascii").strip().splitlines()
    return "".join(lines[1:-1])


@needs_openssl
class KeygenTests(ToolTestCase):
    def keygen(self, name: str = "new.key", *more: Any, **options: Any):
        return self.run_tool("keygen", "--out", self.root / name, *more, **options)

    def passphrase_file(
        self, text: str = PASSPHRASE + "\n", name: str = "pass.txt"
    ) -> Path:
        return self.write(name, text.encode("utf-8"))

    def call_main(self, *arguments: Any):
        """``plugin_sign.main`` in this process: (code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = ps.main([str(a) for a in arguments])
            except SystemExit as stop:
                code = stop.code
        return code, out.getvalue(), err.getvalue()

    def call_with_prompt(self, answers: List[str], *arguments: Any):
        """``call_main`` with a terminal that gives ``answers`` in turn:
        (code, stdout, stderr, number of questions)."""
        asked = unittest.mock.Mock(side_effect=answers)
        with unittest.mock.patch.object(ps, "stdin_is_terminal", return_value=True):
            with unittest.mock.patch.object(ps.getpass, "getpass", asked):
                code, out, err = self.call_main(*arguments)
        return code, out, err, asked.call_count

    def sign_and_verify(
        self, key: Path, public_key: str, *options: Any, **run: Any
    ) -> None:
        folder = self.make_plugin("signed")
        done = self.run_tool(
            "plugin", "--key", key, "--key-id", "made-key", "--plugin", folder,
            *options, secrets=[PASSPHRASE], **run,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        done = self.run_tool(
            "verify", "--public-key", public_key, "--purpose", "plugin",
            folder / "plugin.sig",
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "ok\n")

    # -- a key that is not encrypted

    def test_keygen_makes_a_key_that_signs_and_verifies(self) -> None:
        done = self.keygen("new.key", "--key-id", "made-key")
        self.assertEqual(done.returncode, 0, done.stderr)
        key = self.root / "new.key"
        self.assertTrue(key.read_bytes().startswith(b"-----BEGIN PRIVATE KEY-----\n"))
        lines = done.stdout.splitlines()
        self.assertEqual(len(lines), 2, done.stdout)
        self.assertRegex(lines[0], r"^[0-9a-f]{64}$")
        self.assertEqual(lines[1], "made-key")
        # The same text as public-key prints.
        again = self.run_tool("public-key", "--key", key)
        self.assertEqual(again.stdout, lines[0] + "\n")
        self.sign_and_verify(key, lines[0])
        # Two keys are two keys.
        other = self.keygen("other.key")
        self.assertNotEqual(other.stdout.strip(), lines[0])

    def test_keygen_without_a_key_id_prints_the_public_key_only(self) -> None:
        done = self.keygen()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertRegex(done.stdout, r"^[0-9a-f]{64}\n$")
        # The key is not encrypted, and the tool says so.
        self.assertIn("not encrypted", done.stderr)

    def test_keygen_prints_no_private_material(self) -> None:
        done = self.keygen("new.key", "--key-id", "made-key")
        self.assertEqual(done.returncode, 0, done.stderr)
        body = key_body(self.root / "new.key")
        seed = base64.b64decode(body)[-32:]
        for output in (done.stdout, done.stderr):
            self.assertNotIn(body, output)
            self.assertNotIn(seed.hex(), output)
            self.assertNotIn(base64.urlsafe_b64encode(seed).decode(), output)
            self.assertNotIn("PRIVATE", output)

    def test_keygen_makes_the_file_private_to_its_owner(self) -> None:
        done = self.keygen()
        self.assertEqual(done.returncode, 0, done.stderr)
        key = self.root / "new.key"
        if os.name != "nt":
            self.assertEqual(key.stat().st_mode & 0o777, 0o600)
            return
        # Windows has no mode: icacls lists who may use the file. After
        # keygen it is one entry, the current user, with full control and
        # nothing inherited from the folder. The names of the entries depend
        # on the language of Windows, so only their number is counted.
        listing = subprocess.run(
            [ps.windows_program("icacls"), str(key)],
            capture_output=True, text=True, check=True,
        ).stdout
        self.assertEqual(listing.count(":("), 1, listing)
        self.assertIn(":(F)", listing)
        self.assertNotIn("(I)", listing)

    @unittest.skipUnless(os.name == "nt", "Windows permissions")
    def test_a_file_with_entries_of_its_own_ends_with_one(self) -> None:
        # Some versions of Python make a new file with entries for SYSTEM,
        # Administrators and the owner on the file itself, not inherited.
        # They have to go as well.
        key = self.write("own-entries.key", b"")
        icacls = ps.windows_program("icacls")
        subprocess.run(
            [icacls, str(key), "/grant", "*S-1-5-18:F", "*S-1-5-32-544:F", "*S-1-3-4:F"],
            capture_output=True, text=True, check=True,
        )
        ps.restrict_to_current_user(str(key))
        listing = subprocess.run(
            [icacls, str(key)], capture_output=True, text=True, check=True
        ).stdout
        self.assertEqual(listing.count(":("), 1, listing)
        self.assertIn(":(F)", listing)
        self.assertNotIn("(I)", listing)

    def test_keygen_refuses_a_file_that_exists(self) -> None:
        key = self.write("new.key", b"keep this\n")
        done = self.keygen()
        self.assertEqual(done.returncode, 2)
        self.assertIn("already exists", done.stderr)
        self.assertEqual(done.stdout, "")
        self.assertEqual(key.read_bytes(), b"keep this\n")
        # A folder too, and the second run of a keygen that worked.
        (self.root / "folder").mkdir()
        self.assertEqual(self.keygen("folder").returncode, 2)
        self.assertEqual(self.keygen("fresh.key").returncode, 0)
        first = (self.root / "fresh.key").read_bytes()
        self.assertEqual(self.keygen("fresh.key").returncode, 2)
        self.assertEqual((self.root / "fresh.key").read_bytes(), first)

    def test_a_file_that_appears_during_keygen_is_not_touched(self) -> None:
        # The file is made by one call that fails when it exists, so a file
        # that appears after the first look is neither written nor removed.
        key = self.root / "race.key"
        real_lexists = os.path.lexists

        def late(path: str) -> bool:
            found = real_lexists(path)
            if not found and Path(path) == key:
                key.write_bytes(b"not yours\n")
            return found

        with unittest.mock.patch.object(ps.os.path, "lexists", side_effect=late):
            code, out, err = self.call_main("keygen", "--out", key)
        self.assertEqual(code, 2)
        self.assertIn("already exists", err)
        self.assertEqual(out, "")
        self.assertEqual(key.read_bytes(), b"not yours\n")

    def test_keygen_with_a_bad_key_id_writes_nothing(self) -> None:
        for bad in ("Upper", "has space", "", "x" * 65):
            done = self.keygen("new.key", "--key-id", bad)
            self.assertEqual(done.returncode, 2, repr(bad))
            self.assertEqual(done.stdout, "")
        self.assertFalse((self.root / "new.key").exists())

    def test_keygen_in_a_folder_that_is_not_there(self) -> None:
        done = self.keygen("no-folder/new.key")
        self.assertEqual(done.returncode, 2)
        self.assertEqual(done.stdout, "")

    def test_a_failure_after_the_file_is_made_removes_it(self) -> None:
        key = self.root / "new.key"
        refusal = ps.Refusal("could not restrict the key file")
        with unittest.mock.patch.object(ps, "protect_key_file", side_effect=refusal):
            code, out, err = self.call_main("keygen", "--out", key)
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertIn("could not restrict", err)
        self.assertFalse(key.exists())

        # And when the key that was written cannot be read back.
        with unittest.mock.patch.object(
            ps, "public_key_of", side_effect=ps.Refusal("no")
        ):
            code, out, err = self.call_main("keygen", "--out", key)
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertFalse(key.exists())

    def test_a_failed_openssl_leaves_no_file(self) -> None:
        key = self.root / "new.key"
        done = self.keygen("new.key", "--openssl", self.root / "no-openssl")
        self.assertEqual(done.returncode, 2)
        self.assertFalse(key.exists())
        failed = subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"boom")
        with unittest.mock.patch.object(ps, "run_openssl", return_value=failed):
            code, out, err = self.call_main("keygen", "--out", key)
        self.assertEqual(code, 1)
        self.assertFalse(key.exists())

    # -- a key that is encrypted

    def test_keygen_with_a_passphrase_from_the_environment(self) -> None:
        env = dict(os.environ, TEST_PLUGIN_PASS=PASSPHRASE)
        done = self.keygen(
            "enc.key", "--key-id", "made-key", "--passphrase-env",
            "TEST_PLUGIN_PASS", env=env, secrets=[PASSPHRASE],
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        key = self.root / "enc.key"
        self.assertTrue(key.read_bytes().startswith(ENCRYPTED_HEADER))
        self.assertEqual(done.stderr, "")
        public_key = done.stdout.splitlines()[0]
        self.sign_and_verify(
            key, public_key, "--passphrase-env", "TEST_PLUGIN_PASS", env=env
        )
        # The same key gives the same public key.
        shown = self.run_tool(
            "public-key", "--key", key, "--passphrase-env", "TEST_PLUGIN_PASS",
            env=env, secrets=[PASSPHRASE],
        )
        self.assertEqual(shown.stdout, public_key + "\n")

    def test_keygen_with_a_passphrase_from_a_file(self) -> None:
        pass_file = self.passphrase_file()
        done = self.keygen(
            "enc.key", "--passphrase-file", pass_file, secrets=[PASSPHRASE]
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        key = self.root / "enc.key"
        self.assertTrue(key.read_bytes().startswith(ENCRYPTED_HEADER))
        # Neither the encrypted text nor the passphrase is in the output.
        self.assertNotIn(key_body(key), done.stdout + done.stderr)
        self.assertEqual(done.stderr, "")
        self.sign_and_verify(
            key, done.stdout.strip(), "--passphrase-file", pass_file
        )

    def test_the_passphrase_is_the_first_line_without_its_ending(self) -> None:
        # A Windows line ending, and more lines that are not the passphrase.
        pass_file = self.passphrase_file(PASSPHRASE + "\r\nsecond line\n")
        done = self.keygen("enc.key", "--passphrase-file", pass_file)
        self.assertEqual(done.returncode, 0, done.stderr)
        same = self.passphrase_file(PASSPHRASE, "plain.txt")  # no line ending
        shown = self.run_tool(
            "public-key", "--key", self.root / "enc.key",
            "--passphrase-file", same,
        )
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertEqual(shown.stdout, done.stdout)

    def test_a_passphrase_can_have_spaces_and_non_ascii_text(self) -> None:
        pass_file = self.passphrase_file("  pass phrase ü中  \n")
        done = self.keygen("enc.key", "--passphrase-file", pass_file)
        self.assertEqual(done.returncode, 0, done.stderr)
        shown = self.run_tool(
            "public-key", "--key", self.root / "enc.key",
            "--passphrase-file", pass_file,
        )
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertEqual(shown.stdout, done.stdout)

    def test_a_passphrase_for_a_key_that_has_none_is_ignored(self) -> None:
        self.assertEqual(self.keygen().returncode, 0)
        done = self.run_tool(
            "public-key", "--key", self.root / "new.key",
            "--passphrase-file", self.passphrase_file(),
        )
        self.assertEqual(done.returncode, 0, done.stderr)

    # -- what is refused

    def encrypted_key(self) -> Path:
        done = self.keygen("enc.key", "--passphrase-file", self.passphrase_file())
        self.assertEqual(done.returncode, 0, done.stderr)
        return self.root / "enc.key"

    def test_signing_with_what_cannot_open_the_key(self) -> None:
        key = self.encrypted_key()
        folder = self.make_plugin()
        wrong = self.passphrase_file(WRONG_PASSPHRASE + "\n", "wrong.txt")
        empty = self.passphrase_file("", "empty.txt")
        env = dict(os.environ, TEST_PLUGIN_WRONG=WRONG_PASSPHRASE, TEST_PLUGIN_EMPTY="")
        env.pop("TEST_PLUGIN_UNSET", None)
        cases = (
            ("a wrong passphrase", 1, "passphrase", ["--passphrase-file", wrong]),
            ("a wrong passphrase in the environment", 1, "passphrase",
             ["--passphrase-env", "TEST_PLUGIN_WRONG"]),
            ("no passphrase option", 1, "encrypted", []),
            ("an unset variable", 2, "not set",
             ["--passphrase-env", "TEST_PLUGIN_UNSET"]),
            ("a missing passphrase file", 2, "could not read",
             ["--passphrase-file", self.root / "no-such.txt"]),
            ("an empty file", 2, "empty", ["--passphrase-file", empty]),
            ("an empty variable", 2, "empty",
             ["--passphrase-env", "TEST_PLUGIN_EMPTY"]),
            ("two options", 2, "not allowed with",
             ["--passphrase-file", wrong, "--passphrase-env", "TEST_PLUGIN_WRONG"]),
            ("a prompt with no terminal", 2, "terminal", ["--passphrase-prompt"]),
        )
        for what, code, text, options in cases:
            done = self.run_tool(
                "plugin", "--key", key, "--key-id", "made-key", "--plugin",
                folder, *options, env=env, timeout=60,
                secrets=[PASSPHRASE, WRONG_PASSPHRASE],
            )
            self.assertEqual(done.returncode, code, what + ": " + done.stderr)
            self.assertIn(text, done.stderr, what)
            self.assertEqual(done.stdout, "", what)
            self.assertFalse((folder / "plugin.sig").exists(), what)

    def test_every_command_that_takes_a_key_refuses_an_encrypted_one(self) -> None:
        key = self.encrypted_key()
        keys = self.write("keys.json", as_bytes(key_list_document()))
        revoked = self.write("revoked-in.json", as_bytes(revocations_document()))
        for command, extra, out in (
            ("public-key", [], None),
            ("key-list", ["--key-id", "root-1", "--payload", keys],
             self.root / "list.json"),
            ("revocations", ["--key-id", "root-1", "--payload", revoked],
             self.root / "revoked.json"),
        ):
            if out is not None:
                extra = extra + ["--out", out]
            done = self.run_tool(command, "--key", key, *extra, timeout=60)
            self.assertEqual(done.returncode, 1, command)
            self.assertIn("encrypted", done.stderr, command)
            self.assertEqual(done.stdout, "", command)
            if out is not None:
                self.assertFalse(out.exists(), command)

    def test_keygen_refuses_what_gives_no_passphrase(self) -> None:
        empty = self.passphrase_file("", "empty.txt")
        env = dict(os.environ, TEST_PLUGIN_EMPTY="", TEST_PLUGIN_PASS=PASSPHRASE)
        env.pop("TEST_PLUGIN_UNSET", None)
        cases = (
            (2, "not set", ["--passphrase-env", "TEST_PLUGIN_UNSET"]),
            (2, "could not read", ["--passphrase-file", self.root / "no-such.txt"]),
            (2, "empty", ["--passphrase-file", empty]),
            (2, "empty", ["--passphrase-env", "TEST_PLUGIN_EMPTY"]),
            (2, "not allowed with",
             ["--passphrase-file", empty, "--passphrase-env", "TEST_PLUGIN_PASS"]),
            (2, "terminal", ["--passphrase-prompt"]),
        )
        for code, text, options in cases:
            done = self.keygen(
                "new.key", *options, env=env, timeout=60, secrets=[PASSPHRASE]
            )
            self.assertEqual(done.returncode, code, done.stderr)
            self.assertIn(text, done.stderr)
            self.assertEqual(done.stdout, "")
            self.assertFalse((self.root / "new.key").exists(), options)

    def test_no_option_takes_the_passphrase_itself(self) -> None:
        for option in ("--passphrase", "--passin", "--password", "--pass"):
            done = self.keygen(
                "new.key", option, "hunter2-never-shown", secrets=["hunter2"]
            )
            self.assertEqual(done.returncode, 2, option)
            self.assertFalse((self.root / "new.key").exists())
        # The name of an environment variable that is not set is not shown:
        # it may be a passphrase typed in the wrong place.
        done = self.keygen(
            "new.key", "--passphrase-env", "hunter2-never-shown",
            secrets=["hunter2"],
        )
        self.assertEqual(done.returncode, 2)

    def test_the_passphrase_is_never_in_the_arguments_of_openssl(self) -> None:
        real_run = subprocess.run
        calls: List[Any] = []

        def spy(command: Any, *args: Any, **kwargs: Any) -> Any:
            calls.append((list(command), kwargs.get("env")))
            return real_run(command, *args, **kwargs)

        key = self.root / "enc.key"
        folder = self.make_plugin()
        before = dict(os.environ)
        with unittest.mock.patch.dict(os.environ, {"TEST_PLUGIN_PASS": PASSPHRASE}):
            with unittest.mock.patch.object(ps.subprocess, "run", side_effect=spy):
                code, out, err = self.call_main(
                    "keygen", "--out", key, "--passphrase-env", "TEST_PLUGIN_PASS"
                )
                self.assertEqual(code, 0, err)
                code, out2, err2 = self.call_main(
                    "plugin", "--key", key, "--key-id", "made-key", "--plugin",
                    folder, "--passphrase-env", "TEST_PLUGIN_PASS",
                )
                self.assertEqual(code, 0, err2)
            # This process's environment never holds the private variable.
            self.assertNotIn(ps.PASSPHRASE_VAR, os.environ)
        self.assertEqual(dict(os.environ), before)
        for text in (out, err, out2, err2):
            self.assertNotIn(PASSPHRASE, text)

        used = 0
        for command, environment in calls:
            for argument in command:
                self.assertNotIn(PASSPHRASE, argument)
                self.assertFalse(argument.startswith("pass:"), argument)
            for option in ("-passin", "-pass"):
                if option in command:
                    self.assertEqual(
                        command[command.index(option) + 1],
                        "env:" + ps.PASSPHRASE_VAR,
                    )
                    self.assertEqual(environment[ps.PASSPHRASE_VAR], PASSPHRASE)
                    used += 1
        # genpkey, then the read back, the public key and the signature.
        self.assertGreaterEqual(used, 4)

    # -- the prompt, with a terminal that the test supplies

    def test_the_prompt_asks_twice_for_a_new_key_and_once_to_use_it(self) -> None:
        key = self.root / "enc.key"
        code, made, err, asked = self.call_with_prompt(
            [PASSPHRASE, PASSPHRASE],
            "keygen", "--out", key, "--passphrase-prompt",
        )
        self.assertEqual(code, 0, err)
        self.assertEqual(asked, 2)
        self.assertTrue(key.read_bytes().startswith(ENCRYPTED_HEADER))
        code, shown, err, asked = self.call_with_prompt(
            [PASSPHRASE], "public-key", "--key", key, "--passphrase-prompt",
        )
        self.assertEqual(code, 0, err)
        self.assertEqual(asked, 1)
        self.assertEqual(shown, made)
        code, shown, err, asked = self.call_with_prompt(
            [WRONG_PASSPHRASE], "public-key", "--key", key, "--passphrase-prompt",
        )
        self.assertEqual(code, 1)
        self.assertNotIn(WRONG_PASSPHRASE, err)

    def test_two_different_answers_at_the_prompt_are_an_error(self) -> None:
        key = self.root / "enc.key"
        code, out, err, asked = self.call_with_prompt(
            [PASSPHRASE, WRONG_PASSPHRASE],
            "keygen", "--out", key, "--passphrase-prompt",
        )
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertIn("not the same", err)
        self.assertNotIn(PASSPHRASE, err)
        self.assertNotIn(WRONG_PASSPHRASE, err)
        self.assertFalse(key.exists())

    def test_an_empty_answer_at_the_prompt_is_refused(self) -> None:
        key = self.root / "enc.key"
        code, out, err, asked = self.call_with_prompt(
            ["", ""], "keygen", "--out", key, "--passphrase-prompt"
        )
        self.assertEqual(code, 2)
        self.assertIn("empty", err)
        self.assertFalse(key.exists())

    def test_standard_input_that_is_not_a_terminal_is_not_one(self) -> None:
        # The null device: isatty says yes for it on Windows.
        with open(os.devnull) as null:
            with unittest.mock.patch.object(ps.sys, "stdin", null):
                self.assertFalse(ps.stdin_is_terminal())
        with unittest.mock.patch.object(ps.sys, "stdin", None):
            self.assertFalse(ps.stdin_is_terminal())


if __name__ == "__main__":
    unittest.main()
