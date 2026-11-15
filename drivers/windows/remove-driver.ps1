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
# helper service, the port devices of the user-mode driver (hardware ids VSSCOMUM\PORT
# and VSSCOMUM\VSSCOM0, class Ports; the helper's have instance ids SWD\VSSCOMUM\...) and
# the package whose original name is vsscom_um.inf are touched; the kernel-mode driver,
# certificates and the test-signing setting are not.
#
# The devices are removed with SetupAPI (DIF_REMOVE), the way the guest script
# test\uninstall-driver.ps1 does, which works on absent devices and on every Windows the
# driver supports. A device that cannot be removed does not stop the script: the helper
# and the package are taken away first, so that nothing is left with no way to remove it,
# and the devices that are left are reported at the end with exit code 7.
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

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Stop-With([int] $Code, [string] $Message) {
    if ($Code -eq 0 -or $Code -eq 10) { Write-Host $Message } else { [Console]::Error.WriteLine($Message) }
    exit $Code
}

# Runs a program with a time limit and returns its exit code, or 124 when it ran out
# of time (it is then stopped, with anything it started). Its output goes to the host,
# so it reaches the installer's log. The same function is in setup-driver.ps1.
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

# The port devices of the helper: every instance of its software device, present or absent. A port
# that is closed leaves its device behind (that is how its slot keeps its COM number), so after the
# helper has gone these are what is left, and a device that stays keeps the COM number "in use". No
# error is hidden: if the query itself fails the script ends with 1 and does not read it as "none".
function Get-PortDevices {
    @(Get-PnpDevice -InstanceId 'SWD\VSSCOMUM\*')
}

# Removes the devices of the user-mode driver's ports, the absent ones too, by hardware id and class
# (the code of drivers\windows\test\common.ps1, [VssSetup]::RemoveDevices). Returns how many. A device
# that cannot be removed throws, with the Windows error.
function Remove-PortDevices {
    $ports = [guid] '4D36E978-E325-11CE-BFC1-08002BE10318'
    $removed = 0
    foreach ($hardwareId in 'VSSCOMUM\PORT', 'VSSCOMUM\VSSCOM0') {
        $removed += [VssDeviceRemoval]::Remove($ports, $hardwareId)
    }
    $removed
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class VssDeviceRemoval
{
    const uint SPDRP_HARDWAREID = 1;
    const uint DIF_REMOVE = 5;
    static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA
    {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceRegistryProperty(IntPtr set, ref SP_DEVINFO_DATA data, uint property,
                                                        out uint type, byte[] buffer, uint size, out uint required);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    // Removes every device of the class whose hardware ids include hardwareId, present or absent.
    public static int Remove(Guid classGuid, string hardwareId)
    {
        // Flags 0: every device of the class, the ones that have gone included.
        IntPtr set = SetupDiGetClassDevs(ref classGuid, IntPtr.Zero, IntPtr.Zero, 0);
        if (set == InvalidHandle) throw new Win32Exception(Marshal.GetLastWin32Error(), "SetupDiGetClassDevs");
        int removed = 0;
        try
        {
            for (uint index = 0; ; index++)
            {
                var data = new SP_DEVINFO_DATA { cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA)) };
                if (!SetupDiEnumDeviceInfo(set, index, ref data)) break;
                var buffer = new byte[1024];
                uint type, required;
                if (!SetupDiGetDeviceRegistryProperty(set, ref data, SPDRP_HARDWAREID, out type, buffer,
                                                      (uint)buffer.Length, out required))
                    continue;
                string ids = Encoding.Unicode.GetString(buffer, 0, (int)required);
                if (ids.IndexOf(hardwareId, StringComparison.OrdinalIgnoreCase) < 0) continue;
                if (!SetupDiCallClassInstaller(DIF_REMOVE, set, ref data))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "DIF_REMOVE");
                removed++;
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return removed;
    }
}
'@

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

    # The port devices, absent ones included, before the package goes, because a device that is left
    # behind keeps its COM number reserved. A device that cannot be removed does not stop the script:
    # the package is deleted all the same (the installer deletes this script afterwards, so a package
    # left in the store would have nothing to remove it), and the devices are reported at the end.
    $restart = $false
    $deviceFailure = $null
    try {
        $devices = @(Get-PortDevices)
        Write-Host ('port devices: ' + $(if ($devices.Count -eq 0) { 'none left by the driver' } else { ($devices | ForEach-Object { $_.InstanceId }) -join ', ' }))
    }
    catch { $deviceFailure = "the port devices could not be listed: $($_.Exception.Message)"; Write-Host $deviceFailure }
    try { Write-Host ("removed {0} port device(s)" -f (Remove-PortDevices)) }
    catch { $deviceFailure = "a port device could not be removed: $($_.Exception.Message)"; Write-Host $deviceFailure }

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
    # Last, so that nothing above depends on it.
    $leftDevices = @(Get-PortDevices)
    if ($leftDevices.Count -gt 0 -and -not $restart) {
        $names = ($leftDevices | ForEach-Object { $_.InstanceId }) -join ', '
        Stop-With 7 ("a port device is still there, so its COM number stays in use: $names" + $(if ($deviceFailure) { " ($deviceFailure)" } else { '' }))
    }

    if ($restart) { Stop-With 10 'removed; restart Windows to finish removing the driver' }
    Stop-With 0 'removed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
