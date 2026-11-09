# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Runs the signed path of the Windows installer without the release secrets.

.DESCRIPTION
  A pull request never signs, so the installer it builds holds unsigned driver files, and setup-driver.ps1 refuses
  them before it asks Windows anything. A signed run (SIGN_WINDOWS=test) takes a different path: the files carry a
  signature, so setup-driver.ps1 goes on to pnputil. That path once hung a release dry run for an hour while the
  pull request runs passed.

  This script copies the staged tree, signs the driver files in the copy with a throwaway certificate that nothing
  trusts (so they are signed, and by a publisher Windows does not know, exactly as the owner's test certificate
  looks to a hosted runner), builds a second installer from that copy and runs tools/ci/check-windows-installer.ps1
  on it: install, upgrade, uninstall. The result must be the same as for a real test-signed package: Studio
  installed, the driver refused as not trusted, nothing left after the uninstall, and no hang.

  The installer is not published and the certificate never leaves the runner.

.PARAMETER Stage
  The staged install tree, with the driver in bin\driver.

.PARAMETER CPackConfig
  build/<preset>/CPackConfig.cmake.

.PARAMETER SummaryFile
  A file to append a Markdown note to. In CI this is $env:GITHUB_STEP_SUMMARY.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Stage,
  [Parameter(Mandatory)][string]$CPackConfig,
  [string]$SummaryFile = ''
)

$ErrorActionPreference = 'Stop'

$temporary = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$work = Join-Path $temporary 'installer-rehearsal'
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
$tree = Join-Path $work 'stage'
Copy-Item -LiteralPath (Resolve-Path $Stage).Path -Destination $tree -Recurse

$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Studio installer rehearsal (trusted by nobody)' -CertStoreLocation Cert:\CurrentUser\My
try {
  foreach ($file in 'bin\driver\driver\vsscom_um.dll', 'bin\driver\driver\vsscom_um.cat', 'bin\driver\helper\vsscom_helper.exe') {
    $path = Join-Path $tree $file
    $result = Set-AuthenticodeSignature -LiteralPath $path -Certificate $certificate -HashAlgorithm SHA256
    if (-not $result.SignerCertificate) { throw "Could not sign $file ($($result.StatusMessage))." }
    Write-Host "signed $file ($($result.Status))"
  }
}
finally {
  Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -ErrorAction SilentlyContinue
}

$out = Join-Path $work 'dist'
& cpack --config (Resolve-Path $CPackConfig).Path -G NSIS -B $out `
  -D "CPACK_INSTALLED_DIRECTORIES=$((Resolve-Path $tree).Path -replace '\\', '/');/" `
  -D 'CPACK_INSTALL_CMAKE_PROJECTS=' `
  -D 'CPACK_PACKAGE_FILE_NAME=rehearsal'
if ($LASTEXITCODE -ne 0) { throw "cpack failed with $LASTEXITCODE." }
$installer = Join-Path $out 'rehearsal.exe'
if (-not (Test-Path -LiteralPath $installer)) { throw 'cpack did not make the rehearsal installer.' }

& (Join-Path $PSScriptRoot 'check-windows-installer.ps1') -Phase Install -Installer $installer -SummaryFile $SummaryFile
& (Join-Path $PSScriptRoot 'check-windows-installer.ps1') -Phase Uninstall

if ($SummaryFile) {
  Add-Content -LiteralPath $SummaryFile -Encoding utf8 -Value @('', '### Windows installer, signed path', '',
    'The driver files were signed with a throwaway certificate that nothing trusts and the installer was installed, upgraded and uninstalled on this runner: the same path a test-signed release takes, which a pull request run otherwise never takes.')
}
