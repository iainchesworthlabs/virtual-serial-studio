# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes a machine trust a driver package or helper program that was signed by the
# owner's own certificate authority (CA), so the package installs with test signing
# OFF. Run it as an administrator. It handles public certificates only: it never
# needs, asks for or touches a private key.
#
#   install-trust.ps1 -RootCertificate <root.pem> -IntermediateCertificate <intermediate.pem>
#                     [-Package <folder, .cat, .dll or .exe>] [-ExpectedRootSha256 <hex>]
#                     [-AllowTestSigning] [-WhatIf]
#   install-trust.ps1 -Uninstall -RootCertificate <root.pem> -IntermediateCertificate <intermediate.pem>
#                     [-Package <...> | -SignerThumbprint <hex>] [-IncludeUnmarked]
#
# What it changes on the machine (and what -Uninstall takes away again):
#   - the root certificate     -> Local Machine, Trusted Root Certification Authorities
#   - the intermediate         -> Local Machine, Intermediate Certification Authorities
#   - the signer's certificate -> Local Machine, Trusted Publishers
# The signer's certificate is read from the signature of -Package. It is added only
# after the signature checks out against the root you named, so a package signed by
# anyone else adds nothing. The Trusted Publishers entry is needed because pnputil
# cannot ask the question "do you trust this publisher?" and refuses without it.
#
# Everything is checked before the first certificate is added: the root, the
# intermediate, the chain of the signer, and that the signature on -Package has not been
# broken (a file altered after signing adds nothing). The certificates are then added,
# the signature is checked again now that the root is trusted, and if that or any later
# step fails what this run added is taken out again.
#
# The root is trusted for code signing only (its "enhanced key usage" property is set to
# that one purpose), so a certificate it issued is not accepted for a web site or for mail.
#
# Each certificate that this script adds carries the friendly name "Virtual Serial Studio
# (install-trust.ps1)". -Uninstall removes only certificates with that name, so a root
# that the machine trusted for another reason stays. A certificate added by an earlier
# version of this script has no name: -IncludeUnmarked removes it too.
#
# Trusting a root means trusting everything that CA ever signs for code signing, for
# every program on the machine. Do this only on machines you own, and check the printed
# fingerprint of the root against the one you hold. -ExpectedRootSha256 makes the
# script refuse a root whose fingerprint differs.
#
# -StoreLocation and the three store names are for the tests of this script, which
# use stores of their own so that no real store is touched.
#
# The driver is not installed here. Afterwards:  pnputil /add-driver vsscom_um.inf /install
#
# Exit codes: 0 done, 1 a check failed (nothing is left added), 2 not an administrator,
# 3 test signing is on.

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)] [string] $RootCertificate,
    [Parameter(Mandatory = $true)] [string] $IntermediateCertificate,
    [string] $Package,
    [string] $SignerThumbprint,
    [string] $ExpectedRootSha256,
    [switch] $AllowTestSigning,
    [switch] $Uninstall,
    [switch] $IncludeUnmarked,
    [ValidateSet('LocalMachine', 'CurrentUser')] [string] $StoreLocation = 'LocalMachine',
    [string] $RootStoreName = 'Root',
    [string] $IntermediateStoreName = 'CA',
    [string] $PublisherStoreName = 'TrustedPublisher'
)

$ErrorActionPreference = 'Stop'

# The friendly name of every certificate this script adds; -Uninstall removes only those.
$Marker = 'Virtual Serial Studio (install-trust.ps1)'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

// The "enhanced key usage" property of a certificate in a store (CERT_ENHKEY_USAGE_PROP_ID):
// the purposes the machine trusts the certificate for. X509Store has no way to set it.
public static class VssCertPurpose
{
    const uint CERT_ENHKEY_USAGE_PROP_ID = 9;

    [StructLayout(LayoutKind.Sequential)]
    struct CRYPT_DATA_BLOB { public int cbData; public IntPtr pbData; }

    [DllImport("crypt32.dll", SetLastError = true)]
    static extern bool CertSetCertificateContextProperty(IntPtr context, uint propertyId, uint flags, ref CRYPT_DATA_BLOB data);

    [DllImport("crypt32.dll", SetLastError = true)]
    static extern bool CertGetCertificateContextProperty(IntPtr context, uint propertyId, byte[] data, ref int size);

