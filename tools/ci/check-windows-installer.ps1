# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Installs, upgrades or uninstalls the Windows installer silently and checks what the machine looks like.

.DESCRIPTION
  The release workflow runs it on the hosted runner (docs/releasing.md, "The Windows installer"), and it runs by
  hand in a guest or on any machine you can install on, as an administrator. Every program it starts has a time
  limit: one that runs out is stopped, the process tree and the logs are printed, and the script fails, so a
  silent installer that waits for something nobody can answer ends in a reason and not in an hour of silence.

  Install    First install with /DRIVER=0 (Studio only), then an upgrade over it with the driver, which is the
             default. The driver state after the upgrade, which the installer records in the Apps & features
             key, must be one of:
               installed, installed-restart-needed   the helper service runs and the package is in the driver store
               not-trusted                           no service, no package, and the log says why
             Anything else fails. On a machine that trusts neither an unsigned nor a test-signed driver, which is
             a hosted runner, not-trusted is the expected result.
  Uninstall  Silent uninstall, then no install folder, Apps & features entry, Start menu folder, helper service or
             driver package may remain.

  The installer logs (driver-setup.log, driver-remove.log) are printed on success and on failure.

.PARAMETER Phase
  Install or Uninstall.

.PARAMETER Installer
  The installer .exe (Install).

.PARAMETER SummaryFile
  A file to append a Markdown note to. In CI this is $env:GITHUB_STEP_SUMMARY.

.PARAMETER Seconds
  The limit for one run of the installer or the uninstaller. Default 600.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('Install', 'Uninstall')][string]$Phase,
  [string]$Installer,
  [string]$SummaryFile = '',
  [int]$Seconds = 600
)

$ErrorActionPreference = 'Stop'

$folder = Join-Path $(if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }) 'Virtual Serial Studio'
# The installer is a 32-bit program, so its registry entries are in the 32-bit view.
$key = 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\VirtualSerialStudio'
$startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Virtual Serial Studio'

function Get-DriverPackages {
  @(Get-WindowsDriver -Online -All | Where-Object { (Split-Path -Leaf $_.OriginalFileName) -eq 'vsscom_um.inf' })
}

function Show-State([string] $title) {
  Write-Host "::group::$title"
  Write-Host ('test signing: ' + ((& bcdedit.exe /enum '{current}' 2>&1 | Select-String -Pattern 'testsigning') -join ' '))
  $service = Get-Service -Name vsscom_helper -ErrorAction SilentlyContinue
  Write-Host ('helper service: ' + $(if ($service) { $service.Status } else { 'none' }))
  Write-Host ('vsscom_um packages in the driver store: ' + (Get-DriverPackages).Count)
  if (Test-Path -LiteralPath $key) { Get-ItemProperty -LiteralPath $key | Select-Object DisplayName, DisplayVersion, VssDriver | Format-List | Out-String | Write-Host }
  foreach ($name in 'driver-setup.log', 'driver-remove.log') {
    $log = Join-Path $folder $name
    if (Test-Path -LiteralPath $log) {
      Write-Host "--- $name"
      Get-Content -LiteralPath $log | ForEach-Object { Write-Host "  $_" }
    }
  }
  Write-Host '::endgroup::'
}

# Starts a program and waits for it for a limited time. When it runs out the process tree is printed and stopped.
function Invoke-Bounded([string] $File, [string[]] $Arguments, [string] $What) {
  Write-Host "$What ..."
  $started = Get-Date
  $process = if ($Arguments) { Start-Process -FilePath $File -ArgumentList $Arguments -PassThru } else { Start-Process -FilePath $File -PassThru }
  if (-not $process.WaitForExit($Seconds * 1000)) {
    Write-Host "::error title=$What did not finish::It was still running after $Seconds seconds and was stopped."
    Write-Host '::group::Processes'
    Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId, Name, CommandLine |
      Format-Table -AutoSize -Wrap | Out-String -Width 250 | Write-Host
    Write-Host '::endgroup::'
    Show-State "State after $What timed out"
    & taskkill.exe /T /F /PID $process.Id | Out-Null
    throw "$What did not finish in $Seconds seconds."
  }
  $process.WaitForExit()
  Write-Host ("$What finished in {0:N0} s with exit code {1}" -f ((Get-Date) - $started).TotalSeconds, $process.ExitCode)
  return $process.ExitCode
}

function Assert-Studio([string] $when) {
  foreach ($file in 'bin\vss_gui.exe', 'bin\vss_cli.exe', 'bin\driver\setup-driver.ps1', 'bin\driver\driver\vsscom_um.cat', 'bin\driver\helper\vsscom_helper.exe', 'Uninstall.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $folder $file))) { throw "$file is missing after the $when." }
  }
  if (-not (Test-Path -LiteralPath $key)) { throw "No Apps & features entry after the $when." }
}

