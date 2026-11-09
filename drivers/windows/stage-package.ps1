# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Puts the user-mode driver package, the helper service and the scripts that set
# them up in the folder that Studio looks in: <install tree>\bin\driver, beside
# vss_gui.exe. That is the layout setup-driver.ps1 and the Windows installer
# expect (drivers/windows/README.md, "Setting up from Studio").
#
#   stage-package.ps1 -Out <folder> [-Platform x64|ARM64] [-Artifacts <folder>]
#
#   <Out>\setup-driver.ps1, remove-driver.ps1
#   <Out>\driver\vsscom_um.inf, vsscom_um.dll, vsscom_um.cat
#   <Out>\helper\vsscom_helper.exe, install-helper.ps1
#   <Out>\trust\install-trust.ps1
#
# The packages come from build.ps1 (drivers\windows\artifacts\package-um and
# package-helper) and are signed, or not, before this runs. Nothing is changed in
# them here. The script fails when a file is missing, so a release cannot go out
# with half a driver.

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Out,
    [ValidateSet('x64', 'ARM64')] [string] $Platform = 'x64',
    [string] $Artifacts = (Join-Path $PSScriptRoot 'artifacts')
)

$ErrorActionPreference = 'Stop'

function Copy-Required([string] $From, [string] $ToFolder) {
    if (-not (Test-Path -LiteralPath $From -PathType Leaf)) { throw "missing: $From" }
    New-Item -ItemType Directory -Force -Path $ToFolder | Out-Null
    Copy-Item -LiteralPath $From -Destination $ToFolder -Force
}

$um = Join-Path $Artifacts "package-um\$Platform"
$helper = Join-Path $Artifacts "package-helper\$Platform"

Copy-Required (Join-Path $PSScriptRoot 'setup-driver.ps1') $Out
Copy-Required (Join-Path $PSScriptRoot 'remove-driver.ps1') $Out
foreach ($file in 'vsscom_um.inf', 'vsscom_um.dll', 'vsscom_um.cat') {
    Copy-Required (Join-Path $um $file) (Join-Path $Out 'driver')
}
Copy-Required (Join-Path $helper 'vsscom_helper.exe') (Join-Path $Out 'helper')
Copy-Required (Join-Path $PSScriptRoot 'vsscom_helper\install-helper.ps1') (Join-Path $Out 'helper')
Copy-Required (Join-Path $PSScriptRoot 'trust\install-trust.ps1') (Join-Path $Out 'trust')

Get-ChildItem -LiteralPath $Out -Recurse -File | ForEach-Object { $_.FullName.Substring($Out.Length).TrimStart('\') }
