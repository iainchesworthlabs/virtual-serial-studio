#!/usr/bin/env pwsh
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
<#
.SYNOPSIS
    Runs the Windows driver tests inside a Windows guest. tools/vm/guest-cell.ps1 copies this
    file and the driver bundle in and runs it; run it by hand only to repeat a run.

.DESCRIPTION
    The bundle (tools/vm/make-driver-bundle.ps1) is C:\vss\driver-bundle.zip. For each driver in
    -Flavour that the bundle holds, in order, this script:

      1. signs the package with a throwaway certificate made in the guest (prepare-test-signing.ps1);
      2. installs the driver (install-driver.ps1; -Helper for the user-mode driver);
      3. runs the driver's own suite (test-driver.ps1) when -Suite is given;
      4. runs the 32-bit and hung-program tests (test-wow64.ps1);
      5. uninstalls it (uninstall-driver.ps1).

    A step that fails does not stop the next one, so a failed install still leaves the evidence
    of the steps that ran. Everything goes to C:\vss\driver-out, zipped as C:\vss\driver-out.zip:
    a folder per driver with one JSON file per step (result-<step>.json), a log per step and the
    JSON the test client printed, and driver-summary.json.

    The guest is a disposable VSS guest with test signing on. Do not run this anywhere else.
#>

param(
    [string[]] $Flavour = @('kmdf', 'umdf'),
    [switch] $Suite
)

$ErrorActionPreference = 'Continue'
$root = 'C:\vss'
$work = Join-Path $root 'driver'
$out = Join-Path $root 'driver-out'
New-Item -ItemType Directory -Force $root | Out-Null
Remove-Item $work, $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Expand-Archive -Path (Join-Path $root 'driver-bundle.zip') -DestinationPath $work -Force

$summary = @()

# Runs one script of the bundle in a new PowerShell, so that one step cannot upset the next, and
# returns its exit code. The script's output goes to <flavour>\<step>.log.
function Invoke-Step([string] $Driver, [string] $Step, [string] $Script, [string[]] $Arguments, [switch] $NoResultPath) {
    $folder = Join-Path $work $Driver
    $stepOut = Join-Path $out $Driver
    New-Item -ItemType Directory -Force $stepOut | Out-Null
    $result = Join-Path $stepOut "result-$Step.json"
    $log = Join-Path $stepOut "$Step.log"
    if (-not $NoResultPath) { $Arguments = $Arguments + @('-ResultPath', $result) }
    Push-Location $folder
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $folder $Script) @Arguments *>&1 |
            Out-File -Encoding utf8 $log
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    Write-Host "[$Driver] $Step exit $code"
    $script:summary += [ordered]@{ flavour = $Driver; step = $Step; exit = $code; result = "$Driver\$(if ($NoResultPath) { "$Step.log" } else { "result-$Step.json" })" }
    $code
}

foreach ($name in $Flavour) {
    $folder = Join-Path $work $name
    if (-not (Test-Path $folder)) {
        Write-Host "[$name] not in the bundle; skipped"
        $summary += [ordered]@{ flavour = $name; step = 'bundle'; exit = $null; result = 'not in the bundle' }
        continue
    }
    $driverArgs = @('-Flavour', $name)

    # Exit 1 means signed but test signing or a restart is still needed: nothing would load.
    $signed = Invoke-Step $name 'prepare-test-signing' 'prepare-test-signing.ps1' `
        @('-ConfirmDisposableGuest', '-PackageDir', $folder, '-Flavour', $name) -NoResultPath
    if ($signed -ne 0) {
        Write-Host "[$name] the package is not ready to install; skipping the tests"
        continue
    }

    $installArgs = $driverArgs + @('-PackageDir', $folder)
    if ($name -eq 'umdf') { $installArgs += '-Helper' }
    $installed = Invoke-Step $name 'install' 'install-driver.ps1' $installArgs
    if ($installed -eq 0) {
        # The helper is a service that has just started; give it and the ports a moment.
        Start-Sleep -Seconds 5
        if ($Suite) { Invoke-Step $name 'suite' 'test-driver.ps1' $driverArgs | Out-Null }
        # The studio's own client, when the bundle carries it (make-driver-bundle.ps1 -UnitTests).
        # -Exe is given: Windows PowerShell leaves $PSScriptRoot empty in a parameter default.
        # It runs before the hung-program tests, which kill programs and restart the helper
        # service and can leave a port behind.
        $unitTests = Join-Path $folder 'vss_unit_tests.exe'
        if (Test-Path $unitTests) {
            Invoke-Step $name 'client' 'test-client.ps1' @('-Exe', $unitTests) | Out-Null
        }
        Invoke-Step $name 'wow64' 'test-wow64.ps1' ($driverArgs + @('-ClientDir', $folder)) | Out-Null
    }
    Invoke-Step $name 'uninstall' 'uninstall-driver.ps1' $driverArgs | Out-Null
}

$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $out 'driver-summary.json')
Compress-Archive -Path $out -DestinationPath (Join-Path $root 'driver-out.zip') -Force
exit 0
