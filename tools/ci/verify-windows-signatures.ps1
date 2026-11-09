# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Checks that the Windows programs a leg produced carry the signature the signing mode promises.

.DESCRIPTION
  Used by .github/actions/sign-windows-binaries in CI, and by hand after a local signing test
  (docs/windows-com-driver.md, "Signing"). Exit status 1 means a file failed.

  Mode false     Nothing is signed on purpose. Prints a notice and checks nothing.
  Mode test      Every file must carry a signature that is intact and an RFC 3161 timestamp. The
                 certificate comes from the owner's personal CA, so a machine without that CA
                 reports NotTrusted or UnknownError, which is accepted. A file that is unsigned,
                 changed after signing or lacks a timestamp fails.
  Mode production
                 As test, and the chain must be trusted: Status Valid, and `signtool verify /pa`
                 passes.

  Every .exe is also checked for the execution level in its manifest: a program that asks for
  requireAdministrator or highestAvailable would show the permission prompt at every start. Only
  a helper that a step starts on purpose may elevate, through ShellExecute and the runas verb,
  never through its manifest. The one exception is an installer (-AllowElevation): it writes to
  Program Files, so it asks for administrator rights once, when it starts, and its manifest says so.

.PARAMETER Mode
  false, test or production, as SIGN_WINDOWS says.

.PARAMETER Files
  Paths of the files to check.

.PARAMETER AllowElevation
  The files are installers, whose manifest may ask for requireAdministrator or highestAvailable.
  Everything else is checked as before.

.PARAMETER SummaryFile
  A file to append a Markdown table to. In CI this is $env:GITHUB_STEP_SUMMARY.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('false', 'test', 'production')][string]$Mode,
  [Parameter(Mandatory)][string[]]$Files,
  [string]$SummaryFile = '',
  [switch]$AllowElevation
)

$ErrorActionPreference = 'Stop'

function Find-Signtool {
  $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
  if ($found) { return $found.FullName }
  $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
  if ($onPath) { return $onPath.Source }
  return $null
}

# The manifest is stored as text in the resource section, so a byte search finds the level.
function Get-ExecutionLevel([string]$path) {
  $bytes = [IO.File]::ReadAllBytes($path)
  $text = [Text.Encoding]::ASCII.GetString($bytes)
  if ($text -match 'requestedExecutionLevel\s+level\s*=\s*"(\w+)"') { return $Matches[1] }
  return $null
}

$list = @($Files | ForEach-Object { $_ -split "`r?`n" } | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_.Trim() })
if ($list.Count -eq 0) { throw 'No files to check.' }

$rows = New-Object System.Collections.Generic.List[string]
$failed = 0

if ($Mode -eq 'false') {
  Write-Host '::notice title=Windows binaries not signed::SIGN_WINDOWS is false, or the signing secrets are not set, so these files are unsigned on purpose.'
  foreach ($f in $list) {
    $detail = ''
    if ($f -match '\.exe$' -and (Test-Path -LiteralPath $f)) {
      $level = Get-ExecutionLevel $f
      if (-not $AllowElevation -and $level -in @('requireAdministrator', 'highestAvailable')) {
        $failed++
        $detail = "manifest asks for $level"
        Write-Host "::error title=Windows manifest check failed::$f - $detail"
      }
    }
    $rows.Add("| ``$f`` | not signed (signing is off) | $detail |")
  }
}
else {
  $signtool = Find-Signtool
  foreach ($f in $list) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $f)) {
      $problems.Add('file not found')
      $rows.Add("| ``$f`` | missing | $($problems -join '; ') |")
      $failed++
      continue
    }
    $sig = Get-AuthenticodeSignature -LiteralPath $f
    $status = [string]$sig.Status
    if ($null -eq $sig.SignerCertificate) { $problems.Add('not signed') }
    else {
      if ($status -in @('HashMismatch', 'NotSigned', 'Incompatible')) { $problems.Add("signature is $status") }
      if ($null -eq $sig.TimeStamperCertificate) { $problems.Add('no timestamp') }
      if ($Mode -eq 'production') {
        if ($status -ne 'Valid') { $problems.Add("status $status, not Valid") }
        elseif ($signtool) {
          & $signtool verify /pa $f | Out-Null
          if ($LASTEXITCODE -ne 0) { $problems.Add('signtool verify /pa failed') }
        }
        else { $problems.Add('signtool.exe not found') }
      }
    }
    if ($f -match '\.exe$') {
      $level = Get-ExecutionLevel $f
      if (-not $AllowElevation -and $level -in @('requireAdministrator', 'highestAvailable')) { $problems.Add("manifest asks for $level") }
    }
    $subject = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
    if ($problems.Count -gt 0) {
      $failed++
      Write-Host "::error title=Windows signature check failed::$f - $($problems -join '; ')"
      $rows.Add("| ``$f`` | FAILED | $($problems -join '; ') |")
    }
    else {
      Write-Host "OK $f ($status; $subject)"
      $rows.Add("| ``$f`` | signed, $status | $subject |")
    }
  }
}

if ($SummaryFile) {
  $header = @('', "### Windows signatures (mode: $Mode)", '', '| File | Result | Detail |', '|---|---|---|')
  Add-Content -LiteralPath $SummaryFile -Value ($header + $rows) -Encoding utf8
}

if ($failed -gt 0) { throw "$failed file(s) failed the signature check." }
