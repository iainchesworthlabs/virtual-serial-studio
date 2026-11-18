# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs or repairs everything Virtual Serial Studio needs on Windows to make
# virtual COM ports for an ordinary user: the user-mode driver package and the
# helper service. Studio starts it from "Set up Virtual Serial Studio" through
# one Windows permission prompt; it can also be run by hand from a terminal
# opened as administrator. Running it again repairs a partial install.
#
#   setup-driver.ps1 [-PackageRoot <folder>] [-ReinstallDriver]
#   setup-driver.ps1 -Query -BundledVer <date,version> -QueryOut <file>
#
# The driver package has a version (DriverVer in the .inf: a date, then four numbers; Windows
# compares the date first). When the driver store already holds a vsscom_um package that is the
# same as the one in the package folder or newer, the driver is left as it is and only the helper
# service is installed or repaired; -ReinstallDriver installs the package again. An older one, or
# none, is replaced by the package. -Query changes nothing, needs no administrator and writes three
# lines to -QueryOut: none, older, same, newer or unknown; the installed version; the bundled one.
#
# The package folder holds:
#   driver\vsscom_um.inf, vsscom_um.dll, vsscom_um.cat   the user-mode driver
#   helper\vsscom_helper.exe, install-helper.ps1         the helper service
#
# Nothing is downloaded. Every file is checked first: each must carry a
# signature, and the machine must trust it (the signer chains to a trusted root;
# see trust\install-trust.ps1) or be in test signing mode. On top of that the
# catalog must be signed by Microsoft or by a publisher in Trusted Publishers,
# whatever its signature status and whether test signing is on, because Windows
# asks "Would you like to install this device software?" for any other signer and
# a silent run cannot answer. That is decided before pnputil is started. If any
# check fails nothing is changed. The script never adds a certificate to the machine.
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
    [string] $PackageRoot,
    [switch] $ReinstallDriver,
    [switch] $Query,
    [string] $BundledVer,
    [string] $QueryOut
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

# True when the driver store holds a package whose original name is vsscom_um.inf and whose version is the
# one the .inf file in the package folder declares (DriverVer=<date>,<version>).
function Test-PackageStored([string] $InfPath) {
    $declared = Select-String -LiteralPath $InfPath -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1
    $wanted = if ($declared) { $declared.Matches[0].Groups[1].Value } else { $null }
    foreach ($package in @(Get-WindowsDriver -Online)) {
        if ((Split-Path -Leaf $package.OriginalFileName) -ne 'vsscom_um.inf') { continue }
        if (-not $wanted -or $package.Version.ToString() -eq $wanted) { return $true }
    }
    $false
}

# The packages of the user-mode driver in the driver store, each as its date and its four-part version.
# They are read from the registry (the same place Studio reads), which every user may read and which is
# quicker than Get-WindowsDriver. The "Version" value is 8 bytes of its own, the class GUID, the date as a
# FILETIME (offset 24) and the version as one 64-bit number with the first part in the highest 16 bits.
function Get-DriverStorePackages {
    $root = 'HKLM:\SYSTEM\DriverDatabase\DriverPackages'
    if (-not (Test-Path -LiteralPath $root)) { return }
    foreach ($key in @(Get-ChildItem -LiteralPath $root | Where-Object { $_.PSChildName -like 'vsscom_um.inf_*' })) {
        $bytes = [byte[]] (Get-ItemProperty -LiteralPath $key.PSPath -Name Version).Version
        if ($bytes.Length -lt 40) { continue }
        $packed = [BitConverter]::ToUInt64($bytes, 32)
        $parts = foreach ($shift in 48, 32, 16, 0) { [int](($packed -shr $shift) -band 0xFFFF) }
        [pscustomobject]@{
            Date    = [DateTime]::FromFileTimeUtc([BitConverter]::ToInt64($bytes, 24)).Date
            Version = [version]::new($parts[0], $parts[1], $parts[2], $parts[3])
        }
    }
}

# '11/18/2026,1.5.1.0' as a date and a version, or $null.
function ConvertFrom-DriverVer([string] $Text) {
    if ($Text -notmatch '^\s*(\d{1,2})/(\d{1,2})/(\d{4})\s*,\s*(\d+)\.(\d+)\.(\d+)\.(\d+)\s*$') { return $null }
    try {
        [pscustomobject]@{
            Date    = [DateTime]::new([int]$Matches[3], [int]$Matches[1], [int]$Matches[2])
            Version = [version]::new([int]$Matches[4], [int]$Matches[5], [int]$Matches[6], [int]$Matches[7])
        }
    }
    catch { $null }
}

