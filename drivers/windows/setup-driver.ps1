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
    [string] $PackageRoot = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

function Stop-With([int] $Code, [string] $Message) {
    if ($Code -eq 0 -or $Code -eq 10) { Write-Host $Message } else { [Console]::Error.WriteLine($Message) }
    exit $Code
}

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

    $restart = $false
    & pnputil.exe /add-driver $inf /install | Out-Null
    if ($LASTEXITCODE -eq 3010) { $restart = $true }
    elseif ($LASTEXITCODE -ne 0) { Stop-With 7 "pnputil failed with $LASTEXITCODE" }

    & $installHelper -ExePath $exe
    if ($LASTEXITCODE -ne 0) { Stop-With 8 "install-helper.ps1 failed with $LASTEXITCODE" }

    if ($restart) { Stop-With 10 'installed; restart Windows before using the driver' }
    Stop-With 0 'installed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
