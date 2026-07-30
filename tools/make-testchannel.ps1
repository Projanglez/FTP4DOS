<#
.SYNOPSIS
    Builds a signed update channel on this machine, for testing the updater on
    real hardware.

.DESCRIPTION
    Testing the auto-updater end to end needs a version NEWER than the one
    running. Once v1.1.0 is the published release, a 386 running v1.1.0 will
    correctly report "you already have the newest version" - which proves the
    check works but exercises none of the download, verification or swap.

    This script produces a local channel advertising a higher version, signed
    with the real production key, so the DOS side runs exactly the code path it
    would for a genuine update - signature check included. Only the Cloudflare
    hop is left out, and that is already proven separately.

    Serve the directory over plain HTTP, point the DOS machine at it with
    FTP4DOS_UPDHOST / FTP4DOS_UPDPORT in MTCP.CFG, and press Alt+F10.

.EXAMPLE
    pwsh tools/make-testchannel.ps1 -Version 1.1.1 -PrivateKey "$HOME\ftp4dos-keys\ftp4dos-sign-1.pem"
#>
[CmdletBinding()]
param(
    [string]$Version = "1.9.9",
    [Parameter(Mandatory = $true)][string]$PrivateKey,
    [string]$Exe = "FTP4DOS.EXE",
    [string]$OutDir = "C:\ftp4dos-testchannel",
    [string]$Notes = "test channel - not a real release",
    [int]$Port = 8080,
    [string]$OpenSsl
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$openssl  = Resolve-OpenSsl -Explicit $OpenSsl
$repoRoot = Split-Path -Parent $PSScriptRoot
$exePath  = if ([System.IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $repoRoot $Exe }
if (-not (Test-Path $exePath)) { throw "executable not found: $exePath (run wmake first)" }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

Copy-Item $exePath (Join-Path $OutDir 'FTP4DOS.EXE') -Force
$size = (Get-Item (Join-Path $OutDir 'FTP4DOS.EXE')).Length
$sha  = (Get-FileHash (Join-Path $OutDir 'FTP4DOS.EXE') -Algorithm SHA256).Hash.ToLower()

# LF, no BOM: the client hashes exactly the bytes it receives, so what is
# signed here has to be byte-identical to what gets served.
$manifest = "# FTP4DOS update manifest`n" +
            "version=$Version`n" +
            "date=$((Get-Date).ToString('yyyy-MM-dd'))`n" +
            "size=$size`n" +
            "sha256=$sha`n" +
            "file=/FTP4DOS.EXE`n" +
            "notes=$($Notes -replace '[\r\n]', ' ')`n"

$inf = Join-Path $OutDir 'UPDATE.INF'
$sig = Join-Path $OutDir 'UPDATE.SIG'
[System.IO.File]::WriteAllText($inf, $manifest, (New-Object System.Text.UTF8Encoding $false))

Write-Host "OpenSSL will ask for the key passphrase."
& $openssl dgst -sha256 -sign $PrivateKey -out $sig $inf
if ($LASTEXITCODE -ne 0) { throw "signing failed" }

# Same check publish-update.ps1 makes: the signature must verify against a key
# the binary actually carries, or the 386 will refuse the update and the test
# will look like a bug in the client.
$keysText = Get-Content -Raw (Join-Path $repoRoot 'src\rsakeys.cpp')
$ok = $false
foreach ($name in @('rsa_key_primary', 'rsa_key_reserve')) {
    if ($keysText -notmatch "(?s)$name\s*\[[^\]]*\]\s*=\s*\{(.*?)\};") { continue }
    $bytes = [byte[]]([regex]::Matches($Matches[1], '0x([0-9a-fA-F]{2})') |
             ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
    $p = [System.Security.Cryptography.RSAParameters]::new()
    $p.Modulus = $bytes; $p.Exponent = [byte[]]@(1,0,1)
    $rsa = [System.Security.Cryptography.RSA]::Create(); $rsa.ImportParameters($p)
    $pem = Join-Path $env:TEMP "$name-testchan.pem"
    [System.IO.File]::WriteAllText($pem, $rsa.ExportSubjectPublicKeyInfoPem()); $rsa.Dispose()
    & $openssl dgst -sha256 -verify $pem -signature $sig $inf 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $ok = $true; Write-Host "signature verifies against $name" -ForegroundColor Green; break }
}
if (-not $ok) { throw "the signature does not verify against the keys in $Exe - the DOS side would refuse it" }

$ip = (Get-NetIPAddress -AddressFamily IPv4 |
       Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' } |
       Select-Object -First 1 -ExpandProperty IPAddress)

Write-Host ""
Write-Host "channel ready in $OutDir" -ForegroundColor Green
Write-Host "  version $Version, $size bytes"
Write-Host "  sha256  $sha"
Write-Host ""
Write-Host "1. Serve it:"
Write-Host "     py -m http.server $Port --bind 0.0.0.0 --directory `"$OutDir`""
Write-Host ""
Write-Host "2. On the 386, add to MTCP.CFG (key and value separated by a space,"
Write-Host "   no trailing whitespace):"
Write-Host "     FTP4DOS_UPDHOST $ip"
Write-Host "     FTP4DOS_UPDPORT $Port"
Write-Host ""
Write-Host "3. Start FTP4DOS and press Alt+F10."
Write-Host ""
Write-Host "Remove those two lines afterwards, or the machine keeps looking at" -ForegroundColor Yellow
Write-Host "this test channel instead of the real one."
