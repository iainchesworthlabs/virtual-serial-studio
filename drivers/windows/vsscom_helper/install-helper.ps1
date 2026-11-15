# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs or removes the vsscom_helper service: the program that makes the ports
# of the user-mode driver for ordinary users (drivers/windows/vsscom_helper/
# protocol.h says how). Run it as an administrator, from the folder that holds
# vsscom_helper.exe, or name the file.
#
#   install-helper.ps1 [-ExePath <vsscom_helper.exe>] [-InstallDir <folder>]
#   install-helper.ps1 -Uninstall [-InstallDir <folder>]
#
# The program is copied to a folder that only administrators can write
# (Program Files by default), because the service runs it as SYSTEM. The service
# starts with Windows and restarts itself if it stops by accident. Nothing else
# is changed: the user-mode driver is a separate package.
#
# Exit codes:
#   0   the service is installed and running (or removed)
#   2   not run as an administrator
#   1   anything else (the reason is written to the error stream)
#
# The service is registered with its path in quotes. The folder name has spaces, and
# a path without quotes makes Windows try C:\Program.exe first, as SYSTEM. The path is
# not passed to sc.exe: Windows PowerShell 5.1 hands sc.exe the argument with its quotes
# stripped. New-Service passes the string to the service manager as it is, and the
# script reads the stored path back and stops if it does not begin with a quote.

[CmdletBinding()]
param(
    [string] $ExePath,
    [string] $InstallDir,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty in a parameter default when the
# script is started with -File, so the default is set in the body.
if (-not $ExePath) { $ExePath = Join-Path $PSScriptRoot 'vsscom_helper.exe' }
# A 64-bit PowerShell started by a 32-bit program (the installer) inherits ProgramFiles as the
# x86 folder; ProgramW6432 is always the 64-bit one.
if (-not $InstallDir) {
    $programs = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $InstallDir = Join-Path $programs 'Virtual Serial Studio\Helper'
}
$name = 'vsscom_helper'

# A test defines this function before it runs the script, so the script can be tried on a
# machine where nobody is an administrator and nothing real is touched.
if (-not (Test-Path -LiteralPath 'Function:\Test-IsAdministrator')) {
    function Test-IsAdministrator {
        $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
}

# Write-Error would throw here, because $ErrorActionPreference is Stop, and the exit that
# follows it would never run. The message goes to the error stream directly.
if (-not (Test-IsAdministrator)) {
    [Console]::Error.WriteLine('run this as an administrator')
    exit 2
}

function Wait-ServiceState([string] $State, [int] $Seconds = 30) {
    $until = (Get-Date).AddSeconds($Seconds)
    do {
        $service = Get-Service -Name $name -ErrorAction SilentlyContinue
        if ($State -eq 'Gone' -and -not $service) { return $true }
        if ($service -and $service.Status.ToString() -eq $State) { return $true }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $until)
    $false
}

function Remove-HelperService {
    if (Get-Service -Name $name -ErrorAction SilentlyContinue) {
        & sc.exe stop $name | Out-Null
        Wait-ServiceState 'Stopped' 40 | Out-Null
        & sc.exe delete $name | Out-Null
        if (-not (Wait-ServiceState 'Gone' 20)) { throw "the $name service did not go" }
    }
}

try {
    if ($Uninstall) {
        Remove-HelperService
        if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
        Write-Host "$name removed"
        exit 0
    }

    if (-not (Test-Path $ExePath)) { throw "$ExePath not found" }
    # A file that is not signed is fine on a development machine and worth a word.
    $signature = Get-AuthenticodeSignature $ExePath
    if ($signature.Status -ne 'Valid') {
        Write-Warning "vsscom_helper.exe is not signed by a trusted publisher ($($signature.Status))"
    }

    Remove-HelperService
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $target = Join-Path $InstallDir 'vsscom_helper.exe'
    Copy-Item $ExePath $target -Force

    # Only SYSTEM and administrators may change the folder or the program.
    & icacls.exe $InstallDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'icacls failed' }

    # The path is stored in quotes (see the top of this file). The service runs as LocalSystem,
    # which is what New-Service gives it when no credential is named.
    New-Service -Name $name -BinaryPathName "`"$target`"" -DisplayName 'Virtual Serial Studio port helper' -StartupType Automatic `
        -Description 'Makes virtual serial ports for the user-mode driver, for ordinary users.' | Out-Null
    $stored = (Get-CimInstance -ClassName Win32_Service -Filter "Name='$name'").PathName
    if (-not $stored -or -not $stored.StartsWith('"')) {
        # Do not leave a service that runs as SYSTEM with a path Windows would search.
        Remove-HelperService
        throw "the $name service was registered with a path that is not in quotes ($stored)"
    }
    # Restart after a stop that was not asked for; the ports of the stopped helper are gone.
    & sc.exe failure $name reset= 86400 actions= restart/5000/restart/5000/none/0 | Out-Null
    & sc.exe start $name | Out-Null
    if (-not (Wait-ServiceState 'Running' 30)) { throw "the $name service did not start" }
    Write-Host "$name installed in $InstallDir and running"
    exit 0
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
