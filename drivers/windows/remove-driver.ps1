# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Takes away what setup-driver.ps1 put on the machine: the helper service, the
# ports it made, and the user-mode driver package. It is what the uninstaller of
# Virtual Serial Studio runs, and it can be run by hand from a terminal opened as
# administrator. Running it on a machine that has none of it is not an error.
#
#   remove-driver.ps1 [-PackageRoot <folder>]
#
# The package folder is the one setup-driver.ps1 took its files from. Only the
# helper service and the package whose original name is vsscom_um.inf are touched;
# the kernel-mode driver, certificates and the test-signing setting are not.
#
# Exit codes:
#   0   done: no helper service and no vsscom_um package remain
#   10  done, and Windows asks for a restart to finish removing the driver
#   2   not run as an administrator
#   7   the driver package could not be removed
#   8   the helper service could not be removed
#   1   anything else

[CmdletBinding()]
param(
    [string] $PackageRoot
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty in a parameter default when the
# script is started with -File, so the default is set in the body.
if (-not $PackageRoot) { $PackageRoot = $PSScriptRoot }

function Stop-With([int] $Code, [string] $Message) {
    if ($Code -eq 0 -or $Code -eq 10) { Write-Host $Message } else { [Console]::Error.WriteLine($Message) }
    exit $Code
}

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-With 2 'run this as an administrator'
}

$serviceName = 'vsscom_helper'

try {
    # The helper first: the ports it made go when it stops, and the driver package
    # cannot be removed while they exist.
    $installHelper = Join-Path $PackageRoot 'helper\install-helper.ps1'
    if (Test-Path -LiteralPath $installHelper -PathType Leaf) {
        & $installHelper -Uninstall
        if ($LASTEXITCODE -ne 0) { Stop-With 8 "install-helper.ps1 -Uninstall failed with $LASTEXITCODE" }
    }
    elseif (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
        & sc.exe stop $serviceName | Out-Null
        for ($i = 0; $i -lt 80 -and (Get-Service -Name $serviceName).Status -ne 'Stopped'; $i++) { Start-Sleep -Milliseconds 500 }
        & sc.exe delete $serviceName | Out-Null
        for ($i = 0; $i -lt 40 -and (Get-Service -Name $serviceName -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
        if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) { Stop-With 8 "the $serviceName service did not go" }
        Write-Host "$serviceName removed"
    }
    else {
        Write-Host "no $serviceName service"
    }

    # The driver packages, found in the store by their original file name.
    $published = @(Get-WindowsDriver -Online -All |
        Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' } |
        ForEach-Object { $_.Driver })
    if ($published.Count -eq 0) { Write-Host 'no vsscom_um driver package in the driver store' }

    $restart = $false
    foreach ($name in $published) {
        $out = & pnputil.exe /delete-driver $name /uninstall /force 2>&1
        $code = $LASTEXITCODE
        Write-Host (($out | Out-String).Trim())
        if ($code -eq 3010) { $restart = $true }
        elseif ($code -ne 0) { Stop-With 7 "pnputil /delete-driver $name failed with $code" }
    }

    # Check, so a package that is still there is reported and not assumed gone.
    $left = @(Get-WindowsDriver -Online -All |
        Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' })
    if ($left.Count -gt 0 -and -not $restart) { Stop-With 7 'the vsscom_um driver package is still in the driver store' }

    if ($restart) { Stop-With 10 'removed; restart Windows to finish removing the driver' }
    Stop-With 0 'removed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
