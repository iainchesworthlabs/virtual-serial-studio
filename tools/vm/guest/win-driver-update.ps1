#!/usr/bin/env pwsh
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
<#
.SYNOPSIS
    Runs the driver update scenario inside a Windows guest. tools/vm/driver-update-cell.ps1 copies
    this file and the bundle in and runs it.

.DESCRIPTION
    The bundle (C:\vss\update-bundle.zip) holds two staged packages, as stage-package.ps1 lays them
    out (setup-driver.ps1, remove-driver.ps1, driver\, helper\, trust\), and the guest scripts of
    drivers/windows/test:

      old\   the driver at one revision of drivers/windows/driver-version.json
      new\   the same driver at the next revision
      test\  prepare-test-signing.ps1, common.ps1, hub-io.ps1 and the rest

    The scenario answers what setup-driver.ps1 does to a PC that already has a driver:

      1. sign both packages with throwaway certificates made in the guest;
      2. set up old, make two ports through the helper, close one (its device stays behind), keep one open;
      3. set up new over it: the driver store, and the driver each port device is bound to;
      4. make a port again, and one more: which driver do new ports and reused devices get;
      5. set up new again (same version): nothing is installed again;
      6. set up old (a downgrade): nothing is replaced;
      7. set up new with -ReinstallDriver;
      8. remove everything with remove-driver.ps1.

    Each step writes snap-<step>.json (the packages in the store, the port devices with the package
    they are bound to) and <step>.log to C:\vss\update-out, zipped as C:\vss\update-out.zip, with
    summary.json. The guest is a disposable VSS guest; do not run this anywhere else.
#>

$ErrorActionPreference = 'Continue'
$root = 'C:\vss'
$work = Join-Path $root 'upd'
$out = Join-Path $root 'update-out'
New-Item -ItemType Directory -Force $root | Out-Null
Remove-Item $work, $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Expand-Archive -Path (Join-Path $root 'update-bundle.zip') -DestinationPath $work -Force
$summary = [ordered]@{}

function Save-Json($Name, $Value) { $Value | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $out $Name) }

# The packages in the driver store and the port devices of the helper, each with the package it uses.
function Get-Snapshot {
    $packages = @(Get-WindowsDriver -Online -All | Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' } |
        ForEach-Object { [ordered]@{ driver = $_.Driver; version = "$($_.Version)"; date = "$($_.Date)" } })
    $devices = @(Get-PnpDevice -InstanceId 'SWD\VSSCOMUM\*' -ErrorAction SilentlyContinue | ForEach-Object {
        $id = $_.InstanceId
        $property = { param($key) (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction SilentlyContinue).Data }
        [ordered]@{
            id = $id; present = ($_.Present -eq $true); status = "$($_.Status)"
            driverVersion = "$(& $property 'DEVPKEY_Device_DriverVersion')"
            infPath = "$(& $property 'DEVPKEY_Device_DriverInfPath')"
        }
    })
    [ordered]@{ packages = $packages; devices = $devices }
}

function Invoke-Script([string] $Step, [string] $Script, [string[]] $Arguments) {
    $log = Join-Path $out "$Step.log"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments *>&1 | Out-File -Encoding utf8 $log
    $code = $LASTEXITCODE
    Write-Host "[$Step] exit $code"
    $summary[$Step] = [ordered]@{ exit = $code }
    $code
}

function Save-Step([string] $Step) {
    Save-Json "snap-$Step.json" (Get-Snapshot)
    Save-Query $Step
}

# What the installer's driver page would say now, from a throwaway installer built from driver.nsh and run
# elevated, as the real one is. Its bundled version is the old package's, so: none, then same, then newer.
function Save-Query([string] $Step) {
    $exe = Join-Path $work 'test\driver-query.exe'
    if (-not (Test-Path $exe)) { return }
    Remove-Item (Join-Path $work 'test\q.out') -ErrorAction SilentlyContinue
    Start-Process -FilePath $exe -ArgumentList '/S' -Wait
    $answer = if (Test-Path (Join-Path $work 'test\q.out')) { Get-Content (Join-Path $work 'test\q.out') -Raw } else { '(no answer)' }
    Write-Host "[$Step] installer page query: $answer"
    $summary["query-$Step"] = "$answer"
}

