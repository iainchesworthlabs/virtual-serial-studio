#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the Windows driver set-up scripts.

    python -m unittest discover -s tools -p "test_driver_scripts.py" -v

The scripts are drivers/windows/setup-driver.ps1, remove-driver.ps1, vsscom_helper/install-helper.ps1 and
trust/install-trust.ps1. They are run under Windows PowerShell 5.1, the one the installer and Studio start, and are
skipped elsewhere.

Nothing real is touched. Each test writes a small harness script that defines functions with the names of every command
the script under test calls (sc.exe, pnputil, icacls, New-Service, Get-Service, Get-PnpDevice, Get-WindowsDriver,
Get-AuthenticodeSignature and so on; a function wins over a program of the same name), then runs the script in the same process. The
stubs write what they were asked to a log, and the tests read the log and the exit code. A command that has no stub
for the test in hand fails the test and runs nothing. The one place a script touches a certificate store is
install-trust.ps1, which the tests point at stores of their own name under HKEY_CURRENT_USER (-StoreLocation and the
store-name parameters), deleted afterwards.
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

ROOT = Path(__file__).resolve().parent.parent
DRIVERS = ROOT / "drivers" / "windows"
SETUP = DRIVERS / "setup-driver.ps1"
REMOVE = DRIVERS / "remove-driver.ps1"
HELPER = DRIVERS / "vsscom_helper" / "install-helper.ps1"
TRUST = DRIVERS / "trust" / "install-trust.ps1"
POWERSHELL = shutil.which("powershell") if sys.platform == "win32" else None

# Defined first in every harness. Whatever a test does not stub cannot reach the machine.
PRELUDE = r"""
$ErrorActionPreference = 'Stop'
function Log([string] $Text) { Add-Content -LiteralPath $env:VSS_TEST_LOG -Value $Text -Encoding UTF8 }
$Scenario = Get-Content -LiteralPath $env:VSS_TEST_SCENARIO -Raw | ConvertFrom-Json
foreach ($name in 'sc.exe', 'pnputil.exe', 'certutil.exe', 'bcdedit.exe', 'icacls.exe', 'taskkill.exe', 'devcon.exe',
                  'New-Service', 'Set-Service', 'Remove-Service', 'Stop-Service', 'Start-Service', 'Restart-Service',
                  'Remove-PnpDevice', 'Add-WindowsDriver', 'Remove-WindowsDriver', 'Import-Certificate', 'Set-ItemProperty',
                  'New-ItemProperty', 'Remove-ItemProperty') {
    $text = "Log 'UNSTUBBED $name'; throw 'a command that has no stub in this test was called: $name'"
    Set-Item -Path "function:global:$name" -Value ([scriptblock]::Create($text))
}
"""

# What the helper service looks like to install-helper.ps1: a service that exists or not, a path stored for it, and
# a clock that moves a second at every look so that a wait ends. sc.exe strips the quotes of binPath, as Windows
# PowerShell 5.1 makes it do.
HELPER_STUBS = r"""
$global:HelperState = $null
$global:HelperPath = $null
if ($Scenario.service_exists) { $global:HelperState = 'Running'; $global:HelperPath = $Scenario.service_path }
$global:Clock = [datetime]'2026-01-01'
function Get-Date { $global:Clock = $global:Clock.AddSeconds(1); $global:Clock }
function Start-Sleep { }
function Get-Service {
    [CmdletBinding()] param([string] $Name)
    if ($global:HelperState) { [pscustomobject]@{ Name = 'vsscom_helper'; Status = $global:HelperState } }
}
function sc.exe {
    Log ('sc.exe ' + ($args -join ' '))
    $global:LASTEXITCODE = 0
    switch ($args[0]) {
        'stop' { if ($global:HelperState) { $global:HelperState = 'Stopped' } }
        'delete' { $global:HelperState = $null }
        'create' { $global:HelperState = 'Stopped'; $global:HelperPath = ([string] $args[2]).Trim('"') }
        'start' { if (-not $Scenario.start_fails) { $global:HelperState = 'Running' } }
    }
}
function icacls.exe { Log ('icacls.exe ' + ($args -join ' ')); $global:LASTEXITCODE = [int] $Scenario.icacls_code }
function New-Service {
    [CmdletBinding()] param($Name, $BinaryPathName, $DisplayName, $StartupType, $Description)
    Log "New-Service $Name BinaryPathName=[$BinaryPathName] StartupType=$StartupType"
    $global:HelperState = 'Stopped'
    $global:HelperPath = if ($Scenario.strip_quotes) { ([string] $BinaryPathName).Trim('"') } else { $BinaryPathName }
}
function Get-CimInstance { [CmdletBinding()] param($ClassName, $Filter) [pscustomobject]@{ PathName = $global:HelperPath } }
"""

