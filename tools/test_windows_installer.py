#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks on how the Windows installer tells an upgrade from an uninstall.

    python3 -m unittest discover -s tools -p "test_windows_installer.py" -v

Nothing here runs an installer. The installer runs the uninstaller of the version it
replaces, and that uninstaller keeps the driver, the helper service and the PATH entry
only when it is told it is part of an upgrade. These tests check the text that carries
that message (cmake/packaging/windows/nsis-template.cmake and driver.nsh). The installer
check in CI (tools/ci/check-windows-installer.ps1) runs the result: a finished upgrade, an
uninstall with the switch and the clean-up scripts it leaves, an uninstall with _?= and
without the switch, and a normal uninstall. A cancelled upgrade, with the wizard on screen,
is only run by hand on a guest machine (docs/releasing.md).
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
MAKENSIS = shutil.which("makensis") or next((str(p) for p in (Path("C:/Program Files (x86)/NSIS/Bin/makensis.exe"),) if p.exists()), None)

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
        # cmake -P reads the variable without configuring a compiler project.
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "root.cmake"
            script.write_text('message("${CMAKE_ROOT}")\n', encoding="utf-8")
            result = subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        root = result.stderr.strip() or result.stdout.strip()
        self.assertTrue(root, "cmake -P printed no CMAKE_ROOT")
        return root

    def stock_template(self) -> Path:
        stock = Path(self.cmake_root()) / "Modules" / "Internal" / "CPack" / "NSIS.template.in"
        if not stock.exists():
            self.skipTest("this CMake has no NSIS template")
        return stock

    def test_the_old_uninstaller_is_started_with_the_upgrade_switch_before_the_folder(self) -> None:
        stock = self.stock_template()
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
        # An unfinished upgrade is picked up at the label every path of .onInit reaches.
        self.assertEqual(made.count("Call VssAdoptUnfinishedUpgrade"), 1)
        # The marker that driver.nsh requires comes before the line that carries the defines.
        self.assertLess(made.index("!define VSS_TEMPLATE_PASSES_UPGRADE_SWITCH"), made.index("@CPACK_NSIS_DEFINES@"))
        # Nothing else changed: undo the three replacements and get CMake's own file back.
        undone = (
            made.replace(
                "  StrCpy $VssUpgradeFolder $3\n  ExecWait '\"$0\" /S /VSSUPGRADE=1 _?=$3' ;Do not copy the uninstaller to a temp file",
                OLD_LINE,
            )
            .replace("\ninst:\n  Call VssAdoptUnfinishedUpgrade\n", "\ninst:\n")
            .replace("\n!define VSS_TEMPLATE_PASSES_UPGRADE_SWITCH\n@CPACK_NSIS_DEFINES@\n", "\n@CPACK_NSIS_DEFINES@\n")
        )
        self.assertEqual(undone, original)

    def fake_root(self, tmp: Path, text: str) -> Path:
        fake = tmp / "root"
        target = fake / "Modules" / "Internal" / "CPack"
        target.mkdir(parents=True)
        (target / "NSIS.template.in").write_text(text, encoding="utf-8")
        return fake

    def test_a_template_without_one_of_the_lines_stops_the_configure_step(self) -> None:
        lines = {
            "runs the earlier version's uninstaller": OLD_LINE,
            "label that ends the uninstall": "\ninst:\n  ; Reads components status for registry\n",
            "holds CPACK_NSIS_DEFINES": "\n@CPACK_NSIS_DEFINES@\n",
        }
        whole = "".join(lines.values())
        for message, line in lines.items():
            with self.subTest(missing=message), tempfile.TemporaryDirectory() as tmp:
                result = write_template(self.fake_root(Path(tmp), whole.replace(line, "\n")).as_posix(), Path(tmp))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, " ".join(result.stderr.split()))
                self.assertFalse((Path(tmp) / "out" / "NSIS.template.in").exists())

    def test_a_template_with_one_of_the_lines_twice_stops_the_configure_step(self) -> None:
        whole = OLD_LINE + "\ninst:\n  ; Reads components status for registry\n\n@CPACK_NSIS_DEFINES@\n"
        for line in (OLD_LINE, "\n@CPACK_NSIS_DEFINES@\n"):
            with self.subTest(line=line.strip()), tempfile.TemporaryDirectory() as tmp:
                result = write_template(self.fake_root(Path(tmp), whole + line).as_posix(), Path(tmp))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("more than once", " ".join(result.stderr.split()))

    def test_the_package_settings_use_the_copy(self) -> None:
        text = PACKAGE_CMAKE.read_text(encoding="utf-8")
        self.assertIn("vss_write_nsis_template(", text)
        self.assertIn('set(CPACK_MODULE_PATH "${VSS_NSIS_TEMPLATE_DIR}")', text)


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

    def test_an_installer_not_built_from_the_copy_is_an_error(self) -> None:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        match = re.search(r"^!ifndef VSS_TEMPLATE_PASSES_UPGRADE_SWITCH\n  !error .*\n!endif$", text, re.MULTILINE)
        self.assertIsNotNone(match)
        # Before anything that could be built without the switch.
        self.assertLess(match.start(), text.index("!include"))

    def undo_body(self) -> str:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        return text.split("Function VssUndoUpgrade\n", 1)[1].split("FunctionEnd", 1)[0]

    def test_the_undo_runs_each_script_that_is_there_and_leaves_only_without_a_stash(self) -> None:
        body = self.undo_body()
        # The stash folder decides whether to leave; a missing remove-driver.ps1 does not.
        self.assertIn('${IfNot} ${FileExists} "$R1\\*.*"\n    Goto done', body)
        for script in ("remove-driver.ps1", "edit-path.ps1", "stop-studio.ps1"):
            self.assertIn('${If} ${FileExists} "$R1\\' + script + '"\n    ExecWait', body, script)
        self.assertNotIn('${IfNot} ${FileExists} "$R1\\remove-driver.ps1"', body)

    def test_the_undo_deletes_the_uninstaller_that_could_not_delete_itself(self) -> None:
        body = self.undo_body()
        success = body.split("${If} $1 == 0", 1)[1].split("${Else}", 1)[0]
        self.assertIn('Delete "$R0\\Uninstall.exe"', success)
        self.assertLess(success.index("Uninstall.exe"), success.index('RMDir "$R0"'))

    def test_every_exit_code_of_remove_driver_but_0_and_10_is_shown_and_does_not_stop_the_uninstall(self) -> None:
        # remove-driver.ps1 ends with 7 (a port device or the package stays) or 8 (the helper service stays)
        # after it has done the rest, so the uninstaller must report any other code and carry on deleting files.
        text = DRIVER_NSH.read_text(encoding="utf-8")
        body = text.split("Function un.VssRemoveDriver\n", 1)[1].split("FunctionEnd", 1)[0]
        success, failure = body.split("${Else}", 1)
        self.assertIn("${If} $0 == 0\n  ${OrIf} $0 == 10", success)
        self.assertNotIn("$0 == 7", body)
        self.assertNotIn("$0 == 8", body)
        self.assertIn("MessageBox MB_OK|MB_ICONEXCLAMATION", failure)
        self.assertIn("(code $0)", failure)
        self.assertIn("helper service", failure)
        self.assertIn("$VssDriverLog", failure)
        self.assertNotIn('Delete "$VssDriverLog"', failure)
        self.assertNotIn("Abort", body)
        self.assertNotIn("Quit", body)

    def test_an_unfinished_upgrade_is_picked_up_at_the_start_of_an_install(self) -> None:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        body = text.split("Function VssAdoptUnfinishedUpgrade\n", 1)[1].split("FunctionEnd", 1)[0]
        self.assertIn('${If} $VssUpgradeFolder == ""', body)
        self.assertIn('${FileExists} "$INSTDIR\\${VSS_UNDO_DIR}\\*.*"', body)
        self.assertIn("StrCpy $VssUpgradeFolder $INSTDIR", body)

    def test_a_cancelled_upgrade_is_taken_back(self) -> None:
        text = DRIVER_NSH.read_text(encoding="utf-8")
        package = PACKAGE_CMAKE.read_text(encoding="utf-8")
        self.assertIn("!define MUI_CUSTOMFUNCTION_ABORT VssOnUserAbort", package)
        self.assertIn("Call VssUpgradeDone", package)
        self.assertRegex(text, r"Function \.onInstFailed\n  Call VssUndoUpgrade\nFunctionEnd")
        self.assertRegex(text, r"Function VssOnUserAbort\n  Call VssUndoUpgrade\nFunctionEnd")



