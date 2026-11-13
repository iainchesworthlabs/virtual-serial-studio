# SPDX-License-Identifier: GPL-3.0-or-later
#
# Closes the programs of Virtual Serial Studio that run from the install folder, and
# takes away what a removed Studio would otherwise leave in each user's own settings,
# for the Windows installer (driver.nsh).
#
#   stop-studio.ps1 -Action Check  -Directory "C:\Program Files\Virtual Serial Studio\bin"
#   stop-studio.ps1 -Action Stop   -Directory "C:\Program Files\Virtual Serial Studio\bin"
#   stop-studio.ps1 -Action Forget -Directory "C:\Program Files\Virtual Serial Studio\bin"
#
# Check   says whether vss_gui.exe or vss_cli.exe runs from that folder, in any
#         session: exit 1 when one does, 0 when none does. It changes nothing.
# Stop    ends those programs and waits for them to go. Windows cannot replace or
#         delete a program that runs, so an upgrade would keep the old vss_gui.exe and
#         an uninstall would leave the folder. A program of the same name that runs
#         from another folder (the zip, a build tree) is not touched. Studio writes
#         what runs to the user's running-setup.json a second after every change, so
#         the ports come back the next time it starts.
# Forget  is for a real uninstall, not an upgrade. For every user whose registry hive
#         is loaded it deletes the "Start when I sign in" entry (the value
#         "Virtual Serial Studio" under ...\CurrentVersion\Run) when that entry starts
#         the program in this folder. And in every user's running-setup.json it
#         records that the ports were stopped, as File, Quit does, so a Studio that is
#         installed again offers to start them and does not start them by itself. The
#         file is not deleted: it can be the only copy of a setup nobody saved.
#
# Forget writes as an administrator in folders that ordinary users own, so it is
# careful there: it never deletes, never follows a link (a folder or file that is a
# reparse point is skipped), and writes a file only when that file holds the exact
# text it is there to change.
#
# -Names, -UsersKey, -RunSubKey and -DataFolders exist for the tests
# (tools/test_stop_studio.py), which point them at scratch places.
#
# Exit codes: 0 done or nothing to do (Check: nothing runs), 1 Check: a program runs,
# 2 something could not be read or written, 4 Stop: a program is still running.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Check', 'Stop', 'Forget')][string]$Action,
    [Parameter(Mandatory)][string]$Directory,
    [string[]]$Names = @('vss_gui.exe', 'vss_cli.exe'),
    [int]$Seconds = 20,
    # A key under HKEY_CURRENT_USER that stands in for HKEY_USERS. Empty: the real one.
    [string]$UsersKey = '',
    [string]$RunSubKey = 'Software\Microsoft\Windows\CurrentVersion\Run',
    [string]$RunValue = 'Virtual Serial Studio',
    # The folders that may hold running-setup.json. Empty: one for each user profile.
    [string[]]$DataFolders = @()
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$folder = $Directory.Trim().TrimEnd('\')

function Test-InFolder([string]$program)
{
    if (-not $program) { return $false }
    $parent = Split-Path -Parent $program
    return $parent -and ($parent.TrimEnd('\') -ieq $folder)
}

# The programs of ours that run from the folder, in every session.
function Get-Running
{
    return @(Get-CimInstance -ClassName Win32_Process |
            Where-Object { ($Names -contains $_.Name) -and (Test-InFolder $_.ExecutablePath) })
}

# The program a Run entry starts: the text between the first pair of quotes, or all
# of it up to the first option when there are none.
function Get-Program([string]$command)
{
    $text = $command.Trim()
    if ($text.StartsWith('"'))
    {
        $end = $text.IndexOf('"', 1)
        if ($end -lt 0) { return '' }
        return $text.Substring(1, $end - 1)
    }
    $option = $text.IndexOf(' --')
    if ($option -ge 0) { return $text.Substring(0, $option).Trim() }
    return $text
}

function Remove-SignInEntries
{
    if ($UsersKey)
    {
        $users = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($UsersKey)
        if ($null -eq $users) { throw "The registry key HKEY_CURRENT_USER\$UsersKey does not exist." }
    }
    else
    {
        $users = [Microsoft.Win32.Registry]::Users
    }
    foreach ($sid in $users.GetSubKeyNames())
    {
        # The accounts of people: not the system accounts, not the _Classes hives.
        if ($sid -notmatch '^S-1-5-21-[0-9-]+$') { continue }
        $run = $null
        try { $run = $users.OpenSubKey("$sid\$RunSubKey", $true) }
        catch { Write-Host "${sid}: the Run key could not be opened: $($_.Exception.Message)"; continue }
        if ($null -eq $run) { continue }
        try
        {
            $command = $run.GetValue($RunValue, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($command -isnot [string]) { continue }
            if (Test-InFolder (Get-Program $command))
            {
                $run.DeleteValue($RunValue, $false)
                Write-Host "${sid}: the sign-in entry was removed."
            }
            else
            {
                Write-Host "${sid}: the sign-in entry starts a Studio in another folder and was left."
            }
        }
        finally
        {
            $run.Close()
        }
    }
}

function Get-DataFolders
{
    if ($DataFolders.Count -gt 0) { return $DataFolders }
    $found = @()
    $list = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList')
    if ($null -eq $list) { return $found }
    foreach ($sid in $list.GetSubKeyNames())
    {
        if ($sid -notmatch '^S-1-5-21-[0-9-]+$') { continue }
        $entry = $list.OpenSubKey($sid)
        if ($null -eq $entry) { continue }
        $userFolder = [string]$entry.GetValue('ProfileImagePath', '')
        $entry.Close()
        if ($userFolder) { $found += (Join-Path $userFolder 'AppData\Roaming\iainchesworthlabs\Virtual Serial Studio') }
    }
    $list.Close()
    return $found
}

function Test-Link([string]$path)
{
    $item = Get-Item -LiteralPath $path -Force
    return ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
}

function Set-RunningSetupsStopped
{
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $running = [regex]'"stopped"\s*:\s*false'
    foreach ($data in @(Get-DataFolders))
    {
        $file = Join-Path $data 'running-setup.json'
        try
        {
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
            if ((Test-Link $data) -or (Test-Link (Split-Path -Parent $data)) -or (Test-Link $file))
            {
                Write-Host "$file is behind a link and was left."
                continue
            }
            $text = [System.IO.File]::ReadAllText($file, $utf8)
            if (-not $running.IsMatch($text))
            {
                Write-Host "$file does not say that ports run and was left."
                continue
            }
            [System.IO.File]::WriteAllText($file, $running.Replace($text, '"stopped": true', 1), $utf8)
            Write-Host "$file now says the ports were stopped."
        }
        catch
        {
            # One user's file must not stop the others, or the uninstall.
            Write-Host "$file could not be changed: $($_.Exception.Message)"
        }
    }
}

try
{
    if ($Action -eq 'Check')
    {
        $running = @(Get-Running)
        foreach ($process in $running) { Write-Host "Running: $($process.ExecutablePath) (process $($process.ProcessId))" }
        if ($running.Count -gt 0) { exit 1 }
        exit 0
    }

    if ($Action -eq 'Stop')
    {
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ($true)
        {
            $running = @(Get-Running)
            if ($running.Count -eq 0) { break }
            if ((Get-Date) -ge $deadline)
            {
                Write-Host "A program was still running after $Seconds seconds."
                exit 4
            }
            foreach ($process in $running)
            {
                Write-Host "Ending $($process.ExecutablePath) (process $($process.ProcessId))"
                Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
            }
            Start-Sleep -Milliseconds 250
        }
        Write-Host 'No program of Studio runs from the folder.'
        exit 0
    }

    Remove-SignInEntries
    Set-RunningSetupsStopped
    exit 0
}
catch
{
    Write-Host "Could not finish: $($_.Exception.Message)"
    exit 2
}