    // SEQUENCE { OBJECT IDENTIFIER 1.3.6.1.5.5.7.3.3 }: code signing.
    public static readonly byte[] CodeSigningOnly = { 0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x03 };

    public static void SetCodeSigningOnly(IntPtr context)
    {
        IntPtr memory = Marshal.AllocHGlobal(CodeSigningOnly.Length);
        try
        {
            Marshal.Copy(CodeSigningOnly, 0, memory, CodeSigningOnly.Length);
            CRYPT_DATA_BLOB blob;
            blob.cbData = CodeSigningOnly.Length;
            blob.pbData = memory;
            if (!CertSetCertificateContextProperty(context, CERT_ENHKEY_USAGE_PROP_ID, 0, ref blob))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        finally { Marshal.FreeHGlobal(memory); }
    }

    // The property as stored, or null when the certificate has none (trusted for every purpose).
    public static byte[] Get(IntPtr context)
    {
        int size = 0;
        if (!CertGetCertificateContextProperty(context, CERT_ENHKEY_USAGE_PROP_ID, null, ref size)) return null;
        byte[] data = new byte[size];
        if (!CertGetCertificateContextProperty(context, CERT_ENHKEY_USAGE_PROP_ID, data, ref size)) return null;
        return data;
    }
}
'@

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Read-Certificate([string] $Path) {
    # Accepts PEM (with or without text before the BEGIN line) or DER.
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    $match = [regex]::Match($text, '-----BEGIN CERTIFICATE-----(?<body>.*?)-----END CERTIFICATE-----',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if ($match.Success) {
        $bytes = [Convert]::FromBase64String(($match.Groups['body'].Value -replace '\s', ''))
    }
    New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList (, $bytes)
}

function Get-Sha256([Security.Cryptography.X509Certificates.X509Certificate2] $Certificate) {
    $hash = [Security.Cryptography.SHA256]::Create().ComputeHash($Certificate.RawData)
    ($hash | ForEach-Object { $_.ToString('X2') }) -join ''
}

function Test-IsCa([Security.Cryptography.X509Certificates.X509Certificate2] $Certificate) {
    $constraints = $Certificate.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] }
    $constraints -and $constraints.CertificateAuthority
}

function Open-Store([string] $StoreName, [bool] $Write) {
    $actual = switch ($StoreName) { 'Root' { $RootStoreName } 'CA' { $IntermediateStoreName } 'TrustedPublisher' { $PublisherStoreName } default { $StoreName } }
    $store = New-Object Security.Cryptography.X509Certificates.X509Store -ArgumentList $actual, $StoreLocation
    $mode = if ($Write) { 'ReadWrite' } else { 'ReadOnly' }
    $store.Open($mode)
    $store
}

function Find-InStore([string] $StoreName, [string] $Thumbprint) {
    $store = Open-Store $StoreName $false
    try { $store.Certificates | Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1 } finally { $store.Close() }
}

# What this run has added, so that a later failure takes exactly that out again.
$script:Added = New-Object Collections.Generic.List[object]

