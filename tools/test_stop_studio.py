#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for cmake/packaging/windows/stop-studio.ps1.

    python3 -m unittest discover -s tools -p "test_stop_studio.py" -v

The installer uses the script to find and end the programs of Studio that run from the install folder (Check
and Stop) and, on a real uninstall, to take away the sign-in entry and the "ports are running" mark that Studio
leaves in each user's settings (Forget). The tests run it under Windows PowerShell 5.1, the one the installer
uses. Programs are copies of PING.EXE named vss_cli.exe and vss_gui.exe in a scratch folder. The registry is a
scratch key under HKEY_CURRENT_USER that stands in for HKEY_USERS, and the data folders are scratch folders:
nothing real is read or written. The tests are skipped elsewhere.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "cmake" / "packaging" / "windows" / "stop-studio.ps1"
POWERSHELL = shutil.which("powershell") if sys.platform == "win32" else None
PING = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PING.EXE"
RUN_VALUE = "Virtual Serial Studio"
SID = "S-1-5-21-1-2-3-1001"
SID_2 = "S-1-5-21-1-2-3-1002"
SID_3 = "S-1-5-21-1-2-3-1003"
BOM = b"\xef\xbb\xbf"
# What Qt writes: compact, keys in order.
RUNNING = '{"dirty":false,"documentPath":"","setup":{"name":"demo","ports":[]},"stopped":false,"version":1}'
RUNNING_MARKED = RUNNING.replace('"stopped":false', '"stopped": true')


