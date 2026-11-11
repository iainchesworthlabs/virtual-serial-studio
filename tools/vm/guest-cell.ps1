#!/usr/bin/env pwsh
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
<#
.SYNOPSIS
    Runs one platform-matrix cell in a VSS guest, from the clean snapshot to the evidence.

.DESCRIPTION
    Reverts the guest to `clean`, starts it, waits for VMware Tools, copies in a bundle made by
    tools/vm/make-bundle.ps1 (Windows) or tools/vm/make-bundle.sh (Ubuntu), runs ctest and
    tools/matrix/run_cell.py inside the guest, copies the cell output back, and stops the guest.
    The credentials come from VSS_VM_USER and VSS_VM_PASSWORD, or from
    tools/vm/credentials.ps1 when those are not set.

    Only one cell may use a guest at a time: starting or stopping a guest another run is using
    cuts that run off. The script refuses a guest that is already running unless -Force is given.

    With -DriverBundle, a Windows guest also runs the driver tests after the cell (or instead of
    it, when -Bundle is left out): tools/vm/guest/win-driver.ps1 signs, installs and uninstalls
    each driver in the guest and runs test-wow64.ps1, the 32-bit client and the hung-program
    tests, against it, and test-client.ps1 when the bundle carries vss_unit_tests.exe. The evidence comes back to <Out>\<guest id>\driver-out. The driver is x64
    and ARM64 only, so these guests are 64-bit and the 32-bit program runs under WOW64.

.PARAMETER Guest
    windows10, windows11 or ubuntu2604, as in tools/vm/matrix.json.

.PARAMETER Bundle
    The folder the make-bundle script wrote. Optional when -DriverBundle is given.

.PARAMETER Package
    Optional. A release archive for T9: virtual-serial-studio-*-windows-x86_64.zip for a Windows
    guest, or ...-linux-x86_64.tar.gz for Ubuntu (from the Release workflow's dry run).

.PARAMETER DriverBundle
    Optional, Windows guests only. The zip that tools/vm/make-driver-bundle.ps1 wrote.

.PARAMETER DriverFlavour
    Which drivers to run, kmdf and umdf by default. A flavour the bundle lacks is skipped.

.PARAMETER DriverSuite
    Also run test-driver.ps1, the driver's own suite, before the 32-bit tests.

.PARAMETER Out
    Where the evidence goes: <Out>\<guest id>\cell-out.

.EXAMPLE
    ./tools/vm/make-bundle.ps1 -Repository . -Out D:\bundle-win
    ./tools/vm/guest-cell.ps1 -Guest windows11 -Bundle D:\bundle-win -Commit (git rev-parse --short HEAD)

.EXAMPLE
    ./tools/vm/make-driver-bundle.ps1 -Repository . -Out D:\bundle-driver\driver-bundle.zip
    ./tools/vm/guest-cell.ps1 -Guest windows10 -DriverBundle D:\bundle-driver\driver-bundle.zip
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('windows10', 'windows11', 'ubuntu2604')] [string] $Guest,
    [string] $Bundle,
    [string] $Package,
    [string] $DriverBundle,
    [ValidateSet('kmdf', 'umdf')] [string[]] $DriverFlavour = @('kmdf', 'umdf'),
    [switch] $DriverSuite,
    [string] $Out = 'out/guest-cell',
    [string] $Commit = '',
    [switch] $Force,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$vmrun = Join-Path $here 'vmrun.ps1'
$matrix = Get-Content -LiteralPath (Join-Path $here 'matrix.json') -Raw | ConvertFrom-Json
$entry = $matrix.guests | Where-Object { $_.id -eq $Guest }
# Not Join-Path: the root is a Windows path, and on another system Join-Path refuses its drive letter.
$vm = "$($matrix.root)\$($entry.name)\$($entry.name).vmx"
$cell = $entry.name -replace ' - ', '-' -replace ' ', '-'
$snapshot = $matrix.snapshot
$windows = $entry.kind -eq 'windows'

if (-not $Bundle -and -not $DriverBundle) { throw 'pass -Bundle, -DriverBundle or both' }
if ($DriverBundle -and -not $windows) { throw '-DriverBundle is for the Windows guests: the driver is a Windows driver' }