def function_body(text: str, name: str) -> str:
    return text.split(f"Function {name}\n", 1)[1].split("\nFunctionEnd", 1)[0]


class DriverVersionPage(unittest.TestCase):
    """The page and the silent install decide by the version of the driver package (driver.nsh)."""

    def setUp(self) -> None:
        self.text = DRIVER_NSH.read_text(encoding="utf-8")

    def test_the_cmake_file_hands_the_installer_the_bundled_version_and_the_script(self) -> None:
        package = PACKAGE_CMAKE.read_text(encoding="utf-8")
        self.assertIn("!define VSS_BUNDLED_DRIVER_VER", package)
        self.assertIn("!define VSS_DRIVER_QUERY_SCRIPT", package)
        self.assertIn("drivers/windows/vsscom_um/vsscom_um.inf", package)

    def test_the_query_runs_the_script_that_changes_nothing_in_the_same_way_as_the_others(self) -> None:
        body = function_body(self.text, "VssDriverQuery")
        self.assertIn("-Query", body)
        self.assertIn('-BundledVer "${VSS_BUNDLED_DRIVER_VER}"', body)
        self.assertIn('"$WINDIR\\Sysnative\\cmd.exe" /C set "PSModulePath="', body)
        self.assertIn("-NoProfile -ExecutionPolicy Bypass -File", body)
        # It reads the script out of the installer: the install folder is empty when the page is shown.
        self.assertIn('File "/oname=$PLUGINSDIR\\setup-driver.ps1" "${VSS_DRIVER_QUERY_SCRIPT}"', body)
        self.assertNotIn("$INSTDIR", body)
        self.assertNotIn("$EXEDIR", body)
        # Whatever goes wrong, the state is a word the rest understands.
        self.assertIn('StrCpy $VssDriverState "unknown"', body)

    def test_each_state_has_its_own_text_and_the_box_that_belongs_to_it(self) -> None:
        body = function_body(self.text, "VssDriverPageCreate")
        for state in ("same", "newer", "older"):
            self.assertIn(f'$VssDriverState == "{state}"', body)
        self.assertIn("Install the driver again (repair)", body)
        self.assertIn("Update the virtual COM port driver and its helper service", body)
        # The repair box is not ticked unless /REPAIRDRIVER=1 asked for it, and the newer state has no box at all.
        repair = body.split('"Install the driver again (repair)"', 1)[1].split("${NSD_CreateLabel}", 1)[0]
        self.assertIn("${If} $VssDriverRepair == 1", repair)
        newer = body.split('$VssDriverState == "newer"', 1)[1].split("${ElseIf}", 1)[0]
        self.assertNotIn("CreateCheckbox", newer)
        self.assertIn("is newer than the one in this installer", newer)
        self.assertIn("left alone", newer)

    def test_leaving_the_page_reads_the_box_that_the_state_made(self) -> None:
        body = function_body(self.text, "VssDriverPageLeave")
        self.assertIn('${If} $VssDriverCheckbox == ""', body)
        # In the same state the box is the repair and not the choice to install.
        same = body.split('$VssDriverState == "same"', 1)[1].split("${ElseIf}", 1)[0]
        self.assertIn("StrCpy $VssDriverRepair 1", same)
        self.assertNotIn("VssDriverWanted", same)

    def test_a_silent_install_installs_again_only_when_asked(self) -> None:
        switch = function_body(self.text, "VssDriverSwitch")
        self.assertIn('"/DRIVER="', switch)
        self.assertIn('"/REPAIRDRIVER="', switch)
        self.assertRegex(switch, r'\$1 == "1"\n    StrCpy \$VssDriverRepair 1\n  \$\{Else\}\n    StrCpy \$VssDriverRepair 0')
        install = function_body(self.text, "VssInstallDriver")
        self.assertRegex(install, r'\$\{If\} \$VssDriverRepair == 1\n    StrCpy \$VssDriverArgs " -ReinstallDriver"')
        self.assertIn('!insertmacro VssRunScriptArgs "setup-driver.ps1"', install)
        # Leaving a same or newer driver alone is the script's own decision, so that Studio's set-up and a run
        # by hand agree with the installer.
        self.assertEqual(self.text.count("-ReinstallDriver"), 1)

    def test_the_installer_says_what_it_does_with_the_driver_it_found(self) -> None:
        install = function_body(self.text, "VssInstallDriver")
        for phrase in ("no Studio driver is installed yet", "updating it to version", "leaving it as it is",
                       "installing it again", "leaving it alone", "could not be read"):
            self.assertIn(phrase, install)

    def test_the_skip_switch_still_skips_everything_and_a_missing_script_is_still_reported(self) -> None:
        install = function_body(self.text, "VssInstallDriver")
        self.assertLess(install.index('"skipped"'), install.index("Call VssDriverQuery"))
        self.assertLess(install.index("failed-9"), install.index("Call VssDriverQuery"))

    def test_the_uninstaller_says_that_every_version_goes(self) -> None:
        body = function_body(self.text, "un.VssRemoveDriver")
        self.assertIn("every Virtual Serial Studio driver package in the driver store", body)
        self.assertIn("older versions", body)
        # An upgrade still keeps the driver, whatever its version.
        self.assertIn("Upgrade: the driver, the helper service and the ports stay in place.", body)

    def test_no_message_box_waits_for_an_answer_in_a_silent_install(self) -> None:
        # The installer that hung for ever on a build agent (the silent install hang) did so on a question nobody
        # could answer. Every box here has its answer for /S.
        for line in code_lines(DRIVER_NSH):
            if "MessageBox" in line:
                self.assertIn("/SD", line, line)

    @unittest.skipUnless(MAKENSIS, "needs makensis")
    def test_driver_nsh_compiles_with_the_new_page(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "t.nsi"
            script.write_text(
                "Unicode true\n"
                "!define VSS_REGISTRY_KEY 'k'\n"
                "!define MUI_CUSTOMFUNCTION_ABORT VssOnUserAbort\n"
                "!define VSS_BUNDLED_DRIVER_VER '11/18/2026,1.5.1.0'\n"
                f"!define VSS_DRIVER_QUERY_SCRIPT '{ROOT / 'drivers' / 'windows' / 'setup-driver.ps1'}'\n"
                "!include MUI2.nsh\n"
                "!define SHCTX HKLM\n"
                "Name t\n"
                f'OutFile "{Path(directory) / "t.exe"}"\n'
                "RequestExecutionLevel user\n"
                "!define VSS_TEMPLATE_PASSES_UPGRADE_SWITCH\n"
                f"!include '{DRIVER_NSH}'\n"
                "Page custom VssDriverPageCreate VssDriverPageLeave\n"
                "!insertmacro MUI_PAGE_INSTFILES\n"
                '!insertmacro MUI_LANGUAGE "English"\n'
                "Section\n  Call VssInstallDriver\nSectionEnd\n"
                "Section Uninstall\n  Call un.VssRemoveDriver\nSectionEnd\n",
                encoding="utf-8",
            )
            result = subprocess.run([MAKENSIS, str(script)], capture_output=True, text=True, timeout=120, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
