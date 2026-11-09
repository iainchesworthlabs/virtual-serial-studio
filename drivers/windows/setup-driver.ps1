# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs or repairs everything Virtual Serial Studio needs on Windows to make
# virtual COM ports for an ordinary user: the user-mode driver package and the
# helper service. Studio starts it from "Set up Virtual Serial Studio" through
# one Windows permission prompt; it can also be run by hand from a terminal
# opened as administrator. Running it again repairs a partial install.
#
#   setup-driver.ps1 [-PackageRoot <folder>]
#
# The package folder holds:
#   driver\vsscom_um.inf, vsscom_um.dll, vsscom_um.cat   the user-mode driver
#   helper\vsscom_helper.exe, install-helper.ps1         the helper service
#
# Nothing is downloaded. Every file is checked first: each must carry a
# signature, and the machine must trust it (the signer is a trusted publisher
# and chains to a trusted root; see trust\install-trust.ps1) or be in test
# signing mode. If any check fails nothing is changed. The script never adds a
# certificate to the machine.
#
# Exit codes (the studio reads them):
#   0   done: the driver is installed and the helper service is running
#   10  done, and Windows asks for a restart before the driver is used
#   2   not run as an administrator
#   6   a file is unsigned or not trusted on this machine; nothing was changed
#   7   the driver package could not be installed
#   8   the helper service could not be installed or did not start
#   9   the package folder is incomplete
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

# Runs a program with a time limit and returns its exit code, or 124 when it ran out
# of time (it is then stopped, with anything it started). Its output goes to the host,
# so it reaches the installer's log. A program that waits for something nobody can
# answer, such as a prompt on a desktop that is not there, then ends the script with
# a reason and not an hour of silence.
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

# The whole script has a limit too: whatever else it waits for, it ends with exit code
# 124 after fifteen minutes. A timer thread does it, because the script itself may be
# blocked inside a call that never returns.
Add-Type -TypeDefinition 'public static class VssWatchdog { static System.Threading.Timer timer_; public static void Start(int milliseconds) { timer_ = new System.Threading.Timer(delegate { System.Console.Error.WriteLine("setup-driver.ps1 did not finish in time and was stopped"); System.Environment.Exit(124); }, null, milliseconds, -1); } }'
[VssWatchdog]::Start(900000)

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-With 2 'run this as an administrator'
}

try {
    $inf = Join-Path $PackageRoot 'driver\vsscom_um.inf'
    $dll = Join-Path $PackageRoot 'driver\vsscom_um.dll'
    $cat = Join-Path $PackageRoot 'driver\vsscom_um.cat'
    $exe = Join-Path $PackageRoot 'helper\vsscom_helper.exe'
    $installHelper = Join-Path $PackageRoot 'helper\install-helper.ps1'
    foreach ($file in $inf, $dll, $cat, $exe, $installHelper) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Stop-With 9 "missing: $file" }
    }

    # With test signing on, a test certificate is what the machine was set up to
    # accept. Without it the signer must be trusted as it is.
    $testSigning = $false
    $boot = & bcdedit.exe /enum '{current}' 2>$null
    if ($LASTEXITCODE -eq 0 -and ($boot | Select-String -Pattern '^\s*testsigning\s+Yes' -Quiet)) {
        $testSigning = $true
    }
    foreach ($file in $dll, $cat, $exe) {
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        $status = $signature.Status.ToString()
        $accepted = ($status -eq 'Valid') -or
            ($testSigning -and $signature.SignerCertificate -and ($status -in 'UnknownError', 'NotTrusted'))
        if (-not $accepted) {
            Stop-With 6 "$file is not signed by a publisher this machine trusts ($status)"
        }
    }

    # The errors Windows gives for a signature that is missing or whose publisher it does not trust:
    # TRUST_E_SUBJECT_NOT_TRUSTED, TRUST_E_NOSIGNATURE, CERT_E_UNTRUSTEDROOT, CERT_E_CHAINING,
    # CERT_E_UNTRUSTEDCA.
    $untrusted = '0x800B0004', '0x800B0100', '0x800B0109', '0x800B010A', '0x800B0112'

    $restart = $false
    Write-Host "adding the driver package $inf"
    $code = Invoke-Bounded 'pnputil.exe' @('/add-driver', $inf, '/install') 300
    if ($code -eq 3010) { $restart = $true }
    elseif ($code -eq 124) { Stop-With 7 'pnputil did not finish in 300 seconds and was stopped' }
    elseif (('0x{0:X8}' -f ($code -band 0xFFFFFFFFL)) -in $untrusted) {
        # With test signing on, a signature that is present but not trusted passes the check above, and then
        # Windows itself refuses the package. That is the same answer as an untrusted file: nothing was changed.
        Stop-With 6 ('{0} is not signed by a publisher this machine trusts (pnputil refused it with {1})' -f $inf, ('0x{0:X8}' -f ($code -band 0xFFFFFFFFL)))
    }
    elseif ($code -ne 0) { Stop-With 7 "pnputil failed with $code" }

    Write-Host 'installing the helper service'
    & $installHelper -ExePath $exe
    if ($LASTEXITCODE -ne 0) { Stop-With 8 "install-helper.ps1 failed with $LASTEXITCODE" }

    if ($restart) { Stop-With 10 'installed; restart Windows before using the driver' }
    Stop-With 0 'installed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