try {
  if ($Phase -eq 'Install') {
    if (-not $Installer) { throw '-Installer is required to install.' }
    $path = (Resolve-Path $Installer).Path
    Show-State 'State before the install'

    # The machine PATH before anything: the installer must add one folder to it, however long it is
    # (a hosted runner's is far longer than the 1024 characters CPack's own PATH code stops at), and
    # the uninstall must give the same PATH back.
    $pathBefore = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    Write-Host "The machine PATH is $($pathBefore.Length) characters before the install."
    if ($env:RUNNER_TEMP) { Set-Content -LiteralPath (Join-Path $env:RUNNER_TEMP 'vss-path-before.txt') -Value $pathBefore -NoNewline -Encoding utf8 }

    $code = Invoke-Bounded $path @('/S', '/DRIVER=0', '/ADDTOPATH=1') 'First install, without the driver, on the PATH'
    if ($code -ne 0) { throw "The installer exited with $code (first install, /DRIVER=0)." }
    Assert-Studio 'first install'
    if ((Get-ItemProperty -LiteralPath $key).VssDriver -ne 'skipped') { throw 'The driver was not recorded as skipped after /DRIVER=0.' }
    if (Get-Service -Name vsscom_helper -ErrorAction SilentlyContinue) { throw '/DRIVER=0 installed the helper service.' }
    if ((Get-DriverPackages).Count -ne 0) { throw '/DRIVER=0 installed the driver.' }

    $code = Invoke-Bounded $path @('/S') 'Upgrade, with the driver'
    Show-State 'State after the upgrade'
    if ($code -ne 0) { throw "The installer exited with $code (upgrade)." }
    Assert-Studio 'upgrade'
    $state = (Get-ItemProperty -LiteralPath $key).VssDriver
    $log = Join-Path $folder 'driver-setup.log'
    $service = Get-Service -Name vsscom_helper -ErrorAction SilentlyContinue
    switch ($state) {
      { $_ -in 'installed', 'installed-restart-needed' } {
        if (-not $service -or $service.Status -ne 'Running') { throw 'The driver is installed but the helper service is not running.' }
        if ((Get-DriverPackages).Count -ne 1) { throw 'The driver package is not in the driver store.' }
      }
      'not-trusted' {
        if ($service) { throw 'The driver was refused but a helper service exists.' }
        if ((Get-DriverPackages).Count -ne 0) { throw 'The driver was refused but a package is in the driver store.' }
        if ((Get-Content -LiteralPath $log -Raw) -notmatch 'not signed by a publisher this machine trusts') { throw 'driver-setup.log does not say why the driver was refused.' }
      }
      default { throw "Unexpected driver state '$state' after the upgrade." }
    }
    if ($SummaryFile) {
      Add-Content -LiteralPath $SummaryFile -Encoding utf8 -Value @('', '### Windows installer', '',
        "Silent install with the driver, on this runner: **$state**. A runner trusts neither an unsigned nor a test-signed driver, so ``not-trusted`` (Studio installed, driver and service not) is the expected result here; the guest tests in docs/releasing.md cover the installed driver.")
    }

    if (-not (Test-Path -LiteralPath (Join-Path $startMenu 'Virtual Serial Studio.lnk'))) { throw 'The Start menu shortcut is missing.' }
    # /ADDTOPATH=1 put the folder on the machine PATH once, left the rest as it was, and the upgrade
    # (a plain /S) did not take it away or add it again.
    $bin = Join-Path $folder 'bin'
    # Empty entries (a trailing ;) do not count: the installer drops them.
    $pathNow = @([Environment]::GetEnvironmentVariable('Path', 'Machine').Split(';') | Where-Object { $_ })
    $ours = @($pathNow | Where-Object { $_.TrimEnd([char]92) -ieq $bin })
    if ($ours.Count -ne 1) { throw "The folder $bin is on the machine PATH $($ours.Count) times after /ADDTOPATH=1 and an upgrade; it should be once." }
    $expected = (@($pathBefore.Split(';') | Where-Object { $_ }) + $bin) -join ';'
    if (($pathNow -join ';') -ne $expected) { throw 'The installer changed more of the machine PATH than its own folder.' }
  }
  else {
    Show-State 'State before the uninstall'
    # The uninstaller copies itself to a temporary folder and returns at once, so wait for the folder to go.
    $code = Invoke-Bounded (Join-Path $folder 'Uninstall.exe') @('/S') 'Uninstall'
    if ($code -ne 0) { throw "The uninstaller exited with $code." }
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Test-Path -LiteralPath $folder) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    if (Test-Path -LiteralPath $folder) {
      Get-ChildItem -LiteralPath $folder -Recurse | Select-Object -First 20 FullName | Out-String | Write-Host
      throw "$folder is still there after the uninstall."
    }
    if (Test-Path -LiteralPath $key) { throw 'The Apps & features entry is still there.' }
    if (Get-Service -Name vsscom_helper -ErrorAction SilentlyContinue) { throw 'The helper service is still there.' }
    if ((Get-DriverPackages).Count -ne 0) { throw 'The driver package is still in the driver store.' }
    if (Test-Path -LiteralPath $startMenu) { throw 'The Start menu folder is still there.' }
    $pathNow = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if ($pathNow -like '*Virtual Serial Studio*') { throw 'The folder is still on the machine PATH.' }
    $before = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP 'vss-path-before.txt' } else { '' }
    if ($before -and (Test-Path -LiteralPath $before)) {
      # The PATH is what it was, apart from empty entries.
      $was = (Get-Content -LiteralPath $before -Raw).Split(';') | Where-Object { $_ }
      if ((($pathNow.Split(';') | Where-Object { $_ }) -join ';') -ne ($was -join ';')) { throw 'The machine PATH is not what it was before the install.' }
    }
    Write-Host 'The uninstall left nothing behind.'
  }
}
catch {
  Show-State "State after the failure: $($_.Exception.Message)"
  throw
}