function Add-ToStore([string] $StoreName, [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate, [string] $What, [switch] $CodeSigningOnly) {
    $present = Find-InStore $StoreName $Certificate.Thumbprint
    if ($present) {
        # Left exactly as it is. For the root, say what it is trusted for, so the state can be seen.
        $note = ''
        if ($CodeSigningOnly) {
            $property = [VssCertPurpose]::Get($present.Handle)
            $limited = $property -and [Convert]::ToBase64String($property) -eq [Convert]::ToBase64String([VssCertPurpose]::CodeSigningOnly)
            $note = if ($limited) { ' (trusted for code signing only)' } else { ' (trusted for every purpose; not changed. -Uninstall -IncludeUnmarked, then this script again, limits it)' }
        }
        Write-Host "already in $StoreName : $What$note"
        return
    }
    if (-not $PSCmdlet.ShouldProcess("$StoreLocation\$StoreName", "add $What")) { return }
    $Certificate.FriendlyName = $Marker
    if ($CodeSigningOnly) { [VssCertPurpose]::SetCodeSigningOnly($Certificate.Handle) }
    $store = Open-Store $StoreName $true
    try { $store.Add($Certificate) } finally { $store.Close() }
    $script:Added.Add(@{ Store = $StoreName; Thumbprint = $Certificate.Thumbprint; What = $What })
    if ($CodeSigningOnly) {
        $stored = Find-InStore $StoreName $Certificate.Thumbprint
        $property = if ($stored) { [VssCertPurpose]::Get($stored.Handle) } else { $null }
        if (-not $property -or [Convert]::ToBase64String($property) -ne [Convert]::ToBase64String([VssCertPurpose]::CodeSigningOnly)) {
            throw "the $What was added, but Windows did not keep the limit to code signing"
        }
    }
    Write-Host "added to $StoreName : $What"
}

function Remove-ThumbprintFromStore([string] $StoreName, [string] $Thumbprint) {
    $store = Open-Store $StoreName $true
    try {
        $store.Certificates | Where-Object { $_.Thumbprint -eq $Thumbprint } | ForEach-Object { $store.Remove($_) }
    } finally { $store.Close() }
}

# Takes out what this run added, newest first. A failure here is reported and does not stop the rest.
function Undo-Added {
    for ($i = $script:Added.Count - 1; $i -ge 0; $i--) {
        $item = $script:Added[$i]
        try {
            Remove-ThumbprintFromStore $item.Store $item.Thumbprint
            Write-Host "taken out again, $($item.Store) : $($item.What)"
        } catch {
            [Console]::Error.WriteLine("could not take the $($item.What) out of $($item.Store): $($_.Exception.Message)")
        }
    }
    $script:Added.Clear()
}

function Remove-FromStore([string] $StoreName, [string] $Thumbprint, [string] $What) {
    $found = Find-InStore $StoreName $Thumbprint
    if (-not $found) { Write-Host "not in $StoreName : $What"; return }
    if ($found.FriendlyName -ne $Marker -and -not $IncludeUnmarked) {
        Write-Host "left in $StoreName : $What (it was not added by this script; -IncludeUnmarked removes it)"
        return
    }
    if (-not $PSCmdlet.ShouldProcess("$StoreLocation\$StoreName", "remove $What")) { return }
    Remove-ThumbprintFromStore $StoreName $Thumbprint
    Write-Host "removed from $StoreName : $What"
}

function Find-SignedFile([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "-Package not found: $Path" }
    $item = Get-Item -LiteralPath $Path
    if (-not $item.PSIsContainer) { return $item.FullName }
    # A folder: prefer the catalog (what pnputil checks), then the helper program.
    foreach ($pattern in '*.cat', 'vsscom_helper.exe', '*.dll') {
        $found = Get-ChildItem -LiteralPath $item.FullName -Filter $pattern -File | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw "no .cat, .exe or .dll to read a signature from in $Path"
}

try {
    # Write-Error throws here ($ErrorActionPreference is Stop), so the catch below would answer 1 and
    # the exit after it would never run. The messages for exit 2 and 3 go to the error stream directly.
    if (-not $WhatIfPreference -and $StoreLocation -eq 'LocalMachine') {
        if (-not (Test-IsAdministrator)) {
            [Console]::Error.WriteLine('run this as an administrator')
            exit 2
        }
    }

    # With test signing on, an install works with any signature, so it would prove nothing
    # about the trust set up here. Say so and stop, unless the person asked for it anyway.
    if (-not $Uninstall) {
        $boot = (& bcdedit.exe /enum '{current}' 2>$null) -join "`n"
        if ($boot -match '(?im)^\s*testsigning\s+Yes') {
            if (-not $AllowTestSigning) {
                [Console]::Error.WriteLine('test signing is ON, so a package installs here whether or not it is trusted. ' +
                    'Turn it off ("bcdedit /set testsigning off", then restart) or pass -AllowTestSigning.')
                exit 3
            }
            Write-Warning 'test signing is on; an install will not show whether the trust below is enough'
        }
    }

    $root = Read-Certificate $RootCertificate
    $intermediate = Read-Certificate $IntermediateCertificate
    $rootSha = Get-Sha256 $root

    if ($root.Subject -ne $root.Issuer) { throw 'the root certificate is not self-signed' }
    if (-not (Test-IsCa $root)) { throw 'the root certificate is not marked as a CA' }
    if (-not (Test-IsCa $intermediate)) { throw 'the intermediate certificate is not marked as a CA' }
    if ($intermediate.Issuer -ne $root.Subject) { throw 'the intermediate was not issued by this root' }
    if ($ExpectedRootSha256 -and (($ExpectedRootSha256 -replace '[^0-9A-Fa-f]', '').ToUpperInvariant() -ne $rootSha)) {
        throw "the root's SHA-256 fingerprint is $rootSha, not the one expected"
    }
    $now = Get-Date
    foreach ($c in $root, $intermediate) {
        if ($now -lt $c.NotBefore -or $now -gt $c.NotAfter) { throw "certificate $($c.Subject) is outside its validity dates" }
    }
    Write-Host "root         : $($root.Subject)"
    Write-Host "  SHA-256    : $rootSha"
    Write-Host "  valid until: $($root.NotAfter.ToString('yyyy-MM-dd'))"
    Write-Host "intermediate : $($intermediate.Subject)"

    if ($Uninstall) {
        Remove-FromStore 'Root' $root.Thumbprint 'root certificate'
        Remove-FromStore 'CA' $intermediate.Thumbprint 'intermediate certificate'
        $thumb = $SignerThumbprint
        if (-not $thumb -and $Package) {
            $sig = Get-AuthenticodeSignature -LiteralPath (Find-SignedFile $Package)
            if ($sig.SignerCertificate) { $thumb = $sig.SignerCertificate.Thumbprint }
        }
        if ($thumb) { Remove-FromStore 'TrustedPublisher' ($thumb -replace '\s', '').ToUpperInvariant() 'signer certificate' }
        else { Write-Host 'signer certificate left alone (give -Package or -SignerThumbprint to remove it)' }
        exit 0
    }

    # Read the signer before adding anything, so a package signed by the wrong party adds nothing.
    $signer = $null
    if ($Package) {
        $file = Find-SignedFile $Package
        $sig = Get-AuthenticodeSignature -LiteralPath $file
        if (-not $sig.SignerCertificate) { throw "$file has no signature" }
        $signer = $sig.SignerCertificate
        # Before the root is trusted, a package that is signed correctly reads NotTrusted (or UnknownError).
        # One whose bytes changed after signing reads HashMismatch, and nothing is added for it.
        if ($sig.Status.ToString() -notin 'Valid', 'NotTrusted', 'UnknownError') {
            throw "the signature on $file is $($sig.Status) ($($sig.StatusMessage)); nothing was added"
        }
        if (-not $sig.TimeStamperCertificate) { Write-Warning "$file has no timestamp; it stops being valid when the signer's certificate expires" }
        $chain = New-Object Security.Cryptography.X509Certificates.X509Chain
        $chain.ChainPolicy.RevocationMode = 'NoCheck'
        $chain.ChainPolicy.VerificationFlags = [Security.Cryptography.X509Certificates.X509VerificationFlags]::AllowUnknownCertificateAuthority
        [void]$chain.ChainPolicy.ExtraStore.Add($root)
        [void]$chain.ChainPolicy.ExtraStore.Add($intermediate)
        [void]$chain.Build($signer)
        $top = $chain.ChainElements[$chain.ChainElements.Count - 1].Certificate
        if ($top.Thumbprint -ne $root.Thumbprint) {
            throw "$file is not signed under the root you named (its signer chains to $($top.Subject))"
        }
        Write-Host "signer       : $($signer.Subject)  (expires $($signer.NotAfter.ToString('yyyy-MM-dd')))"
        Write-Host "  signed file: $file"
    }

    # Every check that can be made without trusting the root has passed. From here on a failure
    # takes out what this run added.
    try {
        Add-ToStore 'Root' $root 'root certificate' -CodeSigningOnly
        Add-ToStore 'CA' $intermediate 'intermediate certificate'
        if ($signer) {
            if ($PSCmdlet.ShouldProcess($file, 'check the signature now that the root is trusted')) {
                $after = Get-AuthenticodeSignature -LiteralPath $file
                if ($after.Status -ne 'Valid') {
                    throw "the signature on $file is $($after.Status) ($($after.StatusMessage)); signer not added to Trusted Publishers"
                }
            }
            Add-ToStore 'TrustedPublisher' $signer 'signer certificate'
        } else {
            Write-Host 'no -Package given: the signer certificate was not added (pnputil needs it)'
        }
    }
    catch {
        Undo-Added
        throw
    }
    Write-Host 'done. Next: pnputil /add-driver vsscom_um.inf /install  (and install-helper.ps1 for the helper)'
    exit 0
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
