<#
.SYNOPSIS
    Generates the two RSA-2048 signing keys for the update channel.

.DESCRIPTION
    Creates a primary and a reserve key pair and extracts their public halves.

    The passphrases are prompted for by OpenSSL itself - this script never sees,
    stores or passes them, which is the whole reason it does not accept them as
    parameters.

    Why two keys: a shipped DOS binary only ever trusts the keys it was compiled
    with, and there is no way to teach an installed copy a new one. Lose the
    primary without a reserve and the update channel is dead for every existing
    installation. Generate both now, sign with the primary, and put the reserve
    somewhere offline that is not this machine.

    Keys are written outside the repository by default. The private halves must
    never be committed - .gitignore covers the usual names, but the reliable
    protection is keeping them out of the working tree entirely.

.EXAMPLE
    pwsh tools/genkeys.ps1
    pwsh tools/genkeys.ps1 -OutDir D:\secure\ftp4dos-keys
#>
[CmdletBinding()]
param(
    [string]$OutDir = (Join-Path $HOME 'ftp4dos-keys'),
    [string]$OpenSsl
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$openssl = Resolve-OpenSsl -Explicit $OpenSsl
Write-Host "using $openssl"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

$repoRoot = (Resolve-Path (Split-Path -Parent $PSScriptRoot)).Path
if ((Resolve-Path $OutDir).Path.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "WARNING: $OutDir is inside the repository. Private keys do not belong in a working tree." -ForegroundColor Yellow
}

$keys = @(
    @{ Name = 'ftp4dos-sign-1'; Role = 'PRIMARY - used for signing releases' },
    @{ Name = 'ftp4dos-sign-2'; Role = 'RESERVE - store offline, do not use until needed' }
)

foreach ($k in $keys) {
    $priv = Join-Path $OutDir "$($k.Name).pem"
    $pub  = Join-Path $OutDir "$($k.Name)-pub.pem"

    if (Test-Path $priv) {
        Write-Host "skipping $($k.Name): $priv already exists (refusing to overwrite a key)" -ForegroundColor Yellow
        continue
    }

    Write-Host ""
    Write-Host "--- $($k.Name) : $($k.Role) ---" -ForegroundColor Cyan
    Write-Host "OpenSSL will now ask for a passphrase. Use a different one per key."

    & $openssl genrsa -aes256 -out $priv 2048
    if ($LASTEXITCODE -ne 0) { throw "key generation failed for $($k.Name)" }

    Write-Host "Enter the same passphrase again to extract the public half:"
    & $openssl rsa -in $priv -pubout -out $pub
    if ($LASTEXITCODE -ne 0) { throw "could not extract the public key for $($k.Name)" }

    # e=65537 is what rsaverify.cpp implements; anything else would build a
    # binary that cannot verify its own updates.
    $text = (& $openssl rsa -pubin -in $pub -text -noout 2>&1) -join "`n"
    if ($text -notmatch 'Public-Key:\s*\(2048 bit\)') { throw "$($k.Name) is not 2048 bit" }
    if ($text -notmatch 'Exponent:\s*65537')          { throw "$($k.Name) does not use e=65537" }
    Write-Host "ok: 2048 bit, e=65537" -ForegroundColor Green
}

Write-Host ""
Write-Host "Keys are in $OutDir" -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  pwsh tools/pubkey-to-c.ps1 -Primary `"$OutDir\ftp4dos-sign-1-pub.pem`" -Reserve `"$OutDir\ftp4dos-sign-2-pub.pem`""
Write-Host "  wmake"
Write-Host "  pwsh tools/publish-update.ps1 -Version <x.y.z> -PrivateKey `"$OutDir\ftp4dos-sign-1.pem`" -DryRun"
Write-Host ""
Write-Host "Then move ftp4dos-sign-2.pem somewhere offline and off this machine." -ForegroundColor Yellow
Write-Host "Without it, losing the primary key permanently ends updates for every installed copy."
