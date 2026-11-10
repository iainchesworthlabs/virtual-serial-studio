#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for cmake/packaging/windows/edit-path.ps1.

    python3 -m unittest discover -s tools -p "test_edit_path.py" -v

The installer uses the script to add Studio to the machine PATH and to take it out. The
tests run it under Windows PowerShell 5.1, the one the installer uses, against a scratch
key under HKEY_CURRENT_USER, never the real PATH, and are skipped elsewhere.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import unittest
import uuid
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "cmake" / "packaging" / "windows" / "edit-path.ps1"
POWERSHELL = shutil.which("powershell") if sys.platform == "win32" else None
FOLDER = r"C:\Program Files\Virtual Serial Studio\bin"
MAX_LENGTH = 32767


def run_ps(command: str, **env: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
        env={**os.environ, **env},
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class EditPath(unittest.TestCase):
    def setUp(self) -> None:
        self.subkey = f"Software\\VssEditPathTest-{uuid.uuid4().hex}"
        self.addCleanup(lambda: run_ps(f"Remove-Item -LiteralPath 'HKCU:\\{self.subkey}' -Recurse -Force -ErrorAction SilentlyContinue"))

    def write(self, value: str, kind: str = "ExpandString") -> None:
        # [Microsoft.Win32.Registry] keeps the value as written, %VARS% included. The value
        # goes in an environment variable: a command line stops at 32 K characters.
        result = run_ps(
            f"$k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('{self.subkey}');"
            f"$k.SetValue('Path', $env:VSS_TEST_VALUE, [Microsoft.Win32.RegistryValueKind]::{kind}); $k.Close()",
            VSS_TEST_VALUE=value,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def read(self) -> tuple[str, str]:
        result = run_ps(
            f"$k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('{self.subkey}');"
            "$k.GetValue('Path', '', 'DoNotExpandEnvironmentNames'); $k.GetValueKind('Path'); $k.Close()"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        value, kind = result.stdout.strip().splitlines()
        return value, kind

    def edit(self, action: str) -> subprocess.CompletedProcess[str]:
        return run_ps(
            f"& '{SCRIPT}' -Action {action} -Directory '{FOLDER}' -Hive CurrentUser -SubKey '{self.subkey}'; exit $LASTEXITCODE"
        )

    def test_add_appends_and_keeps_the_other_entries_as_written(self) -> None:
        self.write(r"%SystemRoot%\system32;C:\Tools")
        self.assertEqual(self.edit("Add").returncode, 0)
        self.assertEqual(self.read(), (rf"%SystemRoot%\system32;C:\Tools;{FOLDER}", "ExpandString"))

    def test_add_twice_adds_once_even_with_a_trailing_backslash_or_other_case(self) -> None:
        self.write(rf"C:\Tools;{FOLDER.upper()}\\")
        before = self.read()
        self.assertEqual(self.edit("Add").returncode, 0)
        self.assertEqual(self.read(), before)

    def test_add_to_an_empty_path(self) -> None:
        self.write("")
        self.assertEqual(self.edit("Add").returncode, 0)
        self.assertEqual(self.read()[0], FOLDER)

    def test_remove_takes_only_that_folder(self) -> None:
        self.write(rf"C:\One;{FOLDER};C:\Two")
        self.assertEqual(self.edit("Remove").returncode, 0)
        self.assertEqual(self.read()[0], r"C:\One;C:\Two")

    def test_remove_when_absent_changes_nothing(self) -> None:
        self.write(r"C:\One;C:\Two")
        self.assertEqual(self.edit("Remove").returncode, 0)
        self.assertEqual(self.read()[0], r"C:\One;C:\Two")

    def test_a_path_longer_than_the_installer_string_limit_is_extended_whole(self) -> None:
        # The case that broke the installer: a PATH of several thousand characters.
        entries = [rf"C:\Some\Long\Folder\Name\{n:04}" for n in range(200)]
        original = ";".join(entries)
        self.assertGreater(len(original), 4000)
        self.write(original)
        self.assertEqual(self.edit("Add").returncode, 0)
        self.assertEqual(self.read()[0], f"{original};{FOLDER}")
        self.assertEqual(self.edit("Remove").returncode, 0)
        self.assertEqual(self.read()[0], original)

    def test_a_path_that_would_be_too_long_is_left_alone(self) -> None:
        filler = "C:\\" + "x" * 250
        original = ";".join([filler] * (MAX_LENGTH // 250))
        original = original[: MAX_LENGTH - 10]
        self.write(original)
        result = self.edit("Add")
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertEqual(self.read()[0], original)

    def test_a_missing_key_is_an_error_and_makes_nothing(self) -> None:
        result = self.edit("Add")
        self.assertEqual(result.returncode, 2, result.stdout)


if __name__ == "__main__":
    unittest.main()