# The signing script makes a certificate each time it runs, so each package has its own. The helper
# program is signed too, as setup-driver.ps1 checks it.
function Sign-Package([string] $Folder, [string] $Name) {
    Invoke-Script "sign-$Name" (Join-Path $work 'test\prepare-test-signing.ps1') `
        @('-ConfirmDisposableGuest', '-PackageDir', (Join-Path $Folder 'driver'), '-Flavour', 'umdf') | Out-Null
    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq 'CN=Virtual Serial Studio guest test' } |
        Sort-Object NotBefore -Descending | Select-Object -First 1
    $exe = Join-Path $Folder 'helper\vsscom_helper.exe'
    $result = Set-AuthenticodeSignature -FilePath $exe -Certificate $cert -HashAlgorithm SHA256
    Write-Host "[$Name] helper signature $($result.Status)"
    $summary["sign-$Name-helper"] = "$($result.Status)"
}

function Set-Up([string] $Step, [string] $Folder, [string[]] $Extra = @()) {
    $code = Invoke-Script $Step (Join-Path $Folder 'setup-driver.ps1') (@('-PackageRoot', $Folder) + $Extra)
    Start-Sleep -Seconds 5
    Save-Step $Step
    $code
}

# Ports through the helper, as Studio makes them. A port lives as long as the pipe that made it.
. (Join-Path $work 'test\common.ps1')
. (Join-Path $work 'test\hub-io.ps1')
$pipes = @()
function New-HelperPort([string] $Label, [switch] $KeepOpen) {
    $e = 0
    $pipe = [WowHelper]::Open($HelperPipeName, [ref]$e)
    if (-not $pipe) { Write-Host "[$Label] could not open the helper pipe: $e"; $summary["port-$Label"] = "pipe error $e"; return }
    $slot = 0
    $e = $pipe.Create(0, [ref]$slot)
    Write-Host "[$Label] create: error $e slot $slot"
    $summary["port-$Label"] = [ordered]@{ error = $e; slot = $slot }
    if ($KeepOpen -and $e -eq 0) { $script:pipes += $pipe } else { $pipe.Dispose() }
}

$old = Join-Path $work 'old'
$new = Join-Path $work 'new'
Sign-Package $old 'old'
Sign-Package $new 'new'
Save-Step '0-start'

# 2. The old driver, and two ports: one still open, one closed (its device stays behind).
Set-Up '1-setup-old' $old | Out-Null
New-HelperPort 'a-closed'
New-HelperPort 'b-open' -KeepOpen
Start-Sleep -Seconds 3
Save-Step '2-ports-on-old'

# 3. Set up the newer one over it.
Set-Up '3-setup-new' $new | Out-Null
foreach ($pipe in $pipes) { $pipe.Dispose() }
$pipes = @()

# 4. New ports after the update, and the old devices they may reuse.
New-HelperPort 'c-after-update' -KeepOpen
New-HelperPort 'd-after-update' -KeepOpen
Start-Sleep -Seconds 3
Save-Step '4-ports-after-update'
foreach ($pipe in $pipes) { $pipe.Dispose() }
$pipes = @()

# 5 to 7. The same, an older one and a repair.
Set-Up '5-setup-new-again' $new | Out-Null
Set-Up '6-setup-old-downgrade' $old | Out-Null
Set-Up '7-setup-new-reinstall' $new @('-ReinstallDriver') | Out-Null

# 8. Everything goes, the older packages with it.
Invoke-Script '8-remove' (Join-Path $new 'remove-driver.ps1') @('-PackageRoot', $new) | Out-Null
Save-Step '8-after-remove'

Save-Json 'summary.json' $summary
Compress-Archive -Path $out -DestinationPath (Join-Path $root 'update-out.zip') -Force
exit 0