if (-not $DryRun) {
    if ($Bundle) {
        if (-not (Test-Path -LiteralPath $Bundle)) { throw "no such bundle folder: $Bundle" }
        $needed = if ($windows) { 'bundle.zip', 'bundle.json', 'py.zip', 'rt.zip' } else { 'bundle.tgz', 'bundle.env' }
        $absent = $needed | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Bundle $_)) }
        if ($absent) { throw "the bundle lacks $($absent -join ', ')" }
    }
    if ($DriverBundle -and -not (Test-Path -LiteralPath $DriverBundle)) { throw "no such driver bundle: $DriverBundle" }
    if (-not $env:VSS_VM_USER -or -not $env:VSS_VM_PASSWORD) { . (Join-Path $here 'credentials.ps1') | Out-Null }
    if (-not $Force -and ((& $vmrun status -Vm $vm) -eq 'running')) {
        throw "$($entry.name) is running, so another run may be using it; stop it first, or pass -Force"
    }
}

function Step([string] $Action, [hashtable] $Parameters = @{}) {
    & $vmrun $Action -Vm $vm @Parameters -DryRun:$DryRun
    if ($LASTEXITCODE -ne 0 -and $Action -ne 'run') { throw "vmrun $Action failed with exit code $LASTEXITCODE" }
}

$destination = Join-Path $Out $Guest
New-Item -ItemType Directory -Force $destination | Out-Null
$evidence = Join-Path $destination 'cell-out'
$driverEvidence = Join-Path $destination 'driver-out'

try {
    Step 'revert' @{ Name = $snapshot }
    Step 'start'
    # wait-guest, not wait-tools: checkToolsState can say "installed" for minutes after the guest
    # is up, until something asks the guest a question with the credentials.
    Step 'wait-guest' @{ TimeoutSeconds = 900 }

    if ($windows) {
        $folder = 'C:\vss'
        Step 'run' @{ CommandLine = "New-Item -ItemType Directory -Force $folder | Out-Null" }
    }
    if ($windows -and $Bundle) {
        foreach ($name in 'bundle.zip', 'bundle.json', 'py.zip', 'pwsh.zip', 'rt.zip') {
            if ($DryRun -or (Test-Path -LiteralPath (Join-Path $Bundle $name))) {
                Step 'copy-to' @{ Local = (Join-Path $Bundle $name); Guest = "$folder\$name" }
            }
        }
        if ($Package) { Step 'copy-to' @{ Local = $Package; Guest = "$folder\package.zip" } }
        Step 'copy-to' @{ Local = (Join-Path $here 'guest\win-cell.ps1'); Guest = "$folder\win-cell.ps1" }
        $command = "& $folder\win-cell.ps1 -Cell '$cell' -VmName '$($entry.name)' -Commit '$Commit' *> $folder\g.out"
        Step 'run' @{ CommandLine = $command }
        $ran = $LASTEXITCODE
        # The offscreen pictures prove rendering. This one shows the program on the guest's own
        # desktop: a scheduled task starts it in the auto-logon user's session (a program that
        # vmrun starts runs in session 0, which has no screen), then the screen is captured.
        $buildDir = if ($DryRun) { 'build\windows-msvc-release-gui' } else { (Get-Content -LiteralPath (Join-Path $Bundle 'bundle.json') -Raw | ConvertFrom-Json).buildDir }
        $gui = "$folder\src\$buildDir\bin\vss_gui.exe"
        $start = "schtasks /create /tn vss-gui /tr '$gui' /sc once /st 23:59 /ru $env:VSS_VM_USER /it /f; schtasks /run /tn vss-gui; Start-Sleep -Seconds 40"
        Step 'run' @{ CommandLine = $start }
        Step 'screenshot' @{ Local = (Join-Path $destination "desktop-$Guest.png") }
        Step 'run' @{ CommandLine = "schtasks /delete /tn vss-gui /f; Stop-Process -Name vss_gui -Force -ErrorAction SilentlyContinue" }
        Step 'copy-from' @{ Guest = "$folder\g.out"; Local = (Join-Path $destination 'g.out') }
        Step 'copy-from' @{ Guest = "$folder\cell-out.zip"; Local = (Join-Path $destination 'cell-out.zip') }
        if (-not $DryRun) {
            Remove-Item -Recurse -Force $evidence -ErrorAction SilentlyContinue
            Expand-Archive -Path (Join-Path $destination 'cell-out.zip') -DestinationPath $destination -Force
        }
    } elseif (-not $windows) {
        $folder = '/tmp/vss'
        Step 'run' @{ CommandLine = "mkdir -p $folder" }
        foreach ($name in 'bundle.tgz', 'bundle.env') {
            Step 'copy-to' @{ Local = (Join-Path $Bundle $name); Guest = "$folder/$name" }
        }
        if ($Package) { Step 'copy-to' @{ Local = $Package; Guest = "$folder/package.tar.gz" } }
        Step 'copy-to' @{ Local = (Join-Path $here 'guest/linux-cell.sh'); Guest = "$folder/linux-cell.sh" }
        $command = "bash $folder/linux-cell.sh '$cell' '$($entry.name)' '$Commit' > $folder/g.out 2>&1"
        Step 'run' @{ CommandLine = $command }
        $ran = $LASTEXITCODE
        Step 'copy-from' @{ Guest = "$folder/g.out"; Local = (Join-Path $destination 'g.out') }
        Step 'copy-from' @{ Guest = "$folder/cell-out.tgz"; Local = (Join-Path $destination 'cell-out.tgz') }
        if (-not $DryRun) {
            Remove-Item -Recurse -Force $evidence -ErrorAction SilentlyContinue
            tar -xzf (Join-Path $destination 'cell-out.tgz') -C $destination
        }
    }

    # The driver tests run after the cell, in the same guest, so the cell sees a guest with no
    # driver in it. win-driver.ps1 signs, installs and removes each driver itself.
    if ($windows -and $DriverBundle) {
        Step 'copy-to' @{ Local = $DriverBundle; Guest = "$folder\driver-bundle.zip" }
        Step 'copy-to' @{ Local = (Join-Path $here 'guest\win-driver.ps1'); Guest = "$folder\win-driver.ps1" }
        $flavours = ($DriverFlavour | ForEach-Object { "'$_'" }) -join ','
        $suite = if ($DriverSuite) { ' -Suite' } else { '' }
        Step 'run' @{ CommandLine = "& $folder\win-driver.ps1 -Flavour $flavours$suite *> $folder\d.out" }
        Step 'copy-from' @{ Guest = "$folder\d.out"; Local = (Join-Path $destination 'd.out') }
        Step 'copy-from' @{ Guest = "$folder\driver-out.zip"; Local = (Join-Path $destination 'driver-out.zip') }
        if (-not $DryRun) {
            Remove-Item -Recurse -Force $driverEvidence -ErrorAction SilentlyContinue
            Expand-Archive -Path (Join-Path $destination 'driver-out.zip') -DestinationPath $destination -Force
        }
    }
}
finally {
    # Leave the guest off whatever happened: a running guest blocks the next cell.
    # A guest whose Tools are not up cannot be stopped softly; this must not hide the error that got us here.
    try { & $vmrun stop -Vm $vm -DryRun:$DryRun | Out-Null } catch { Write-Warning "could not stop $($entry.name): $($_.Exception.Message)" }
}

