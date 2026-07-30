<#
.SYNOPSIS
    Converts RSA-2048 public keys into src/rsakeys.cpp + src/rsakeys.h.

.DESCRIPTION
    The FTP4DOS updater verifies the signature over UPDATE.INF against public
    keys compiled into the executable. This script turns the PEM public keys
    into the C arrays that rsaverify.cpp consumes.

    Takes PUBLIC keys only. The private keys must never be handed to tooling -
    extract the public half once with

        openssl rsa -in ftp4dos-sign-1.pem -pubout -out ftp4dos-pub-1.pem

    and keep the private key offline.

    Two keys are embedded on purpose. A shipped DOS binary only ever trusts the
    keys it was compiled with, so if the primary key is lost there is no way to
    reach existing installations any more. The reserve key is the insurance:
    generate it now, store it offline, and never sign with it until you have to.

.EXAMPLE
    pwsh tools/pubkey-to-c.ps1 -Primary keys/ftp4dos-pub-1.pem -Reserve keys/ftp4dos-pub-2.pem
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Primary,
    [Parameter(Mandatory = $true)][string]$Reserve,
    [string]$OutDir = "src",
    [string]$OpenSsl,
    [switch]$TestKeys
)

$ErrorActionPreference = 'Stop'

function Resolve-OpenSsl {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "openssl not found at '$Explicit'" }
        return $Explicit
    }

    $cmd = Get-Command openssl -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # Git for Windows ships one but does not put it on the PowerShell PATH.
    $candidates = @(
        "$env:ProgramFiles\Git\mingw64\bin\openssl.exe",
        "$env:ProgramFiles\Git\usr\bin\openssl.exe",
        "${env:ProgramFiles(x86)}\Git\mingw64\bin\openssl.exe",
        "$env:LOCALAPPDATA\Programs\Git\mingw64\bin\openssl.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }

    throw "openssl not found. Install it or pass -OpenSsl <path\to\openssl.exe>."
}

$script:OpenSslExe = Resolve-OpenSsl -Explicit $OpenSsl

function Get-RsaModulus {
    param([string]$PemPath, [string]$Label)

    if (-not (Test-Path $PemPath)) { throw "$Label : file not found: $PemPath" }

    $pem = Get-Content -Raw $PemPath
    if ($pem -notmatch 'BEGIN PUBLIC KEY|BEGIN RSA PUBLIC KEY') {
        throw "$Label : '$PemPath' is not a public key. Never pass private keys to this script - extract the public half with 'openssl rsa -in <priv> -pubout -out <pub>'."
    }

    # Key size and exponent must match what rsaverify.cpp implements. e=65537 is
    # hard-coded there as 16 squarings plus one multiply, and the limb count
    # assumes exactly 2048 bits.
    # -join is not cosmetic: openssl returns an array of lines, and -match on an
    # array filters it instead of returning a boolean.
    $text = (& $script:OpenSslExe rsa -pubin -in $PemPath -text -noout 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "$Label : openssl could not read '$PemPath': $text" }

    if ($text -notmatch 'Public-Key:\s*\((\d+)\s*bit\)') { throw "$Label : cannot determine key size" }
    $bits = [int]$Matches[1]
    if ($bits -ne 2048) { throw "$Label : key is $bits bit, but the DOS verifier implements RSA-2048 only" }

    if ($text -notmatch 'Exponent:\s*(\d+)') { throw "$Label : cannot determine public exponent" }
    $e = [int]$Matches[1]
    if ($e -ne 65537) { throw "$Label : public exponent is $e, but the DOS verifier hard-codes 65537" }

    $modLine = (& $script:OpenSslExe rsa -pubin -in $PemPath -modulus -noout 2>&1) -join ""
    if ($LASTEXITCODE -ne 0) { throw "$Label : openssl -modulus failed: $modLine" }
    $hex = ($modLine -replace '^Modulus=', '').Trim()
    if ($hex.Length -ne 512) { throw "$Label : modulus is $($hex.Length) hex chars, expected 512" }

    $bytes = [byte[]]::new(256)
    for ($i = 0; $i -lt 256; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }

    # Fingerprint so a human can tell at a glance which key a build trusts.
    $sha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
    $fp = (($sha[0..7] | ForEach-Object { $_.ToString('x2') }) -join ':')

    return [pscustomobject]@{ Bytes = $bytes; Fingerprint = $fp }
}

