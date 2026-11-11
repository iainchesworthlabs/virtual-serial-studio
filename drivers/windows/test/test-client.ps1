# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the studio's own client against the installed driver: the one unit test
# that needs a driver (create_driver_port, the transport, the lines, and the
# port going when the transport closes). Run inside a guest, as an administrator,
# after install-driver.ps1, with a vss_unit_tests.exe built for Windows next to
# this script or named by -Exe. A static release build has no runtime to install.
#
#   test-client.ps1 [-Exe <vss_unit_tests.exe>] [-ResultPath <json>]
#
# Exit code 0 when the case ran and passed; a skipped case fails here, because it
# means the driver was not seen.

[CmdletBinding()]
param(
    [string] $Exe = (Join-Path $PSScriptRoot 'vss_unit_tests.exe'),
    [string] $ResultPath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

# Runs the cases whose name holds $Filter and returns what they printed and the exit code.
# A run that is still going after five minutes is ended and counts as failed (exit code -1):
# the cases take seconds, and a client that waits for a driver for ever must not hold the
# guest run.
function Invoke-Case([string] $Filter) {
    $out = [IO.Path]::GetTempFileName()
    $err = [IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $Exe -ArgumentList "`"--filter=$Filter`"" -NoNewWindow -PassThru `
            -RedirectStandardOutput $out -RedirectStandardError $err
        # Windows PowerShell gives no exit code unless the handle was asked for while it ran.
        $null = $process.Handle
        $code = -1
        $note = ''
        if ($process.WaitForExit(300000)) {
            $process.WaitForExit()
            $code = $process.ExitCode
        } else {
            $process.Kill()
            $process.WaitForExit()
            $note = "`nstill running after 300 s; ended"
        }
        $text = ((Get-Content $out -Raw) + (Get-Content $err -Raw) + $note).Trim()
        [pscustomobject]@{ Output = $text; Code = $code }
    } finally {
        Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
    }
}

$tests = @()
try {
    if (-not (Test-Path $Exe)) { throw "$Exe is not there" }
    $env:VSS_TEST_DRIVER = '1'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Case 'a port is made, carries bytes and goes'
    $output = $run.Output
    $code = $run.Code
    $detail = ($output -split "\r?\n" | Select-Object -Last 6) -join '; '
    $ran = $output -match '1 tests?, 1 passed, 0 failed, 0 skipped'
    $status = if ($code -eq 0 -and $ran) { 'pass' } else { 'fail' }
    $tests += New-TestRecord 'C1' 'the studio client makes a port, carries bytes both ways and removes it' $status "$detail ($($watch.Elapsed.TotalSeconds.ToString('0.0')) s)"

    # C3: a line error raised through the studio's transport reaches a program.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Case 'a line error reaches the program'
    $output = $run.Output
    $code = $run.Code
    $detail = ($output -split "\r?\n" | Select-Object -Last 6) -join '; '
    $ran = $output -match '1 tests?, 1 passed, 0 failed, 0 skipped'
    $status = if ($code -eq 0 -and $ran) { 'pass' } else { 'fail' }
    $tests += New-TestRecord 'C3' 'the studio client raises line errors that a program sees with ClearCommError and WaitCommEvent' $status "$detail ($($watch.Elapsed.TotalSeconds.ToString('0.0')) s)"

    # C4: the studio's hang-up and disconnect on a driver port, as a program sees them.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Case 'a hang-up and a disconnect reach the program'
    $output = $run.Output
    $code = $run.Code
    $detail = ($output -split "\r?\n" | Select-Object -Last 6) -join '; '
    $ran = $output -match '1 tests?, 1 passed, 0 failed, 0 skipped'
    $status = if ($code -eq 0 -and $ran) { 'pass' } else { 'fail' }
    $tests += New-TestRecord 'C4' 'the studio drops DCD and DSR for a hang-up and takes the port back for a disconnect' $status "$detail ($($watch.Elapsed.TotalSeconds.ToString('0.0')) s)"

    # C2: COM numbers and the program on the port, through the helper service. Without
    # the helper service it is skipped.
    $helperRunning = [bool]([IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$HelperPipeName")
    if ($helperRunning) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $run = Invoke-Case 'COM numbers with the helper service'
        $output = $run.Output
        $code = $run.Code
        $detail = ($output -split "\r?\n" | Select-Object -Last 6) -join '; '
        $ran = $output -match '1 tests?, 1 passed, 0 failed, 0 skipped'
        $status = if ($code -eq 0 -and $ran) { 'pass' } else { 'fail' }
        $tests += New-TestRecord 'C2' 'the studio client gives a port a COM number, changes it, and sees the program on the port' $status "$detail ($($watch.Elapsed.TotalSeconds.ToString('0.0')) s)"
    } else {
        $tests += New-TestRecord 'C2' 'the studio client gives a port a COM number' 'skip' 'the helper service is not running (install-driver.ps1 -Helper)'
    }
} catch {
    $tests += New-TestRecord 'C1' 'the studio client against the driver' 'fail' $_.Exception.Message
}

Write-Result -Path $ResultPath -Phase 'client' -Tests $tests
$tests | ForEach-Object { '{0,-6} {1,-4} {2}' -f $_.status, $_.id, $_.detail }
# C2 skipped for want of the helper service is not a failure: the kernel-mode driver has none.
if ($tests | Where-Object { $_.status -eq 'fail' }) { exit 1 }
exit 0