DRIVER_STORE = "C:\\Windows\\System32\\DriverStore\\FileRepository\\"
INF_TEXT = "[Version]\nSignature=\"$WINDOWS NT$\"\nDriverVer=11/12/2026,1.1.0.0\n"


def ps_quote(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


class Run:
    def __init__(self, returncode: int, stdout: str, stderr: str, log: list[str]) -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.log = log

    def calls(self, prefix: str) -> list[str]:
        return [line for line in self.log if line.startswith(prefix)]


class Harness(unittest.TestCase):
    """Runs a script in Windows PowerShell with the stubs of a test."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="vss-driver-scripts-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # A scratch Program Files, so that nothing can reach the real one.
        self.programs = self.tmp / "Program Files"
        self.programs.mkdir()

    def run_harness(self, stubs: str, script: Path, arguments: str, scenario: dict, catch: bool = False) -> Run:
        name = uuid.uuid4().hex[:8]
        log = self.tmp / f"{name}.log"
        scenario_file = self.tmp / f"{name}.json"
        scenario_file.write_text(json.dumps(scenario), encoding="utf-8")
        harness = self.tmp / f"{name}.ps1"
        invoke = f"& {ps_quote(str(script))} {arguments}"
        # With catch, a throw out of the script is logged and gives 77, as the caller of a script would see it.
        call = f"try {{ {invoke} }} catch {{ Log 'THREW'; exit 77 }}" if catch else invoke
        harness.write_text(
            PRELUDE + stubs + "\n$global:LASTEXITCODE = 99\n" + call + "\nexit $LASTEXITCODE\n",
            encoding="utf-8",
        )
        env = {
            **os.environ,
            "VSS_TEST_LOG": str(log),
            "VSS_TEST_SCENARIO": str(scenario_file),
            "ProgramW6432": str(self.programs),
            "ProgramFiles": str(self.programs),
        }
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(harness)],
            env=env,
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
        lines = log.read_text(encoding="utf-8-sig").splitlines() if log.exists() else []
        self.assertFalse([line for line in lines if line.startswith("UNSTUBBED")], result.stderr)
        return Run(result.returncode, result.stdout, result.stderr, lines)


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class InstallHelper(Harness):
    STUBS = HELPER_STUBS + "function Test-IsAdministrator { [bool] $Scenario.admin }\n"

    def setUp(self) -> None:
        super().setUp()
        self.exe = self.tmp / "vsscom_helper.exe"
        self.exe.write_bytes(b"MZ not a program")
        self.install_dir = self.programs / "Virtual Serial Studio" / "Helper"

    def install(self, **scenario: object) -> Run:
        scenario = {"admin": True, "icacls_code": 0, **scenario}
        return self.run_harness(
            self.STUBS,
            HELPER,
            f"-ExePath {ps_quote(str(self.exe))} -InstallDir {ps_quote(str(self.install_dir))}",
            scenario,
            catch=True,
        )

    def test_the_service_is_registered_with_its_path_in_quotes(self) -> None:
        run = self.install()
        self.assertEqual(run.returncode, 0, run.stderr)
        target = self.install_dir / "vsscom_helper.exe"
        self.assertTrue(target.exists())
        # The exact string that reaches the service manager: the folder has a space in it.
        self.assertIn(" ", str(target))
        self.assertEqual(
            run.calls("New-Service"), [f'New-Service vsscom_helper BinaryPathName=["{target}"] StartupType=Automatic']
        )
        self.assertEqual(run.calls("sc.exe create"), [], "the path must not go through sc.exe")

    def test_a_service_that_has_the_path_without_quotes_is_replaced(self) -> None:
        old_path = str(self.install_dir / "vsscom_helper.exe")
        run = self.install(service_exists=True, service_path=old_path)
        self.assertEqual(run.returncode, 0, run.stderr)
        sc = run.calls("sc.exe") + run.calls("New-Service")
        self.assertLess(sc.index("sc.exe stop vsscom_helper"), sc.index("sc.exe delete vsscom_helper"))
        self.assertLess(
            sc.index("sc.exe delete vsscom_helper"),
            next(i for i, line in enumerate(sc) if line.startswith("New-Service")),
        )
        self.assertIn(f'BinaryPathName=["{old_path}"]', run.calls("New-Service")[0])

    def test_a_path_that_is_stored_without_quotes_is_an_error_and_the_service_is_taken_out(self) -> None:
        run = self.install(strip_quotes=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn("not in quotes", run.stderr)
        self.assertEqual(run.calls("sc.exe start"), [])
        self.assertEqual(run.calls("sc.exe delete")[-1], "sc.exe delete vsscom_helper")

    def test_not_an_administrator_ends_with_2(self) -> None:
        run = self.install(admin=False)
        self.assertEqual(run.returncode, 2, run.stderr)
        self.assertIn("administrator", run.stderr)
        self.assertEqual(run.log, [])

    def test_a_failure_ends_with_1_and_says_why(self) -> None:
        run = self.install(icacls_code=5)
        self.assertEqual(run.returncode, 1)
        self.assertIn("icacls failed", run.stderr)
        self.assertNotIn("THREW", run.log)  # an exit code, not an exception, for the caller to read

    def test_a_service_that_does_not_start_ends_with_1(self) -> None:
        run = self.install(start_fails=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn("did not start", run.stderr)

    def test_uninstall_removes_the_service_and_the_folder(self) -> None:
        self.install_dir.mkdir(parents=True)
        (self.install_dir / "vsscom_helper.exe").write_bytes(b"x")
        scenario = {"admin": True, "service_exists": True, "service_path": "x"}
        run = self.run_harness(self.STUBS, HELPER, f"-Uninstall -InstallDir {ps_quote(str(self.install_dir))}", scenario)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertFalse(self.install_dir.exists())
        self.assertIn("sc.exe delete vsscom_helper", run.log)


def write_package(root: Path, helper: str) -> Path:
    """A package folder as setup-driver.ps1 reads it. `helper` is the text of helper\\install-helper.ps1."""
    (root / "driver").mkdir(parents=True)
    (root / "helper").mkdir()
    (root / "driver" / "vsscom_um.inf").write_text(INF_TEXT, encoding="ascii")
    for name in ("driver\\vsscom_um.dll", "driver\\vsscom_um.cat", "helper\\vsscom_helper.exe"):
        (root / name).write_bytes(b"x")
    (root / "helper" / "install-helper.ps1").write_text(helper, encoding="ascii")
    return root


SETUP_STUBS = (
    HELPER_STUBS
    + r"""
function Test-IsAdministrator { [bool] $Scenario.admin }
function Invoke-Bounded {
    param([string] $File, [string[]] $Arguments, [int] $Seconds)
    Log ('run ' + $File + ' ' + ($Arguments -join ' '))
    [int] $Scenario.add_code
}
function bcdedit.exe {
    $global:LASTEXITCODE = 0
    if ($Scenario.testsigning) { '    testsigning             Yes' }
}
function Get-AuthenticodeSignature {
    param($LiteralPath)
    $leaf = Split-Path -Leaf $LiteralPath
    $entry = $Scenario.signatures.$leaf
    if (-not $entry) { $entry = $Scenario.signatures.default }
    $signer = if ($entry.thumbprint) { [pscustomobject]@{ Thumbprint = $entry.thumbprint; Subject = $entry.subject } }
    [pscustomobject]@{ Status = $entry.status; SignerCertificate = $signer }
}
function Test-Path {
    param([string] $LiteralPath, [string] $Path, [string] $PathType)
    $where = if ($LiteralPath) { $LiteralPath } else { $Path }
    if ($where -like 'Cert:*') { return (@($Scenario.trusted_publishers) -contains $where.Split('\')[-1]) }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
function Get-WindowsDriver {
    param([switch] $Online, [switch] $All)
    foreach ($p in @($Scenario.store)) {
        [pscustomobject]@{ OriginalFileName = $p.original; Version = [version] $p.version; Driver = 'oem1.inf' }
    }
}
"""
)

PUBLISHER = {"status": "Valid", "thumbprint": "AAAA", "subject": "CN=Some Publisher"}
HARDWARE = {"status": "Valid", "thumbprint": "BBBB", "subject": "CN=Microsoft Windows Hardware Compatibility Publisher, O=Microsoft Corporation"}


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class SetupDriver(Harness):
    def setup(self, helper: str = "exit 0", **scenario: object) -> Run:
        package = write_package(self.tmp / "package", helper) if not (self.tmp / "package").exists() else self.tmp / "package"
        base: dict = {
            "admin": True,
            "add_code": 0,
            "testsigning": False,
            "signatures": {"default": {"status": "Valid", "thumbprint": "FFFF", "subject": "CN=Any"}, "vsscom_um.cat": PUBLISHER},
            "trusted_publishers": ["AAAA"],
            "store": [],
            "icacls_code": 0,
        }
        base.update(scenario)
        return self.run_harness(SETUP_STUBS, SETUP, f"-PackageRoot {ps_quote(str(package))}", base)

    def with_signature(self, cat: dict, **scenario: object) -> Run:
        signatures = {"default": {"status": "Valid", "thumbprint": "FFFF", "subject": "CN=Any"}, "vsscom_um.cat": cat}
        return self.setup(signatures=signatures, **scenario)

    # DRV-10: the answer to "does Windows ask whether to install?" is decided before pnputil is started.
    def test_a_valid_signature_whose_publisher_is_not_trusted_ends_with_6_and_never_starts_pnputil(self) -> None:
        run = self.with_signature(PUBLISHER, trusted_publishers=[])
        self.assertEqual(run.returncode, 6, run.stdout + run.stderr)
        self.assertIn("Trusted Publishers", run.stderr)
        self.assertEqual(run.calls("run pnputil"), [])

    def test_a_valid_signature_whose_publisher_is_trusted_installs(self) -> None:
        run = self.with_signature(PUBLISHER)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(len(run.calls("run pnputil.exe /add-driver")), 1)

    def test_a_package_signed_by_the_microsoft_hardware_publisher_installs_without_trusted_publishers(self) -> None:
        run = self.with_signature(HARDWARE, trusted_publishers=[])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(len(run.calls("run pnputil.exe /add-driver")), 1)

    def test_the_microsoft_name_on_a_signature_that_is_not_valid_does_not_count(self) -> None:
        forged = {**HARDWARE, "status": "NotTrusted"}
        run = self.with_signature(forged, trusted_publishers=[], testsigning=True)
        self.assertEqual(run.returncode, 6, run.stdout + run.stderr)
        self.assertEqual(run.calls("run pnputil"), [])

    def test_test_signing_with_a_signer_that_is_not_trusted_ends_with_6(self) -> None:
        run = self.with_signature({**PUBLISHER, "status": "NotTrusted"}, trusted_publishers=[], testsigning=True)
        self.assertEqual(run.returncode, 6, run.stdout + run.stderr)
        self.assertEqual(run.calls("run pnputil"), [])

    def test_test_signing_with_a_signer_that_is_trusted_installs(self) -> None:
        run = self.with_signature({**PUBLISHER, "status": "NotTrusted"}, testsigning=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_an_untrusted_package_without_test_signing_ends_with_6(self) -> None:
        run = self.with_signature({**PUBLISHER, "status": "NotTrusted"})
        self.assertEqual(run.returncode, 6, run.stdout + run.stderr)
        self.assertEqual(run.calls("run pnputil"), [])

    # DRV-11: pnputil answers 259 when no device was updated.
    def test_pnputil_259_is_a_success_when_the_package_is_in_the_driver_store(self) -> None:
        store = [{"original": "vsscom_um.inf", "version": "1.1.0.0"}]
        run = self.setup(add_code=259, store=store)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("installing the helper service", run.stdout)

    def test_pnputil_259_is_a_failure_when_the_package_is_not_in_the_driver_store(self) -> None:
        run = self.setup(add_code=259, store=[])
        self.assertEqual(run.returncode, 7, run.stdout + run.stderr)
        self.assertNotIn("installing the helper service", run.stdout)

    def test_pnputil_259_is_a_failure_when_the_store_holds_another_version(self) -> None:
        store = [{"original": "vsscom_um.inf", "version": "1.0.0.0"}, {"original": "other.inf", "version": "1.1.0.0"}]
        run = self.setup(add_code=259, store=store)
        self.assertEqual(run.returncode, 7, run.stdout + run.stderr)

    def test_other_pnputil_codes_still_fail_and_3010_asks_for_a_restart(self) -> None:
        self.assertEqual(self.setup(add_code=5).returncode, 7)
        self.assertEqual(self.setup(add_code=3010).returncode, 10)
        self.assertEqual(self.setup(add_code=0x800B0109 - (1 << 32)).returncode, 6)

    # DRV-07: every exit code is one the script can reach.
    def test_a_helper_script_that_exits_with_an_error_ends_with_8(self) -> None:
        run = self.setup(helper="exit 3")
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)

    def test_a_helper_script_that_throws_ends_with_8(self) -> None:
        run = self.setup(helper="throw 'the helper broke'")
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)
        self.assertIn("the helper broke", run.stderr)

    def test_a_helper_script_that_writes_an_error_under_stop_ends_with_8(self) -> None:
        run = self.setup(helper="$ErrorActionPreference = 'Stop'\nWrite-Error 'no'\nexit 1")
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)

    def test_the_real_helper_script_failing_ends_with_8_not_1(self) -> None:
        # The case of the finding: the service did not install, so the driver part had worked and the answer is 8.
        package = write_package(self.tmp / "package", HELPER.read_text(encoding="utf-8"))
        run = self.setup(icacls_code=5)
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)
        self.assertIn("icacls failed", run.stderr)
        self.assertTrue(package.exists())

    def test_the_real_helper_script_installs_with_the_path_in_quotes(self) -> None:
        write_package(self.tmp / "package", HELPER.read_text(encoding="utf-8"))
        run = self.setup()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        stored = run.calls("New-Service")
        self.assertEqual(len(stored), 1)
        self.assertIn('BinaryPathName=["', stored[0])
        self.assertIn('vsscom_helper.exe"]', stored[0])

    def test_a_missing_file_ends_with_9_and_not_an_administrator_with_2(self) -> None:
        package = self.tmp / "package"
        write_package(package, "exit 0")
        (package / "driver" / "vsscom_um.cat").unlink()
        self.assertEqual(self.setup().returncode, 9)
        self.assertEqual(self.setup(admin=False).returncode, 2)

    def test_a_restart_asked_for_by_pnputil_ends_with_10_after_the_helper(self) -> None:
        run = self.setup(add_code=3010)
        self.assertEqual(run.returncode, 10, run.stdout + run.stderr)
        self.assertIn("installing the helper service", run.stdout)


REMOVE_STUBS = r"""
function Test-IsAdministrator { [bool] $Scenario.admin }
$global:Devices = @($Scenario.devices | ForEach-Object { [pscustomobject]@{ InstanceId = $_.id; Status = $_.status } })
$global:Packages = @($Scenario.packages)
function Get-PnpDevice { [CmdletBinding()] param($Class) $global:Devices }
function Get-Service { [CmdletBinding()] param($Name) }
function Get-WindowsDriver {
    param([switch] $Online, [switch] $All)
    foreach ($p in $global:Packages) { [pscustomobject]@{ OriginalFileName = 'C:\x\' + $p.original; Driver = $p.published } }
}
function Invoke-Bounded {
    param([string] $File, [string[]] $Arguments, [int] $Seconds)
    Log ('run ' + $File + ' ' + ($Arguments -join ' '))
    if ($Arguments[0] -eq '/remove-device') {
        if (-not $Scenario.remove_keeps -and [int] $Scenario.remove_code -eq 0) {
            $global:Devices = @($global:Devices | Where-Object { $_.InstanceId -ne $Arguments[1] })
        }
        return [int] $Scenario.remove_code
    }
    if ($Arguments[0] -eq '/delete-driver') { $global:Packages = @(); return [int] $Scenario.delete_code }
    0
}
"""

PORT_DEVICES = [
    {"id": "SWD\\VSSCOMUM\\0000", "status": "Unknown"},
    {"id": "SWD\\VSSCOMUM\\0001", "status": "OK"},
    {"id": "SWD\\VSSCOMUM\\0002", "status": "Unknown"},
    {"id": "ROOT\\PORTS\\0000", "status": "OK"},
    {"id": "USB\\VID_1234&PID_5678\\1", "status": "OK"},
]


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class RemoveDriver(Harness):
    def remove(self, helper: str = "Log 'helper uninstall'\nexit 0", **scenario: object) -> Run:
        package = self.tmp / "package"
        if not package.exists():
            (package / "helper").mkdir(parents=True)
            (package / "helper" / "install-helper.ps1").write_text(helper, encoding="ascii")
        base: dict = {
            "admin": True,
            "devices": PORT_DEVICES,
            "packages": [{"original": "vsscom_um.inf", "published": "oem7.inf"}, {"original": "other.inf", "published": "oem8.inf"}],
            "remove_code": 0,
            "delete_code": 0,
        }
        base.update(scenario)
        return self.run_harness(REMOVE_STUBS, REMOVE, f"-PackageRoot {ps_quote(str(package))}", base)

    # DRV-04
    def test_the_port_devices_are_removed_absent_ones_included_and_nothing_else(self) -> None:
        run = self.remove()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(
            run.calls("run pnputil.exe /remove-device"),
            [
                "run pnputil.exe /remove-device SWD\\VSSCOMUM\\0000",
                "run pnputil.exe /remove-device SWD\\VSSCOMUM\\0001",
                "run pnputil.exe /remove-device SWD\\VSSCOMUM\\0002",
            ],
        )

    def test_the_order_is_helper_then_devices_then_package(self) -> None:
        run = self.remove()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        helper = run.log.index("helper uninstall")
        first_device = next(i for i, line in enumerate(run.log) if "/remove-device" in line)
        package = next(i for i, line in enumerate(run.log) if "/delete-driver" in line)
        self.assertLess(helper, first_device)
        self.assertLess(first_device, package)
        self.assertIn("run pnputil.exe /delete-driver oem7.inf /uninstall /force", run.log)
        self.assertNotIn("run pnputil.exe /delete-driver oem8.inf /uninstall /force", run.log)

    def test_a_device_that_cannot_be_removed_ends_with_7(self) -> None:
        run = self.remove(remove_code=5)
        self.assertEqual(run.returncode, 7, run.stdout + run.stderr)
        self.assertIn("SWD\\VSSCOMUM\\0000", run.stderr)
        self.assertEqual(run.calls("run pnputil.exe /delete-driver"), [])

    def test_a_device_that_pnputil_says_it_removed_but_is_still_there_ends_with_7(self) -> None:
        run = self.remove(remove_keeps=True)
        self.assertEqual(run.returncode, 7, run.stdout + run.stderr)
        self.assertIn("port device is still there", run.stderr)

    def test_a_machine_with_no_port_device_is_not_an_error(self) -> None:
        run = self.remove(devices=[{"id": "ROOT\\PORTS\\0000", "status": "OK"}])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.calls("run pnputil.exe /remove-device"), [])

    def test_a_restart_asked_for_while_removing_a_device_ends_with_10(self) -> None:
        run = self.remove(remove_code=3010, remove_keeps=True)
        self.assertEqual(run.returncode, 10, run.stdout + run.stderr)

    # DRV-07
    def test_a_helper_script_that_exits_with_an_error_ends_with_8(self) -> None:
        run = self.remove(helper="exit 3")
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)
        self.assertEqual(run.calls("run pnputil.exe /remove-device"), [])

    def test_a_helper_script_that_throws_ends_with_8(self) -> None:
        run = self.remove(helper="throw 'the helper broke'")
        self.assertEqual(run.returncode, 8, run.stdout + run.stderr)

    def test_a_package_that_cannot_be_deleted_ends_with_7_and_not_an_administrator_with_2(self) -> None:
        self.assertEqual(self.remove(delete_code=5).returncode, 7)
        self.assertEqual(self.remove(admin=False).returncode, 2)


# Certificates for the install-trust.ps1 tests are made here, in memory, and written as files. No private key is kept.
FIXTURE = r"""
param([string] $Folder)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$X = 'System.Security.Cryptography.X509Certificates'
function New-Pem([byte[]] $Raw) {
    "-----BEGIN CERTIFICATE-----`n" + [Convert]::ToBase64String($Raw, 'InsertLineBreaks') + "`n-----END CERTIFICATE-----`n"
}
$now = [DateTimeOffset]::UtcNow
$sha = [Security.Cryptography.HashAlgorithmName]::SHA256
$pad = [Security.Cryptography.RSASignaturePadding]::Pkcs1
$ca = New-Object "$X.X509BasicConstraintsExtension" -ArgumentList $true, $false, 0, $true
$rootKey = [Security.Cryptography.RSA]::Create(2048)
$req = New-Object "$X.CertificateRequest" -ArgumentList 'CN=VSS Test Root', $rootKey, $sha, $pad
$req.CertificateExtensions.Add($ca)
$root = $req.CreateSelfSigned($now.AddDays(-1), $now.AddDays(300))
$ca2 = New-Object "$X.X509BasicConstraintsExtension" -ArgumentList $true, $false, 0, $true
$interKey = [Security.Cryptography.RSA]::Create(2048)
$req = New-Object "$X.CertificateRequest" -ArgumentList 'CN=VSS Test Intermediate', $interKey, $sha, $pad
$req.CertificateExtensions.Add($ca2)
$inter = $req.Create($root, $now.AddDays(-1), $now.AddDays(200), [byte[]](1, 2, 3, 4))
$interWithKey = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::CopyWithPrivateKey($inter, $interKey)
$signerKey = [Security.Cryptography.RSA]::Create(2048)
$req = New-Object "$X.CertificateRequest" -ArgumentList 'CN=VSS Test Signer', $signerKey, $sha, $pad
$oids = New-Object Security.Cryptography.OidCollection
[void] $oids.Add((New-Object Security.Cryptography.Oid '1.3.6.1.5.5.7.3.3'))
$req.CertificateExtensions.Add((New-Object "$X.X509EnhancedKeyUsageExtension" -ArgumentList $oids, $false))
$signer = $req.Create($interWithKey, $now.AddDays(-1), $now.AddDays(100), [byte[]](5, 6, 7, 8))
[IO.File]::WriteAllText((Join-Path $Folder 'root.pem'), (New-Pem $root.RawData))
[IO.File]::WriteAllText((Join-Path $Folder 'intermediate.pem'), (New-Pem $inter.RawData))
[IO.File]::WriteAllBytes((Join-Path $Folder 'signer.cer'), $signer.RawData)
$otherKey = [Security.Cryptography.RSA]::Create(2048)
$req = New-Object "$X.CertificateRequest" -ArgumentList 'CN=VSS Other Signer', $otherKey, $sha, $pad
$other = $req.CreateSelfSigned($now.AddDays(-1), $now.AddDays(100))
[IO.File]::WriteAllBytes((Join-Path $Folder 'other.cer'), $other.RawData)
@{ root = $root.Thumbprint; intermediate = $inter.Thumbprint; signer = $signer.Thumbprint } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Folder 'thumbprints.json')
"""

# Reads the three test stores back: thumbprint, friendly name and the purposes property of each certificate.
INSPECT = r"""
param([string] $Joined)
$Names = $Joined.Split(',')
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class VssTestProperty {
    [DllImport("crypt32.dll", SetLastError = true)]
    static extern bool CertGetCertificateContextProperty(IntPtr context, uint id, byte[] data, ref int size);
    public static string Hex(IntPtr context) {
        int size = 0;
        if (!CertGetCertificateContextProperty(context, 9, null, ref size)) return "";
        byte[] data = new byte[size];
        if (!CertGetCertificateContextProperty(context, 9, data, ref size)) return "";
        return BitConverter.ToString(data).Replace("-", "");
    }
}
'@
$out = @()
foreach ($name in $Names) {
    $store = New-Object Security.Cryptography.X509Certificates.X509Store -ArgumentList $name, 'CurrentUser'
    $store.Open('ReadOnly')
    foreach ($c in $store.Certificates) {
        $out += [pscustomobject]@{ store = $name; thumbprint = $c.Thumbprint; friendly = $c.FriendlyName; purposes = [VssTestProperty]::Hex($c.Handle) }
    }
    $store.Close()
}
ConvertTo-Json -InputObject @($out) -Compress
"""

CODE_SIGNING_ONLY = "300A06082B06010505070303"
MARKER = "Virtual Serial Studio (install-trust.ps1)"

TRUST_STUBS = r"""
function Test-IsAdministrator { [bool] $Scenario.admin }
function bcdedit.exe { $global:LASTEXITCODE = 0; if ($Scenario.testsigning) { '    testsigning             Yes' } }
$global:Calls = 0
$global:SignerCert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList (, [IO.File]::ReadAllBytes($Scenario.signer))
function Get-AuthenticodeSignature {
    param($LiteralPath)
    $list = @($Scenario.statuses)
    $status = $list[[Math]::Min($global:Calls, $list.Count - 1)]
    $global:Calls++
    Log "signature $status"
    [pscustomobject]@{ Status = $status; StatusMessage = "stub $status"; SignerCertificate = $global:SignerCert; TimeStamperCertificate = $global:SignerCert }
}
"""


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell")
class InstallTrust(Harness):
    fixture: Path
    thumbs: dict

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Path(tempfile.mkdtemp(prefix="vss-trust-fixture-"))
        script = cls.fixture / "make.ps1"
        script.write_text(FIXTURE, encoding="utf-8")
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-Folder", str(cls.fixture)],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"could not make the test certificates: {result.stderr}")
        cls.thumbs = json.loads((cls.fixture / "thumbprints.json").read_text(encoding="utf-8-sig"))

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.fixture, ignore_errors=True)

    def setUp(self) -> None:
        super().setUp()
        tag = uuid.uuid4().hex[:10]
        self.stores = {"root": f"VssTrustTest{tag}Root", "ca": f"VssTrustTest{tag}CA", "pub": f"VssTrustTest{tag}Pub"}
        self.addCleanup(self.drop_stores)
        self.package = self.tmp / "package.cat"
        self.package.write_bytes(b"not a real catalog")

    def drop_stores(self) -> None:
        names = ", ".join(ps_quote(n) for n in self.stores.values())
        subprocess.run(
            [POWERSHELL, "-NoProfile", "-Command",
             f"foreach ($n in {names}) {{ Remove-Item -LiteralPath \"HKCU:\\Software\\Microsoft\\SystemCertificates\\$n\" -Recurse -Force -ErrorAction SilentlyContinue }}"],
            capture_output=True, text=True, timeout=60, check=False,
        )

    def trust(self, *extra: str, package: bool = True, **scenario: object) -> Run:
        base: dict = {"admin": True, "testsigning": False, "statuses": ["NotTrusted", "Valid"], "signer": str(self.fixture / "signer.cer")}
        base.update(scenario)
        arguments = (
            f"-RootCertificate {ps_quote(str(self.fixture / 'root.pem'))} "
            f"-IntermediateCertificate {ps_quote(str(self.fixture / 'intermediate.pem'))} "
            f"-StoreLocation CurrentUser -RootStoreName {self.stores['root']} -IntermediateStoreName {self.stores['ca']} "
            f"-PublisherStoreName {self.stores['pub']} "
            + (f"-Package {ps_quote(str(self.package))} " if package else "")
            + " ".join(extra)
        )
        return self.run_harness(TRUST_STUBS, TRUST, arguments, base)

    def inspect(self) -> list[dict]:
        script = self.tmp / "inspect.ps1"
        script.write_text(INSPECT, encoding="utf-8")
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), ",".join(self.stores.values())],
            capture_output=True, text=True, timeout=120, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def seed(self, store: str, pem_name: str) -> None:
        """Puts a certificate into a test store without the marker, as an earlier version of the script did."""
        script = self.tmp / "seed.ps1"
        script.write_text(
            "param($Name, $Path)\n"
            "$raw = (Get-Content -Raw -LiteralPath $Path) -replace '-----[A-Z ]+-----', '' -replace '\\s', ''\n"
            "$c = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList (, [Convert]::FromBase64String($raw))\n"
            "$s = New-Object Security.Cryptography.X509Certificates.X509Store -ArgumentList $Name, 'CurrentUser'\n"
            "$s.Open('ReadWrite'); $s.Add($c); $s.Close()\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), self.stores[store], str(self.fixture / pem_name)],
            capture_output=True, text=True, timeout=60, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_good_run_adds_the_three_certificates_and_limits_the_root_to_code_signing(self) -> None:
        run = self.trust()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        found = {c["store"]: c for c in self.inspect()}
        self.assertEqual(found[self.stores["root"]]["thumbprint"], self.thumbs["root"])
        self.assertEqual(found[self.stores["root"]]["purposes"], CODE_SIGNING_ONLY)
        self.assertEqual(found[self.stores["ca"]]["thumbprint"], self.thumbs["intermediate"])
        self.assertEqual(found[self.stores["ca"]]["purposes"], "")
        self.assertEqual(found[self.stores["pub"]]["thumbprint"], self.thumbs["signer"])
        for entry in found.values():
            self.assertEqual(entry["friendly"], MARKER)

    def test_a_package_that_was_altered_after_signing_adds_nothing(self) -> None:
        run = self.trust(statuses=["HashMismatch"])
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn("HashMismatch", run.stderr)
        self.assertNotIn("added to", run.stdout)  # refused before anything was added, not added and taken out
        self.assertEqual(self.inspect(), [])

    def test_a_failure_after_the_certificates_were_added_takes_them_out_again(self) -> None:
        run = self.trust(statuses=["NotTrusted", "HashMismatch"])
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn("signer not added", run.stderr)
        self.assertIn("taken out again", run.stdout)
        self.assertEqual(self.inspect(), [])

    def test_a_failure_leaves_alone_what_was_there_before(self) -> None:
        self.seed("root", "root.pem")
        run = self.trust(statuses=["NotTrusted", "HashMismatch"])
        self.assertEqual(run.returncode, 1, run.stdout)
        found = self.inspect()
        self.assertEqual([c["thumbprint"] for c in found], [self.thumbs["root"]])
        self.assertEqual(found[0]["friendly"], "")

    def test_without_a_package_the_root_and_intermediate_are_added_and_the_signer_is_not(self) -> None:
        run = self.trust(package=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual({c["store"] for c in self.inspect()}, {self.stores["root"], self.stores["ca"]})

    def test_whatif_changes_nothing(self) -> None:
        run = self.trust("-WhatIf")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.inspect(), [])

    def test_uninstall_removes_what_a_run_added(self) -> None:
        self.assertEqual(self.trust().returncode, 0)
        run = self.trust("-Uninstall")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.inspect(), [])

    def test_uninstall_leaves_a_root_that_the_script_did_not_add(self) -> None:
        self.seed("root", "root.pem")
        run = self.trust("-Uninstall", package=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual([c["thumbprint"] for c in self.inspect()], [self.thumbs["root"]])
        self.assertIn("not added by this script", run.stdout)

    def test_uninstall_with_include_unmarked_removes_it_too(self) -> None:
        self.seed("root", "root.pem")
        run = self.trust("-Uninstall", "-IncludeUnmarked", package=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.inspect(), [])

    def test_exit_codes_2_and_3_are_reachable(self) -> None:
        # Write-Error under $ErrorActionPreference = 'Stop' throws, so these used to come out as 1.
        run = self.run_harness(
            TRUST_STUBS,
            TRUST,
            f"-RootCertificate {ps_quote(str(self.fixture / 'root.pem'))} -IntermediateCertificate {ps_quote(str(self.fixture / 'intermediate.pem'))}",
            {"admin": False, "testsigning": False, "statuses": ["Valid"], "signer": str(self.fixture / "signer.cer")},
        )
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn("administrator", run.stderr)
        run = self.trust(testsigning=True)
        self.assertEqual(run.returncode, 3, run.stdout + run.stderr)
        self.assertIn("test signing is ON", run.stderr)
        self.assertEqual(self.inspect(), [])

    def test_a_package_signed_under_another_root_adds_nothing(self) -> None:
        run = self.trust(signer=str(self.fixture / "other.cer"))
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn("not signed under the root", run.stderr)
        self.assertEqual(self.inspect(), [])


if __name__ == "__main__":
    unittest.main()
