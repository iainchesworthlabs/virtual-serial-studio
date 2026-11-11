#!/usr/bin/env pwsh
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
<#
.SYNOPSIS
    Packs the Windows driver packages, the guest test scripts and the test client into the zip
    that tools/vm/guest-cell.ps1 -DriverBundle copies into a Windows guest.

.DESCRIPTION
    The zip holds one self-contained folder per driver, because the guest scripts look for the
    driver files, the helper scripts and the test client next to themselves:

      kmdf\   vsscom.sys, vsscom.inf, vsscomctl.inf and the catalogs, the scripts of
              drivers/windows/test, public.h, and vss_com_client_x86.exe / _x64.exe
      umdf\   vsscom_um.dll, vsscom_um.inf, vsscom_um.cat, vsscom_helper.exe,
              install-helper.ps1, and the same scripts and clients

    The packages come from `drivers/windows/build.ps1` (drivers\windows\artifacts\package,
    package-um and package-helper) or from the `vsscom-*` artifacts of the CI driver job. A
    folder left out is left out of the zip, and the guest run skips that driver. The client is
    built here with MSVC, as Win32 and x64, unless -Client names a folder that already holds
    the programs (the CI artifact `vss-com-client`).

    Nothing is signed here. prepare-test-signing.ps1 signs the package in the guest with a
    throwaway certificate, and the guest reverts to `clean` afterwards.

.PARAMETER Repository
    The checkout that holds drivers/windows.

.PARAMETER Out
    The zip to write.

.PARAMETER Kmdf
    The kernel-mode package folder (vsscom.sys and its files). Default:
    <Repository>\drivers\windows\artifacts\package\x64.

.PARAMETER Um
    The user-mode package folder. Default: <Repository>\drivers\windows\artifacts\package-um\x64.

.PARAMETER Helper
    The helper service folder. Default: <Repository>\drivers\windows\artifacts\package-helper\x64.

.PARAMETER Client
    A folder with vss_com_client_x86.exe and vss_com_client_x64.exe. Built when left out.

.PARAMETER UnitTests
    Optional. A vss_unit_tests.exe that needs no runtime installed (a release build with the
    static C runtime). With it the guest run also runs test-client.ps1, the studio's own client
    against the driver (tests C1 to C3).

.EXAMPLE
    ./drivers/windows/build.ps1 -Driver all
    ./tools/vm/make-driver-bundle.ps1 -Repository . -Out D:\bundle-driver\driver-bundle.zip
    ./tools/vm/guest-cell.ps1 -Guest windows11 -DriverBundle D:\bundle-driver\driver-bundle.zip
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Repository,
    [Parameter(Mandatory)] [string] $Out,
    [string] $Kmdf,
    [string] $Um,
    [string] $Helper,
    [string] $Client,
    [string] $UnitTests,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'vss-driver-bundle')
)

$ErrorActionPreference = 'Stop'
$Repository = (Resolve-Path -LiteralPath $Repository).Path
$windows = Join-Path $Repository 'drivers\windows'
$tests = Join-Path $windows 'test'
$artifacts = Join-Path $windows 'artifacts'
if (-not $Kmdf) { $Kmdf = Join-Path $artifacts 'package\x64' }
if (-not $Um) { $Um = Join-Path $artifacts 'package-um\x64' }
if (-not $Helper) { $Helper = Join-Path $artifacts 'package-helper\x64' }

# The test client, once per architecture the guests run.
if (-not $Client) {
    $Client = Join-Path $Work 'client'
    New-Item -ItemType Directory -Force $Client | Out-Null
    foreach ($platform in 'Win32', 'x64') {
        $build = Join-Path $Work "client-build-$platform"
        & cmake -S (Join-Path $tests 'client32') -B $build -A $platform
        if ($LASTEXITCODE -ne 0) { throw "cmake failed for $platform" }
        & cmake --build $build --config Release
        if ($LASTEXITCODE -ne 0) { throw "the client did not build for $platform" }
        Copy-Item (Join-Path $build 'Release\vss_com_client_*.exe') $Client
    }
}
foreach ($name in 'vss_com_client_x86.exe', 'vss_com_client_x64.exe') {
    if (-not (Test-Path (Join-Path $Client $name))) { throw "$name is not in $Client" }
}

if ($UnitTests -and -not (Test-Path -LiteralPath $UnitTests)) { throw "no such file: $UnitTests" }

$stage = Join-Path $Work 'stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

function Add-Driver([string] $Name, [string[]] $PackageFolders) {
    $present = @($PackageFolders | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
    if ($present.Count -eq 0) {
        Write-Warning "no package for $Name ($($PackageFolders -join ', ')); the guest run will skip it"
        return
    }
    $folder = Join-Path $stage $Name
    New-Item -ItemType Directory -Force $folder | Out-Null
    foreach ($source in $present) { Copy-Item (Join-Path $source '*') $folder -Recurse -Force }
    Copy-Item (Join-Path $tests '*.ps1') $folder -Force
    Copy-Item (Join-Path $windows 'vsscom\public.h') $folder -Force
    Copy-Item (Join-Path $Client 'vss_com_client_*.exe') $folder -Force
    if ($UnitTests) { Copy-Item -LiteralPath $UnitTests (Join-Path $folder 'vss_unit_tests.exe') -Force }
}

Add-Driver 'kmdf' @($Kmdf)
Add-Driver 'umdf' @($Um, $Helper)

$items = Get-ChildItem $stage
if ($items.Count -eq 0) { throw 'no driver package was found; run drivers/windows/build.ps1 or pass -Kmdf and -Um' }

$Out = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
Remove-Item $Out -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $Out -Force
Get-Item $Out | ForEach-Object { '{0}  {1:N1} MB  ({2})' -f $_.FullName, ($_.Length / 1MB), (($items | ForEach-Object Name) -join ', ') }