def ps_quote(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def run_ps(command: str, **env: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
        env={**os.environ, **env},
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )


def delete_key_tree(winreg, path: str) -> None:  # type: ignore[no-untyped-def]
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ)
    except FileNotFoundError:
        return
    with key:
        names = []
        index = 0
        while True:
            try:
                names.append(winreg.EnumKey(key, index))
            except OSError:
                break
            index += 1
    for name in names:
        delete_key_tree(winreg, f"{path}\\{name}")
    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class StopStudio(unittest.TestCase):
    def setUp(self) -> None:
        import winreg

        self.winreg = winreg
        self.root = Path(tempfile.mkdtemp(prefix="vss-stop-studio-")).resolve()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.bin = self.root / "bin"
        self.other = self.root / "other"
        self.data = self.root / "data"
        for folder in (self.bin, self.other, self.data):
            folder.mkdir()
        self.users_key = f"Software\\VssStopStudioTest-{uuid.uuid4().hex}"
        winreg.CreateKey(winreg.HKEY_CURRENT_USER, self.users_key).Close()
        self.addCleanup(delete_key_tree, winreg, self.users_key)

    # Programs ------------------------------------------------------------------------------------

    def start(self, folder: Path, name: str) -> subprocess.Popen[bytes]:
        """Starts a program that runs from folder and keeps running for two minutes."""
        exe = folder / name
        shutil.copyfile(PING, exe)
        process = subprocess.Popen(
            [str(exe), "-n", "120", "127.0.0.1"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        def finish() -> None:
            process.kill()
            process.wait(timeout=30)

        # Cleanups run last in, first out: the process ends before the folder is deleted.
        self.addCleanup(finish)
        return process

    def script(self, action: str, directory: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return run_ps(
            f"& {ps_quote(str(SCRIPT))} -Action {action} -Directory {ps_quote(directory)} {' '.join(extra)}; "
            "exit $LASTEXITCODE"
        )

    def check(self, directory: str | None = None) -> subprocess.CompletedProcess[str]:
        return self.script("Check", directory or str(self.bin))

    def stop(self, directory: str | None = None) -> subprocess.CompletedProcess[str]:
        return self.script("Stop", directory or str(self.bin), "-Seconds 30")

    def forget(
        self, data_folders: list[Path] | None = None, users_key: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        # Both are always given, so that the script never falls back to the real users.
        folders = data_folders if data_folders is not None else [self.data]
        self.assertTrue(folders)
        return self.script(
            "Forget",
            str(self.bin),
            f"-UsersKey {ps_quote(users_key or self.users_key)}",
            "-RunSubKey Run",
            f"-RunValue {ps_quote(RUN_VALUE)}",
            "-DataFolders " + ",".join(ps_quote(str(folder)) for folder in folders),
        )

    def assertRuns(self, process: subprocess.Popen[bytes]) -> None:
        self.assertIsNone(process.poll(), "the program was ended")

    def assertEnded(self, process: subprocess.Popen[bytes]) -> None:
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.fail("the program still runs")

    # Registry ------------------------------------------------------------------------------------

    def set_run(self, sid: str, values: dict[str, str]) -> None:
        winreg = self.winreg
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"{self.users_key}\\{sid}\\Run") as key:
            for name, value in values.items():
                winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)

    def get_run(self, sid: str) -> dict[str, tuple[object, int]]:
        winreg = self.winreg
        values: dict[str, tuple[object, int]] = {}
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, f"{self.users_key}\\{sid}\\Run") as key:
            index = 0
            while True:
                try:
                    name, value, kind = winreg.EnumValue(key, index)
                except OSError:
                    break
                values[name] = (value, kind)
                index += 1
        return values

    def entry(self, sid: str) -> object:
        """The sign-in entry of Studio for sid, or None."""
        found = self.get_run(sid).get(RUN_VALUE)
        return found[0] if found else None

    # Data folders --------------------------------------------------------------------------------

    def setup_file(self, folder: Path, content: bytes | str = RUNNING) -> Path:
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / "running-setup.json"
        path.write_bytes(content if isinstance(content, bytes) else content.encode("utf-8"))
        return path

    # Check ---------------------------------------------------------------------------------------

    def test_check_says_nothing_runs_when_no_program_runs_from_the_folder(self) -> None:
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_check_says_a_program_runs_and_changes_nothing(self) -> None:
        process = self.start(self.bin, "vss_cli.exe")
        result = self.check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("vss_cli.exe", result.stdout)
        self.assertRuns(process)

    def test_check_does_not_count_a_program_of_the_same_name_in_another_folder(self) -> None:
        process = self.start(self.other, "vss_cli.exe")
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRuns(process)

    # Stop ----------------------------------------------------------------------------------------

    def test_stop_ends_the_programs_that_run_from_the_folder(self) -> None:
        cli = self.start(self.bin, "vss_cli.exe")
        gui = self.start(self.bin, "vss_gui.exe")
        result = self.stop()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEnded(cli)
        self.assertEnded(gui)
        self.assertEqual(self.check().returncode, 0)

    def test_stop_leaves_a_program_of_the_same_name_that_runs_from_another_folder(self) -> None:
        mine = self.start(self.bin, "vss_gui.exe")
        theirs = self.start(self.other, "vss_gui.exe")
        result = self.stop()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEnded(mine)
        self.assertRuns(theirs)

    def test_stop_matches_a_trailing_backslash_and_a_different_letter_case(self) -> None:
        process = self.start(self.bin, "vss_cli.exe")
        directory = str(self.bin).upper() + "\\"
        self.assertEqual(self.check(directory).returncode, 1)
        result = self.stop(directory)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEnded(process)

    def test_stop_with_nothing_running_succeeds(self) -> None:
        result = self.stop()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_stop_leaves_a_program_in_the_folder_that_is_not_one_of_ours(self) -> None:
        stranger = self.start(self.bin, "other.exe")
        result = self.stop()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRuns(stranger)
        self.assertEqual(self.check().returncode, 0)

    # Forget: sign-in entries ---------------------------------------------------------------------

    def test_forget_removes_a_quoted_sign_in_entry_that_starts_studio_from_the_folder(self) -> None:
        self.set_run(SID, {RUN_VALUE: f'"{self.bin}\\vss_gui.exe" --background'})
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNone(self.entry(SID))

    def test_forget_removes_a_quoted_entry_whose_folder_has_spaces(self) -> None:
        spaced = self.root / "Program Files" / "Virtual Serial Studio" / "bin"
        spaced.mkdir(parents=True)
        self.set_run(SID, {RUN_VALUE: f'"{spaced}\\vss_gui.exe" --background'})
        result = self.script(
            "Forget",
            str(spaced),
            f"-UsersKey {ps_quote(self.users_key)}",
            "-RunSubKey Run",
            f"-RunValue {ps_quote(RUN_VALUE)}",
            f"-DataFolders {ps_quote(str(self.data))}",
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNone(self.entry(SID))

    def test_forget_leaves_an_entry_that_starts_studio_from_another_folder(self) -> None:
        self.set_run(SID, {RUN_VALUE: f'"{self.other}\\vss_gui.exe" --background'})
        before = self.get_run(SID)
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.get_run(SID), before)

    def test_forget_recognises_an_unquoted_command(self) -> None:
        # The path has no spaces here. An unquoted path with spaces is matched only up to " --".
        self.set_run(SID, {RUN_VALUE: f"{self.bin}\\vss_gui.exe --background"})
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNone(self.entry(SID))

    def test_forget_does_not_touch_keys_that_are_not_a_user_sid(self) -> None:
        command = f'"{self.bin}\\vss_gui.exe" --background'
        names = [".DEFAULT", "S-1-5-18", f"{SID}_Classes"]
        for name in names:
            self.set_run(name, {RUN_VALUE: command})
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        for name in names:
            self.assertEqual(self.get_run(name), {RUN_VALUE: (command, self.winreg.REG_SZ)}, name)

    def test_forget_keeps_the_other_values_of_the_run_key(self) -> None:
        other = {"OtherApp": '"C:\\Other\\other.exe" /tray', "Third": "C:\\Third\\third.exe"}
        self.set_run(SID, {**other, RUN_VALUE: f'"{self.bin}\\vss_gui.exe" --background'})
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.get_run(SID), {name: (value, self.winreg.REG_SZ) for name, value in other.items()})

    def test_forget_handles_several_users_at_once(self) -> None:
        command = f'"{self.bin}\\vss_gui.exe" --background'
        elsewhere = f'"{self.other}\\vss_gui.exe" --background'
        self.set_run(SID, {RUN_VALUE: command})
        self.set_run(SID_2, {RUN_VALUE: elsewhere})
        self.set_run(SID_3, {RUN_VALUE: f"{self.bin}\\vss_gui.exe --background"})
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNone(self.entry(SID))
        self.assertEqual(self.entry(SID_2), elsewhere)
        self.assertIsNone(self.entry(SID_3))

    def test_forget_with_a_missing_users_key_is_an_error_that_changes_nothing(self) -> None:
        setup = self.setup_file(self.data)
        result = self.forget(users_key=f"{self.users_key}-missing")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertEqual(setup.read_bytes(), RUNNING.encode("utf-8"))

    # Forget: running setups ----------------------------------------------------------------------

    def test_forget_marks_a_running_setup_as_stopped_and_changes_nothing_else(self) -> None:
        setup = self.setup_file(self.data)
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        content = setup.read_bytes()
        self.assertFalse(content.startswith(BOM))
        self.assertEqual(content, RUNNING_MARKED.encode("utf-8"))

    def test_forget_leaves_a_setup_that_already_says_stopped(self) -> None:
        text = RUNNING.replace('"stopped":false', '"stopped": true')
        setup = self.setup_file(self.data, text)
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(setup.read_bytes(), text.encode("utf-8"))

    def test_forget_changes_only_the_real_mark_not_text_in_a_port_name(self) -> None:
        name = 'tricky "stopped": false name'
        text = json.dumps(
            {
                "dirty": False,
                "documentPath": "",
                "setup": {"name": "demo", "ports": [{"name": name}]},
                "stopped": False,
                "version": 1,
            },
            separators=(",", ":"),
        )
        self.assertEqual(text.count('"stopped":false'), 1)
        self.assertIn('\\"stopped\\": false', text)
        setup = self.setup_file(self.data, text)
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        content = setup.read_bytes().decode("utf-8")
        self.assertEqual(content, text.replace('"stopped":false', '"stopped": true'))
        parsed = json.loads(content)
        self.assertIs(parsed["stopped"], True)
        self.assertEqual(parsed["setup"]["ports"][0]["name"], name)

    def test_forget_keeps_non_ascii_text_as_it_was(self) -> None:
        text = json.dumps(
            {"dirty": False, "setup": {"ports": [{"name": "Gerät-温度"}]}, "stopped": False, "version": 1},
            separators=(",", ":"),
            ensure_ascii=False,
        )
        setup = self.setup_file(self.data, text)
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        content = setup.read_bytes()
        self.assertFalse(content.startswith(BOM))
        self.assertEqual(content, text.replace('"stopped":false', '"stopped": true').encode("utf-8"))
        self.assertEqual(json.loads(content.decode("utf-8"))["setup"]["ports"][0]["name"], "Gerät-温度")

    def test_forget_skips_a_data_folder_that_is_missing_or_has_no_setup(self) -> None:
        empty = self.root / "empty"
        empty.mkdir()
        result = self.forget([self.root / "missing", empty])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((empty / "running-setup.json").exists())
        self.assertFalse((self.root / "missing").exists())

    def test_forget_leaves_a_file_that_is_not_json_and_lacks_the_text(self) -> None:
        setup = self.setup_file(self.data, b"this is not json at all\r\n")
        result = self.forget()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(setup.read_bytes(), b"this is not json at all\r\n")

    def test_forget_skips_a_data_folder_that_is_a_link(self) -> None:
        target = self.root / "target"
        original = self.setup_file(target).read_bytes()
        link = self.root / "link"
        made = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True, text=True, check=False
        )
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        # Cleanups run last in, first out: the junction goes before the folder tree, and never through it.
        self.addCleanup(os.rmdir, link)
        result = self.forget([link])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("link", result.stdout)
        self.assertEqual((target / "running-setup.json").read_bytes(), original)
        self.assertEqual(original, RUNNING.encode("utf-8"))

    def test_forget_handles_two_data_folders_in_one_call(self) -> None:
        first = self.setup_file(self.root / "one")
        second = self.setup_file(self.root / "two")
        result = self.forget([self.root / "one", self.root / "two"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(first.read_bytes(), RUNNING_MARKED.encode("utf-8"))
        self.assertEqual(second.read_bytes(), RUNNING_MARKED.encode("utf-8"))

    def test_forget_never_deletes_a_running_setup(self) -> None:
        marked = self.setup_file(self.root / "marked")
        stopped = self.setup_file(self.root / "stopped", RUNNING_MARKED)
        odd = self.setup_file(self.root / "odd", b"not json")
        empty = self.setup_file(self.root / "blank", b"")
        result = self.forget([path.parent for path in (marked, stopped, odd, empty)])
        self.assertEqual(result.returncode, 0, result.stdout)
        for path in (marked, stopped, odd, empty):
            self.assertTrue(path.is_file(), path)


if __name__ == "__main__":
    unittest.main()
