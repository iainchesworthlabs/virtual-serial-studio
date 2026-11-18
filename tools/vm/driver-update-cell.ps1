#!/usr/bin/env pwsh
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
<#
.SYNOPSIS
    Runs the driver update scenario (tools/vm/guest/win-driver-update.ps1) in a Windows guest, from
    the clean snapshot to the evidence.

.DESCRIPTION
    -Old and -New are folders made by drivers/windows/stage-package.ps1 from two builds of the
    user-mode driver whose revisions in drivers/windows/driver-version.json differ (build one, stage
    it, run `python tools/ci/check_driver_version.py --bump`, build and stage again, and put the tree back).
    The script packs them with the guest scripts of drivers/windows/test, reverts the guest to `clean`,
    runs the scenario, copies the evidence to <Out>\<guest>\update-out and stops the guest. One run at
    a time per guest, as for tools/vm/guest-cell.ps1.

.EXAMPLE
    ./tools/vm/driver-update-cell.ps1 -Guest windows11 -Old D:\tmp\upd\old -New D:\tmp\upd\new -Out D:\tmp\upd\evidence
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('windows10', 'windows11')] [string] $Guest,
    [Parameter(Mandatory)] [string] $Old,
    [Parameter(Mandatory)] [string] $New,
    [string] $Out = 'out/driver-update',
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repository = Split-Path -Parent (Split-Path -Parent $here)
$vmrun = Join-Path $here 'vmrun.ps1'
$matrix = Get-Content -LiteralPath (Join-Path $here 'matrix.json') -Raw | ConvertFrom-Json
$entry = $matrix.guests | Where-Object { $_.id -eq $Guest }
$vm = "$($matrix.root)\$($entry.name)\$($entry.name).vmx"
if (-not $env:VSS_VM_USER -or -not $env:VSS_VM_PASSWORD) { . (Join-Path $here 'credentials.ps1') | Out-Null }
if (-not $Force -and ((& $vmrun status -Vm $vm) -eq 'running')) {
    throw "$($entry.name) is running, so another run may be using it; stop it first, or pass -Force"
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "vss-update-bundle-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory $stage | Out-Null
try {
    Copy-Item -Recurse $Old (Join-Path $stage 'old')
    Copy-Item -Recurse $New (Join-Path $stage 'new')
    New-Item -ItemType Directory (Join-Path $stage 'test') | Out-Null
    Copy-Item (Join-Path $repository 'drivers\windows\test\*.ps1') (Join-Path $stage 'test')
    # The question of the installer's driver page, as a throwaway installer, when makensis is here.
    $makensis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
    if (-not $makensis -and (Test-Path 'C:\Program Files (x86)\NSIS\Bin\makensis.exe')) { $makensis = 'C:\Program Files (x86)\NSIS\Bin\makensis.exe' }
    $declared = Select-String -LiteralPath (Join-Path $Old 'driver\vsscom_um.inf') -Pattern '^\s*DriverVer\s*=\s*(\S+)' | Select-Object -First 1
    if ($makensis -and $declared) {
        & $makensis "/DOUTFILE=$(Join-Path $stage 'test\driver-query.exe')" `
            "/DDRIVER_NSH=$(Join-Path $repository 'cmake\packaging\windows\driver.nsh')" `
            "/DSCRIPT=$(Join-Path $repository 'drivers\windows\setup-driver.ps1')" `
            "/DBUNDLED=$($declared.Matches[0].Groups[1].Value)" (Join-Path $here 'driver-query.nsi') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'makensis failed for driver-query.nsi' }
    }
    $zip = Join-Path ([IO.Path]::GetTempPath()) 'vss-update-bundle.zip'
    Remove-Item $zip -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

    function Step([string] $Action, [hashtable] $Parameters = @{}) {
        & $vmrun $Action -Vm $vm @Parameters
        if ($LASTEXITCODE -ne 0 -and $Action -ne 'run') { throw "vmrun $Action failed with exit code $LASTEXITCODE" }
    }
    $destination = Join-Path $Out $Guest
    New-Item -ItemType Directory -Force $destination | Out-Null
    try {
        Step 'revert' @{ Name = $matrix.snapshot }
        Step 'start'
        Step 'wait-guest' @{ TimeoutSeconds = 900 }
        Step 'run' @{ CommandLine = 'New-Item -ItemType Directory -Force C:\vss | Out-Null' }
        Step 'copy-to' @{ Local = $zip; Guest = 'C:\vss\update-bundle.zip' }
        Step 'copy-to' @{ Local = (Join-Path $here 'guest\win-driver-update.ps1'); Guest = 'C:\vss\win-driver-update.ps1' }
        Step 'run' @{ CommandLine = '& C:\vss\win-driver-update.ps1 *> C:\vss\u.out' }
        Step 'copy-from' @{ Guest = 'C:\vss\u.out'; Local = (Join-Path $destination 'u.out') }
        Step 'copy-from' @{ Guest = 'C:\vss\update-out.zip'; Local = (Join-Path $destination 'update-out.zip') }
        Expand-Archive -Path (Join-Path $destination 'update-out.zip') -DestinationPath $destination -Force
    }
    finally {
        try { & $vmrun stop -Vm $vm | Out-Null } catch { Write-Warning "could not stop $($entry.name): $($_.Exception.Message)" }
    }
}
finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
Write-Host "evidence in $(Join-Path $Out "$Guest\update-out")"
