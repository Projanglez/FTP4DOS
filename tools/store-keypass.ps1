<#
.SYNOPSIS
    Stores the signing key's passphrase so releases stop prompting for it.

.DESCRIPTION
    Asks for the passphrase once and writes it next to the key as <key>.pass,
    encrypted with DPAPI. That blob can only be decrypted by the Windows account
    that wrote it, on the machine that wrote it - a copy, backup or cloud sync of
    the file is useless to anyone else. The key itself stays passphrase-protected;
    nothing about the .pem changes.

    Before writing anything the passphrase is proven correct by actually signing
    a scratch file with it. A typo would otherwise sit there undetected until the
    next release failed.

    Understand what this buys and what it costs: after this, anything running as
    your Windows account can sign an update that every installed FTP4DOS accepts.
    The passphrase prompt was the last point where a release needed you present.
    Delete the .pass file to go back to being asked.

.EXAMPLE
    pwsh tools/store-keypass.ps1 -PrivateKey "$HOME\ftp4dos-keys\ftp4dos-sign-1.pem"

.EXAMPLE
    pwsh tools/store-keypass.ps1 -PrivateKey ... -Remove
    # deletes the stored passphrase; releases prompt again
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PrivateKey,
    [string]$PassFile,
    [string]$OpenSsl,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

if (-not (Test-Path $PrivateKey)) { throw "private key not found: $PrivateKey" }
$target = Get-PassFilePath -PrivateKey $PrivateKey -Explicit $PassFile

if ($Remove) {
    if (Test-Path $target) {
        Remove-Item $target -Force
        Write-Host "removed $target - releases will prompt for the passphrase again" -ForegroundColor Yellow
    } else {
        Write-Host "nothing stored at $target"
    }
    exit 0
}

$openssl = Resolve-OpenSsl -Explicit $OpenSsl

$privText = Get-Content -Raw $PrivateKey
if ($privText -notmatch 'PRIVATE KEY') { throw "$PrivateKey is not a private key" }
if ($privText -notmatch 'ENCRYPTED') {
    Write-Host "WARNING: $PrivateKey has no passphrase - there is nothing to store." -ForegroundColor Yellow
    Write-Host "         The key file alone is enough to sign releases." -ForegroundColor Yellow
    exit 1
}

$sec = Read-Host -AsSecureString "Passphrase for $(Split-Path -Leaf $PrivateKey)"
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try   { $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
if ([string]::IsNullOrEmpty($plain)) { throw "no passphrase entered" }

# Prove it before storing it: sign a scratch file for real.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("keypass-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $msg = Join-Path $tmp "probe.txt"
    $sig = Join-Path $tmp "probe.sig"
    Set-Content -Path $msg -Value "ftp4dos passphrase probe" -NoNewline

    $env:FTP4DOS_KEYPASS = $plain
    try {
        & $openssl dgst -sha256 -sign $PrivateKey -passin env:FTP4DOS_KEYPASS -out $sig $msg 2>&1 | Out-Null
    } finally {
        Remove-Item Env:\FTP4DOS_KEYPASS -ErrorAction SilentlyContinue
    }

    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $sig) -or (Get-Item $sig).Length -ne 256) {
        throw "that passphrase does not unlock $PrivateKey - nothing was stored"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$blob = ConvertTo-SecureString $plain -AsPlainText -Force | ConvertFrom-SecureString
Set-Content -Path $target -Value $blob -NoNewline

Write-Host ""
Write-Host "passphrase verified and stored: $target" -ForegroundColor Green
Write-Host "  readable only by $env:USERDOMAIN\$env:USERNAME on $env:COMPUTERNAME"
Write-Host ""
Write-Host "publish-update.ps1 will now sign without prompting."
Write-Host "To undo:  pwsh tools/store-keypass.ps1 -PrivateKey `"$PrivateKey`" -Remove"