function Format-CArray {
    param([byte[]]$Bytes, [string]$Name, [string]$Comment)

    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine("/* $Comment */")
    [void]$sb.AppendLine("const unsigned char $Name[RSA_MODULUS_BYTES] = {")
    for ($i = 0; $i -lt $Bytes.Length; $i += 12) {
        $slice = $Bytes[$i..([Math]::Min($i + 11, $Bytes.Length - 1))]
        $line = ($slice | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', '
        $suffix = if (($i + 12) -lt $Bytes.Length) { ',' } else { '' }
        [void]$sb.AppendLine("    $line$suffix")
    }
    [void]$sb.AppendLine("};")
    return $sb.ToString()
}

$p = Get-RsaModulus -PemPath $Primary -Label 'primary'
$r = Get-RsaModulus -PemPath $Reserve -Label 'reserve'

if ([Convert]::ToBase64String($p.Bytes) -eq [Convert]::ToBase64String($r.Bytes)) {
    throw "primary and reserve are the same key - that defeats the point of having a reserve"
}

$warning = if ($TestKeys) {
    @"

 * ###################################################################
 * ##  BUILT WITH TEST KEYS - MUST NOT BE RELEASED                  ##
 * ##  Regenerate from the production public keys before shipping.  ##
 * ###################################################################
"@
} else { "" }

$header = @"
/* Generated by tools/pubkey-to-c.ps1 - do not edit by hand.
 *
 * Public keys the updater trusts when verifying the RSA-2048 signature over
 * UPDATE.INF. Two keys are accepted: a shipped binary can never be taught a
 * new key, so the reserve is the only way back if the primary is lost.
 *
 * primary fingerprint: $($p.Fingerprint)
 * reserve fingerprint: $($r.Fingerprint)$warning
 */
#ifndef RSAKEYS_H
#define RSAKEYS_H

#define RSA_MODULUS_BYTES 256

extern const unsigned char rsa_key_primary[RSA_MODULUS_BYTES];
extern const unsigned char rsa_key_reserve[RSA_MODULUS_BYTES];

#endif /* RSAKEYS_H */
"@

$source = @"
/* Generated by tools/pubkey-to-c.ps1 - do not edit by hand.
 *
 * Compiled with a low -zt threshold (see MAKEFILE) so these arrays land in a
 * FAR_DATA segment instead of eating scarce DGROUP space.
 *
 * primary fingerprint: $($p.Fingerprint)
 * reserve fingerprint: $($r.Fingerprint)$warning
 */
#include "rsakeys.h"

$(Format-CArray -Bytes $p.Bytes -Name 'rsa_key_primary' -Comment "Primary signing key, fingerprint $($p.Fingerprint)")
$(Format-CArray -Bytes $r.Bytes -Name 'rsa_key_reserve' -Comment "Reserve signing key, fingerprint $($r.Fingerprint)")
"@

# LF endings and no BOM, matching the rest of src/.
$hPath = Join-Path $OutDir 'rsakeys.h'
$cPath = Join-Path $OutDir 'rsakeys.cpp'
[System.IO.File]::WriteAllText((Resolve-Path $OutDir).Path + '\rsakeys.h', ($header -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
[System.IO.File]::WriteAllText((Resolve-Path $OutDir).Path + '\rsakeys.cpp', ($source -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))

Write-Host "wrote $hPath and $cPath"
Write-Host "  primary fingerprint: $($p.Fingerprint)"
Write-Host "  reserve fingerprint: $($r.Fingerprint)"
if ($TestKeys) { Write-Host "  WARNING: marked as TEST KEYS - do not release this build" -ForegroundColor Yellow }