# -1, 0 or 1: how $A compares with $B the way Windows does it, the date first.
function Compare-DriverVer($A, $B) {
    if ($A.Date -ne $B.Date) { return [Math]::Sign(($A.Date - $B.Date).Ticks) }
    $A.Version.CompareTo($B.Version)
}

function Format-DriverVer($Package) { '{0} ({1})' -f $Package.Version, $Package.Date.ToString('yyyy-MM-dd') }

# How the newest installed package compares with the bundled one: State is none, older, same, newer or
# unknown (the bundled version is not readable, or the store is not), Installed and Bundled are text.
function Get-DriverState($Bundled) {
    $result = [pscustomobject]@{ State = 'unknown'; Installed = ''; Bundled = '' }
    if (-not $Bundled) { return $result }
    $result.Bundled = Format-DriverVer $Bundled
    try { $packages = @(Get-DriverStorePackages) }
    catch { return $result }
    if ($packages.Count -eq 0) { $result.State = 'none'; return $result }
    $newest = $packages[0]
    foreach ($package in $packages) { if ((Compare-DriverVer $package $newest) -gt 0) { $newest = $package } }
    $result.Installed = Format-DriverVer $newest
    $order = Compare-DriverVer $newest $Bundled
    $result.State = if ($order -lt 0) { 'older' } elseif ($order -gt 0) { 'newer' } else { 'same' }
    $result
}

if ($Query) {
    $state = Get-DriverState (ConvertFrom-DriverVer $BundledVer)
    if ($QueryOut) { Set-Content -LiteralPath $QueryOut -Value @($state.State, $state.Installed, $state.Bundled) -Encoding ASCII }
    Write-Host ('driver {0}; installed {1}; bundled {2}' -f $state.State, $state.Installed, $state.Bundled)
    exit 0
}

# The thumbprint of the root at the top of the chain of a certificate, or $null. The chain is built from
# the machine's stores, with no revocation check (a revocation server is not needed to see where it ends).
function Get-ChainTop([Security.Cryptography.X509Certificates.X509Certificate2] $Certificate) {
    $chain = New-Object Security.Cryptography.X509Certificates.X509Chain
    $chain.ChainPolicy.RevocationMode = 'NoCheck'
    [void] $chain.Build($Certificate)
    if ($chain.ChainElements.Count -eq 0) { return $null }
    $chain.ChainElements[$chain.ChainElements.Count - 1].Certificate.Thumbprint
}

# The roots that Microsoft's own hardware signatures (WHQL, attestation) end in, read from the driver store
# of a Windows 11 machine. A package signed by one of these installs without Windows asking a question.
$microsoftRoots = @(
    '3B1EFD3A66EA28B16697394703A72CA340A05BD5'  # Microsoft Root Certificate Authority 2010
    'CDD4EEAE6000AC7F40C3802C171E30148030C072'  # Microsoft Root Certificate Authority
    'A43489159A520F0D93D032CCAF37E7FE20A8B419'  # Microsoft Root Authority
)

# The whole script has a limit too: whatever else it waits for, it ends with exit code
# 124 after fifteen minutes. A timer thread does it, because the script itself may be
# blocked inside a call that never returns.
Add-Type -TypeDefinition 'public static class VssWatchdog { static System.Threading.Timer timer_; public static void Start(int milliseconds) { timer_ = new System.Threading.Timer(delegate { System.Console.Error.WriteLine("setup-driver.ps1 did not finish in time and was stopped"); System.Environment.Exit(124); }, null, milliseconds, -1); } }'
[VssWatchdog]::Start(900000)