if ($DryRun) { exit 0 }
$exitCode = 0

if ($Bundle) {
    $result = Get-ChildItem $evidence -Filter 'result-*.json' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $result) {
        Write-Host "no result file came back; see $(Join-Path $destination 'g.out')"
        $exitCode = 1
    } else {
        $data = Get-Content -LiteralPath $result.FullName -Raw | ConvertFrom-Json
        foreach ($item in $data.results) { '{0,-4} {1,-8} {2}' -f $item.id, $item.status, $item.detail }
        $bad = @($data.results | Where-Object { $_.status -in 'fail', 'not_run' })
        Write-Host "$($data.results.Count - $bad.Count) of $($data.results.Count) tests passed or do not apply; evidence in $evidence"
        if ($bad.Count -gt 0) { $exitCode = 1 }
    }
}

if ($DriverBundle) {
    # One result file per driver and step. A test that failed, or a step that did not leave a
    # result, fails the run; skip and unexpected are printed and do not (read the detail).
    $summaryFile = Join-Path $driverEvidence 'driver-summary.json'
    if (-not (Test-Path -LiteralPath $summaryFile)) {
        Write-Host "no driver summary came back; see $(Join-Path $destination 'd.out')"
        $exitCode = 1
    } else {
        $failed = 0
        foreach ($step in (Get-Content -LiteralPath $summaryFile -Raw | ConvertFrom-Json)) {
            $file = Join-Path $driverEvidence $step.result
            if ($step.step -in 'wow64', 'suite', 'client', 'install', 'uninstall' -and (Test-Path -LiteralPath $file)) {
                $data = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
                foreach ($item in $data.tests) {
                    '{0,-6} {1,-10} {2,-14} {3,-4} {4}' -f $step.flavour, $item.status, $step.step, $item.id, $item.detail
                    if ($item.status -eq 'fail') { $failed++ }
                }
            } elseif ($null -ne $step.exit -and $step.exit -ne 0) {
                '{0,-6} {1,-10} {2,-14} exit {3}' -f $step.flavour, 'fail', $step.step, $step.exit
                $failed++
            }
        }
        Write-Host "driver tests: $failed failed; evidence in $driverEvidence"
        if ($failed -gt 0) { $exitCode = 1 }
    }
}
exit $exitCode
