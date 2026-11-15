#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks on how the Windows installer tells an upgrade from an uninstall.

    python3 -m unittest discover -s tools -p "test_windows_installer.py" -v

Nothing here runs an installer. The installer runs the uninstaller of the version it
replaces, and that uninstaller keeps the driver, the helper service and the PATH entry
only when it is told it is part of an upgrade. These tests check the text that carries
that message (cmake/packaging/windows/nsis-template.cmake and driver.nsh); the installer
check in CI (tools/ci/check-windows-installer.ps1) runs the result.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGING = ROOT / "cmake" / "packaging"
TEMPLATE_CMAKE = PACKAGING / "windows" / "nsis-template.cmake"
DRIVER_NSH = PACKAGING / "windows" / "driver.nsh"
PACKAGE_CMAKE = PACKAGING / "VssPackage-windows.cmake"
CMAKE = shutil.which("cmake")

OLD_LINE = "  ExecWait '\"$0\" /S _?=$3' ;Do not copy the uninstaller to a temp file"


def code_lines(path: Path) -> list[str]:
    """The lines of an NSIS file that are not comments."""
    return [line for line in path.read_text(encoding="utf-8").splitlines() if not line.lstrip().startswith(";")]


def write_template(cmake_root: str, out_dir: Path) -> subprocess.CompletedProcess[str]:
    script = out_dir / "run.cmake"
    script.write_text(
        f'set(CMAKE_ROOT "{cmake_root}")\n'
        f'include("{TEMPLATE_CMAKE.as_posix()}")\n'
        f'vss_write_nsis_template("{(out_dir / "out").as_posix()}")\n',
        encoding="utf-8",
    )
    return subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True, timeout=60, check=False)


@unittest.skipUnless(CMAKE, "needs cmake")
class NsisTemplate(unittest.TestCase):
    def cmake_root(self) -> str:
        result = subprocess.run([CMAKE, "--system-information"], capture_output=True, text=True, timeout=120, check=False)
        match = re.search(r'^CMAKE_ROOT "(.*)"$', result.stdout, re.MULTILINE)
        self.assertIsNotNone(match, result.stdout[:500])
        return match.group(1)

    def test_the_old_uninstaller_is_started_with_the_upgrade_switch_before_the_folder(self) -> None:
        stock = Path(self.cmake_root()) / "Modules" / "Internal" / "CPack" / "NSIS.template.in"
        if not stock.exists():
            self.skipTest("this CMake has no NSIS template")
        with tempfile.TemporaryDirectory() as tmp:
            result = write_template(self.cmake_root(), Path(tmp))
            self.assertEqual(result.returncode, 0, result.stderr)
            made = (Path(tmp) / "out" / "NSIS.template.in").read_text(encoding="utf-8")
        original = stock.read_text(encoding="utf-8")
        # The one command that starts an uninstaller names the switch, and _?= stays last.
        self.assertEqual(made.count("/VSSUPGRADE=1"), 1)
        self.assertIn("ExecWait '\"$0\" /S /VSSUPGRADE=1 _?=$3'", made)
        self.assertNotIn("/S _?=$3", made)
        # The folder is remembered before the uninstaller runs, for the clean-up.
        self.assertLess(made.index("StrCpy $VssUpgradeFolder $3"), made.index("/VSSUPGRADE=1"))
        # Nothing else changed: undo the one replacement and get CMake's own file back.
        undone = made.replace(
            "  StrCpy $VssUpgradeFolder $3\n  ExecWait '\"$0\" /S /VSSUPGRADE=1 _?=$3' ;Do not copy the uninstaller to a temp file",
            OLD_LINE,
        )
        self.assertEqual(undone, original)

    def test_a_template_without_the_line_stops_the_configure_step(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "root"
            target = fake / "Modules" / "Internal" / "CPack"
            target.mkdir(parents=True)
            (target / "NSIS.template.in").write_text("Function .onInit\nFunctionEnd\n", encoding="utf-8")
            result = write_template(fake.as_posix(), Path(tmp))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("runs the earlier version's uninstaller", result.stderr)
            self.assertFalse((Path(tmp) / "out" / "NSIS.template.in").exists())

    def test_the_package_settings_use_the_copy(self) -> None:
        text = PACKAGE_CMAKE.read_text(encoding="utf-8")
        self.assertIn("vss_write_nsis_template(", text)
        self.assertIn("set(CPACK_MODULE_PATH \"${VSS_NSIS_TEMPLATE_DIR}\")", text)


class UninstallerScript(unittest.TestCase):
    def test_no_uninstall_decision_reads_the_folder_the_uninstaller_runs_from(self) -> None:
        # $EXEDIR == $INSTDIR is true for every `Uninstall.exe /S _?=<folder>`, not only an upgrade.
        offenders = [line for line in code_lines(DRIVER_NSH) if "$EXEDIR" in line]
        self.assertEqual(offenders, [])

    def test_the_upgrade_is_read_from_the_switch_and_nothing_else(self) -> None:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        self.assertIn('${un.GetOptions} $0 "/VSSUPGRADE=" $1', text)
        # The three things an upgrade keeps, and the question asked before Studio is closed.
        for name in ("un.VssRemoveDriver", "un.VssRemovePath", "un.VssForgetUsers", "un.VssStopStudio"):
            body = text.split(f"Function {name}\n", 1)[1].split("FunctionEnd", 1)[0]
            self.assertRegex(body, r"\$VssIsUpgrade (==|!=) 1", name)

    def test_the_uninstall_commands_read_the_switch_first_and_stash_before_anything_goes(self) -> None:
        text = PACKAGE_CMAKE.read_text(encoding="utf-8")
        block = text.split("set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS", 1)[1].split('")', 1)[0]
        calls = re.findall(r"Call (un\.\w+)", block)
        self.assertEqual(
            calls,
            ["un.VssReadUpgrade", "un.VssStopStudio", "un.VssStashUndo", "un.VssForgetUsers", "un.VssRemovePath", "un.VssRemoveDriver"],
        )

    def test_the_path_log_is_not_deleted_by_every_uninstall(self) -> None:
        # It is what the message after a failed PATH removal tells the person to read.
        text = PACKAGE_CMAKE.read_text(encoding="utf-8")
        block = text.split("set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS", 1)[1].split('")', 1)[0]
        self.assertNotIn("path-setup.log", block)
        body = DRIVER_NSH.read_text(encoding="utf-8").split("Function un.VssRemovePath\n", 1)[1].split("FunctionEnd", 1)[0]
        failed = body.split("${Else}", 1)[1].split("${EndIf}", 1)[0]
        self.assertIn("MessageBox", failed)
        self.assertIn("Goto done", failed)
        self.assertNotIn("Delete", failed)
        # Every other way out of the function deletes it.
        self.assertIn('clean:\n  Delete "$INSTDIR\\path-setup.log"', body)
        self.assertLess(body.index("clean:"), body.index("done:"))

    def test_a_cancelled_upgrade_is_taken_back(self) -> None:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        package = PACKAGE_CMAKE.read_text(encoding="utf-8")
        self.assertIn("!define MUI_CUSTOMFUNCTION_ABORT VssOnUserAbort", package)
        self.assertIn("Call VssUpgradeDone", package)
        self.assertRegex(text, r"Function \.onInstFailed\n  Call VssUndoUpgrade\nFunctionEnd")
        self.assertRegex(text, r"Function VssOnUserAbort\n  Call VssUndoUpgrade\nFunctionEnd")


if __name__ == "__main__":
    unittest.main()