if (-not (Test-IsAdministrator)) {
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

    # A signature that Windows accepts is not enough for a user-mode driver. Windows installs a package
    # without a question only from Microsoft or from a publisher it has been told to trust (the signer in
    # Trusted Publishers); for any other signer pnputil asks "Would you like to install this device
    # software?" on the desktop, and in a session with nobody to answer, such as a silent install on a
    # build agent, it waits for ever. That holds when test signing is on, and also when the signature is
    # Valid and the chain is trusted. So the answer is decided here, before pnputil is started, and does
    # not depend on the status: the catalog is signed by Microsoft's hardware publisher, or its signer is
    # in Trusted Publishers (trust\install-trust.ps1 puts it there), or nothing is changed.
    $catalogSignature = Get-AuthenticodeSignature -LiteralPath $cat
    $signer = $catalogSignature.SignerCertificate
    # Microsoft's hardware publisher: the name alone is not enough (any certificate can carry it), so the
    # chain must also end in one of the Microsoft roots above.
    $microsoftSigned = $catalogSignature.Status.ToString() -eq 'Valid' -and $signer -and
        ($signer.Subject -match '(^|,\s*)CN=Microsoft Windows Hardware Compatibility Publisher(,|$)') -and
        ((Get-ChainTop $signer) -in $microsoftRoots)
    if (-not $microsoftSigned) {
        if (-not $signer -or -not (Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPublisher\$($signer.Thumbprint)")) {
            Stop-With 6 "$cat is not signed by a publisher this machine trusts (its signer is not in Trusted Publishers, so Windows would ask whether to install it)"
        }
    }

    # The errors Windows gives for a signature that is missing or whose publisher it does not trust:
    # TRUST_E_SUBJECT_NOT_TRUSTED, TRUST_E_NOSIGNATURE, CERT_E_UNTRUSTEDROOT, CERT_E_CHAINING,
    # CERT_E_UNTRUSTEDCA.
    $untrusted = '0x800B0004', '0x800B0100', '0x800B0109', '0x800B010A', '0x800B0112'

    $restart = $false
    $declared = Select-String -LiteralPath $inf -Pattern '^\s*DriverVer\s*=\s*(.+?)\s*$' | Select-Object -First 1
    $state = Get-DriverState $(if ($declared) { ConvertFrom-DriverVer $declared.Matches[0].Groups[1].Value })
    $installDriver = $true
    if (($state.State -eq 'same' -or $state.State -eq 'newer') -and -not $ReinstallDriver) {
        $installDriver = $false
        Write-Host ("the driver package in the driver store is version {0}; this setup carries {1}; the driver was left as it is" -f $state.Installed, $state.Bundled)
    }
    elseif ($state.State -eq 'older') {
        Write-Host ("replacing the driver package version {0} with {1}" -f $state.Installed, $state.Bundled)
    }
    if ($installDriver) {
        Write-Host "adding the driver package $inf"
        $code = Invoke-Bounded 'pnputil.exe' @('/add-driver', $inf, '/install') 180
        if ($code -eq 3010) { $restart = $true }
        elseif ($code -eq 259) {
            # ERROR_NO_MORE_ITEMS, which pnputil documents (PnPUtil Return Values) as "no devices match the
            # supplied driver or the target device is already using a better or newer driver". It is the normal
            # answer when set-up is run again while ports exist. The package is in the driver store, or this
            # is not an answer to accept, so the store decides.
            try { $stored = Test-PackageStored $inf }
            catch { Stop-With 7 "pnputil answered 259 (no device was updated) and the driver store could not be read: $($_.Exception.Message)" }
            if (-not $stored) { Stop-With 7 'pnputil answered 259 (no device was updated) and this version of the vsscom_um driver package is not in the driver store' }
            Write-Host 'the devices already use this driver or a newer one; the driver package is in the driver store'
        }
        elseif ($code -eq 124) { Stop-With 7 'pnputil did not finish in 180 seconds and was stopped' }
        elseif (('0x{0:X8}' -f ($code -band 0xFFFFFFFFL)) -in $untrusted) {
            # With test signing on, a signature that is present but not trusted passes the check above, and then
            # Windows itself refuses the package. That is the same answer as an untrusted file: nothing was changed.
            Stop-With 6 ('{0} is not signed by a publisher this machine trusts (pnputil refused it with {1})' -f $inf, ('0x{0:X8}' -f ($code -band 0xFFFFFFFFL)))
        }
        elseif ($code -ne 0) { Stop-With 7 "pnputil failed with $code" }

        # pnputil can answer 0 for a package that is already in the store, and Windows keeps the one it has when the new
        # DriverVer is not newer than it (DriverVer is compared by date, then by the four-part version). Then the old
        # driver stays and every port it makes is refused. The store must hold the version this package declares, or
        # the setup says so and stops before the helper is touched.
        if ($code -ne 259) {
            try { $stored = Test-PackageStored $inf }
            catch { Stop-With 7 "pnputil answered $code and the driver store could not be read: $($_.Exception.Message)" }
            if (-not $stored) { Stop-With 7 "pnputil answered $code, but the driver store does not hold the version of the vsscom_um package that this setup carries; Windows kept an older one" }
        }
    }

    Write-Host 'installing the helper service'
    # install-helper.ps1 ends with an exit code, but a throw out of it (a missing command, a parameter
    # that does not bind) must give the same answer, so both end in 8.
    $global:LASTEXITCODE = 0
    $helperFailure = $null
    try { & $installHelper -ExePath $exe }
    catch { $helperFailure = $_.Exception.Message }
    if ($helperFailure) { Stop-With 8 "install-helper.ps1 failed: $helperFailure" }
    if ($LASTEXITCODE -ne 0) { Stop-With 8 "install-helper.ps1 failed with $LASTEXITCODE" }

    if ($restart) { Stop-With 10 'installed; restart Windows before using the driver' }
    Stop-With 0 'installed'
}
catch {
    Stop-With 1 $_.Exception.Message
}
