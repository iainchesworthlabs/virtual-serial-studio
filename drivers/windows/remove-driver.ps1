# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Takes away what setup-driver.ps1 put on the machine: the helper service, the
# ports it made, the port devices that stay behind when a port goes (each keeps its
# COM number reserved), and the user-mode driver package. It is what the uninstaller of
# Virtual Serial Studio runs, and it can be run by hand from a terminal opened as
# administrator. Running it on a machine that has none of it is not an error.
#
#   remove-driver.ps1 [-PackageRoot <folder>]
#
# The package folder is the one setup-driver.ps1 took its files from. Only the
# helper service, the devices whose instance id begins SWD\VSSCOMUM\ and the package
# whose original name is vsscom_um.inf are touched; the kernel-mode driver, certificates
# and the test-signing setting are not. Removing a device needs Windows 10 version 2004
# or later (pnputil /remove-device).
#
# Exit codes:
#   0   done: no helper service, no port device and no vsscom_um package remain
#   10  done, and Windows asks for a restart to finish removing the driver
#   2   not run as an administrator
#   7   a port device or the driver package could not be removed
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

# A test defines these two functions before it runs the script, so the script can be tried on
# a machine where nobody is an administrator and no program is started. Nothing else reads them.
if (-not (Test-Path -LiteralPath 'Function:\Test-IsAdministrator')) {
    function Test-IsAdministrator {
        $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
}

function Stop-With([int] $Code, [string] $Message) {
    if ($Code -eq 0 -or $Code -eq 10) { Write-Host $Message } else { [Console]::Error.WriteLine($Message) }
    exit $Code
}

# Runs a program with a time limit and returns its exit code, or 124 when it ran out
# of time (it is then stopped, with anything it started). Its output goes to the host,
# so it reaches the installer's log. The same function is in setup-driver.ps1.
if (-not (Test-Path -LiteralPath 'Function:\Invoke-Bounded')) {
function Invoke-Bounded([string] $File, [string[]] $Arguments, [int] $Seconds) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = (($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($Seconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Write-Host "$File did not finish in $Seconds seconds and was stopped"
        return 124
    }
    $process.WaitForExit()
    foreach ($text in $out.Result, $err.Result) { if ($text.Trim()) { Write-Host $text.Trim() } }
    return $process.ExitCode
}
}

# The port devices: every instance of the helper's software device, present or absent. A port that is
# closed leaves its device behind (that is how its slot keeps its COM number), so after the helper has
# gone these are what is left, and a device that stays keeps the COM number "in use".
function Get-PortDevices {
    @(Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'SWD\VSSCOMUM\*' })
}

# The whole script ends with exit code 124 after ten minutes, whatever it waits for. A
# timer thread does it, because the script may be blocked inside a call that never returns.
Add-Type -TypeDefinition 'public static class VssWatchdog { static System.Threading.Timer timer_; public static void Start(int milliseconds) { timer_ = new System.Threading.Timer(delegate { System.Console.Error.WriteLine("remove-driver.ps1 did not finish in time and was stopped"); System.Environment.Exit(124); }, null, milliseconds, -1); } }'
[VssWatchdog]::Start(600000)

if (-not (Test-IsAdministrator)) {
    Stop-With 2 'run this as an administrator'
}

$serviceName = 'vsscom_helper'

try {
    # The helper first: the ports it made go when it stops, and the driver package
    # cannot be removed while they exist.
    $installHelper = Join-Path $PackageRoot 'helper\install-helper.ps1'
    if (Test-Path -LiteralPath $installHelper -PathType Leaf) {
        # install-helper.ps1 ends with an exit code, but a throw out of it must give the same answer.
        $global:LASTEXITCODE = 0
        $helperFailure = $null
        try { & $installHelper -Uninstall }
        catch { $helperFailure = $_.Exception.Message }
        if ($helperFailure) { Stop-With 8 "install-helper.ps1 -Uninstall failed: $helperFailure" }
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

    # The port devices, absent ones included, before the package goes: a device that is left behind
    # keeps its COM number reserved. pnputil /remove-device does what the guest test script's
    # DIF_REMOVE does.
    $restart = $false
    $devices = @(Get-PortDevices)
    if ($devices.Count -eq 0) { Write-Host 'no port device left by the driver' }
    foreach ($device in $devices) {
        Write-Host "removing the port device $($device.InstanceId)"
        $code = Invoke-Bounded 'pnputil.exe' @('/remove-device', $device.InstanceId) 120
        if ($code -eq 3010) { $restart = $true }
        elseif ($code -eq 124) { Stop-With 7 "pnputil /remove-device $($device.InstanceId) did not finish in 120 seconds and was stopped" }
        elseif ($code -ne 0) { Stop-With 7 "pnputil /remove-device $($device.InstanceId) failed with $code" }
    }

    # The driver packages, found in the store by their original file name.
    $published = @(Get-WindowsDriver -Online -All |
        Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' } |
        ForEach-Object { $_.Driver })
    if ($published.Count -eq 0) { Write-Host 'no vsscom_um driver package in the driver store' }

    foreach ($name in $published) {
        Write-Host "deleting the driver package $name"
        $code = Invoke-Bounded 'pnputil.exe' @('/delete-driver', $name, '/uninstall', '/force') 300
        if ($code -eq 3010) { $restart = $true }
        elseif ($code -eq 124) { Stop-With 7 "pnputil /delete-driver $name did not finish in 300 seconds and was stopped" }
        elseif ($code -ne 0) { Stop-With 7 "pnputil /delete-driver $name failed with $code" }
    }

    # Check, so a package that is still there is reported and not assumed gone.
    $left = @(Get-WindowsDriver -Online -All |
        Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' })
    if ($left.Count -gt 0 -and -not $restart) { Stop-With 7 'the vsscom_um driver package is still in the driver store' }
    $leftDevices = @(Get-PortDevices)
    if ($leftDevices.Count -gt 0 -and -not $restart) { Stop-With 7 ('a port device is still there: ' + (($leftDevices | ForEach-Object { $_.InstanceId }) -join ', ')) }

    if ($restart) { Stop-With 10 'removed; restart Windows to finish removing the driver' }
    Stop-With 0 'removed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
