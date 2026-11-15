# SPDX-License-Identifier: GPL-3.0-or-later
#
# Adds a folder to, or takes it out of, the machine PATH for the Windows installer
# (driver.nsh). CPack's own NSIS code does this in NSIS strings, which stop at 1024
# characters; a PATH longer than that made it show "PATH too long installer unable to
# modify PATH!" and, on the way out, can cut a long PATH short. This script works on
# the registry value itself, whatever its length.
#
#   edit-path.ps1 -Action Add    -Directory "C:\Program Files\Virtual Serial Studio\bin"
#   edit-path.ps1 -Action Remove -Directory "C:\Program Files\Virtual Serial Studio\bin"
#
# It changes nothing else: every other entry is kept as written (a %SystemRoot%
# stays a %SystemRoot%), the value keeps its type (REG_EXPAND_SZ, or REG_SZ if that is
# what it was; a PATH with no value gets REG_EXPAND_SZ), and a folder that is already
# there, in any case, with or without a trailing backslash or quotes, is not added a
# second time. Empty entries (a trailing or a doubled semicolon) are dropped when the
# script writes the PATH. When the result would be longer than
# Windows allows in a registry value the PATH is left untouched and the exit code is
# 3. -Hive and -SubKey exist for the tests, which point them at a scratch key.
#
# Exit codes: 0 done or nothing to do, 2 the registry could not be read or written,
# 3 the PATH would be too long.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Add', 'Remove')][string]$Action,
    [Parameter(Mandatory)][string]$Directory,
    [ValidateSet('LocalMachine', 'CurrentUser')][string]$Hive = 'LocalMachine',
    [string]$SubKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# The most characters a registry string value may hold.
$maxLength = 32767

function Get-Entries([string]$value)
{
    if ($value -eq '') { return @() }
    return @($value.Split(';'))
}

function Test-Same([string]$entry, [string]$folder)
{
    return $entry.Trim().Trim('"').TrimEnd('\') -ieq $folder
}

try
{
    $folder = $Directory.Trim().TrimEnd('\')
    $root = if ($Hive -eq 'LocalMachine') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
    $key = $root.OpenSubKey($SubKey, $true)
    if ($null -eq $key) { throw "The registry key $Hive\$SubKey does not exist." }
    try
    {
        # Read without expanding, so that %SystemRoot% and the like survive.
        $current = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $entries = Get-Entries $current
        # The type the value has now. A key with no Path value gets the type Windows uses.
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        if ($key.GetValueNames() -contains 'Path')
        {
            $found = $key.GetValueKind('Path')
            if ($found -eq [Microsoft.Win32.RegistryValueKind]::String) { $kind = $found }
        }
        $present = @($entries | Where-Object { Test-Same $_ $folder }).Count -gt 0

        if ($Action -eq 'Add')
        {
            if ($present)
            {
                Write-Host "$folder is already on the PATH."
                exit 0
            }
            # @() makes a list of one entry a list: a single item out of a pipeline is a
            # string, and a string plus a string is one longer string, not two entries.
            $kept = @($entries | Where-Object { $_.Trim() -ne '' })
            $new = ($kept + $folder) -join ';'
        }
        else
        {
            if (-not $present)
            {
                Write-Host "$folder is not on the PATH."
                exit 0
            }
            $kept = @($entries | Where-Object { $_.Trim() -ne '' -and -not (Test-Same $_ $folder) })
            $new = $kept -join ';'
        }

        if ($new.Length -gt $maxLength)
        {
            Write-Host "The PATH would be $($new.Length) characters, more than the $maxLength Windows allows. It was not changed."
            exit 3
        }
        $key.SetValue('Path', $new, $kind)
        Write-Host "$Action done: the PATH is now $($new.Length) characters."
    }
    finally
    {
        $key.Close()
    }
}
catch
{
    Write-Host "Could not change the PATH: $($_.Exception.Message)"
    exit 2
}
