<#
.SYNOPSIS
    Signs the update manifest and attaches it to a GitHub release.

.DESCRIPTION
    Builds UPDATE.INF from the executable being released, signs it with the
    private key, and uploads UPDATE.INF, UPDATE.SIG and FTP4DOS.EXE as release
    assets. The Cloudflare Worker serves those three from the latest release, so
    this is the whole publishing step for the update channel.

    Two checks run before anything is uploaded, because both mistakes are
    invisible until a user tries to update and cannot:

      1. The signing key must match a public key compiled into the executable.
         Signing with a key the shipped binaries do not trust produces an
         update that every client correctly refuses.

      2. src/rsakeys.cpp must not be a test-key build. Those are marked, and
         shipping one would mean shipping a binary that trusts a throwaway key
         whose private half sat in a temp directory.

    The signature covers the exact bytes of UPDATE.INF as written here, and the
    client hashes exactly the bytes it receives - so this script writes LF
    endings and no BOM, and nothing may reformat the file afterwards.

.EXAMPLE
    pwsh tools/publish-update.ps1 -Version 1.0.2 -PrivateKey C:\keys\ftp4dos-sign-1.pem `
         -Notes "LFN fix; faster MLSD"

.EXAMPLE
    pwsh tools/publish-update.ps1 -Version 1.0.2 -PrivateKey ... -DryRun
    # writes and verifies everything locally, uploads nothing
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$PrivateKey,
    [string]$Notes = "",
    [string]$Exe   = "FTP4DOS.EXE",
    [string]$Tag,
    [string]$Repo  = "Projanglez/FTP4DOS",
    [string]$OpenSsl,
    [string]$PassFile,
    [switch]$Republish,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$openssl = Resolve-OpenSsl -Explicit $OpenSsl
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Tag) { $Tag = "v$Version" }

$exePath = if ([System.IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $repoRoot $Exe }
if (-not (Test-Path $exePath)) { throw "executable not found: $exePath (run wmake first)" }

$keysCpp = Join-Path $repoRoot "src\rsakeys.cpp"
if (-not (Test-Path $keysCpp)) { throw "src/rsakeys.cpp not found - generate it with tools/pubkey-to-c.ps1" }

# ---------------------------------------------------------------------------
# Guard 1: refuse to publish a test-key build
# ---------------------------------------------------------------------------
$keysText = Get-Content -Raw $keysCpp
if ($keysText -match 'BUILT WITH TEST KEYS') {
    throw @"
src/rsakeys.cpp is a TEST KEY build.

Regenerate it from the production public keys and rebuild before publishing:
  pwsh tools/pubkey-to-c.ps1 -Primary <pub1.pem> -Reserve <pub2.pem>
  wmake
"@
}

# ---------------------------------------------------------------------------
# Guard 2 is applied after signing (below): the signature is checked against
# the public keys actually compiled into the executable. That is a stronger
# test than comparing moduli - it exercises the same operation the DOS client
# performs - and it needs the private key only once, so OpenSSL asks for the
# passphrase a single time.
# ---------------------------------------------------------------------------
function Get-EmbeddedPublicKeys {
    param([string]$Text, [string]$Dir)
    $out = [ordered]@{}
    foreach ($name in @('rsa_key_primary', 'rsa_key_reserve')) {
        if ($Text -notmatch "(?s)$name\s*\[[^\]]*\]\s*=\s*\{(.*?)\};") {
            throw "cannot find $name in src/rsakeys.cpp"
        }
        $bytes = [byte[]]([regex]::Matches($Matches[1], '0x([0-9a-fA-F]{2})') |
                 ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
        if ($bytes.Length -ne 256) { throw "$name has $($bytes.Length) bytes, expected 256" }

        # Rebuild a PEM public key from the modulus and the exponent
        # rsaverify.cpp hard-codes (65537), so what we verify against is
        # exactly what the shipped binary carries.
        $p = [System.Security.Cryptography.RSAParameters]::new()
        $p.Modulus  = $bytes
        $p.Exponent = [byte[]]@(0x01, 0x00, 0x01)
        $rsa = [System.Security.Cryptography.RSA]::Create()
        $rsa.ImportParameters($p)
        $pem = Join-Path $Dir "$name.pem"
        [System.IO.File]::WriteAllText($pem, $rsa.ExportSubjectPublicKeyInfoPem())
        $rsa.Dispose()
        $out[$name] = $pem
    }
    return $out
}

$privText = Get-Content -Raw $PrivateKey
if ($privText -notmatch 'PRIVATE KEY') { throw "$PrivateKey is not a private key" }

# ---------------------------------------------------------------------------
# Guard 3: the executable must actually carry the version being published
#
# APP_VERSION is compiled into several strings ("Help - FTP4DOS v1.2.0 ..."), so
# the binary can be asked what it thinks it is. Two mistakes this catches, both
# of which fail silently in the field:
#
#   - Publishing a build that still says 1.2.0-dev under the name 1.2.1. Clients
#     install it, still report the -dev version, and are offered the same update
#     forever - an update loop nobody notices.
#   - Publishing an executable that was never rebuilt after the version bump.
# ---------------------------------------------------------------------------
$exeText = [System.Text.Encoding]::GetEncoding(28591).GetString(
               [System.IO.File]::ReadAllBytes($exePath))
# @() matters: a single match would otherwise come back as a bare string, and
# $stamped[0] would index its first CHARACTER instead of the version.
$stamped = @([regex]::Matches($exeText, 'FTP4DOS v([0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+)?)') |
             ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)

if (-not $stamped) {
    throw "cannot find a version string in $exePath - is it really an FTP4DOS build?"
}
if ($stamped.Count -gt 1) {
    throw "found conflicting version strings in ${exePath}: $($stamped -join ', ')"
}
if ($stamped[0] -ne $Version) {
    throw @"
$Exe reports version '$($stamped[0])', but you are publishing '$Version'.

Set APP_VERSION in src/ncftp.cpp to '$Version', rebuild with wmake, and run
this again. Publishing as-is would leave every client reporting
'$($stamped[0])' after updating, so they would be offered this same update on
every check.
"@
}
Write-Host "executable reports version $($stamped[0])" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Guard 4: the version has to be newer than the one already published
#
# Forgetting the bump is invisible otherwise: the release goes out, every client
# says "up to date", and it reaches nobody. Re-running for a version that is
# already live is legitimate (replacing an asset), so that needs -Republish
# rather than being impossible.
# ---------------------------------------------------------------------------
function ConvertTo-VersionParts {
    param([string]$V)
    if ($V -match '^v?([0-9]+)\.([0-9]+)\.([0-9]+)') {
        return [int[]]@($Matches[1], $Matches[2], $Matches[3])
    }
    return $null
}

$latestTag = & gh api "repos/$Repo/releases/latest" --jq '.tag_name' 2>$null
if ($LASTEXITCODE -ne 0 -or -not $latestTag) {
    Write-Host "could not read the current latest release - skipping the version check" -ForegroundColor Yellow
} else {
    $new = ConvertTo-VersionParts $Version
    $old = ConvertTo-VersionParts $latestTag
    if ($new -and $old) {
        $cmp = 0
        for ($i = 0; $i -lt 3 -and $cmp -eq 0; $i++) {
            if ($new[$i] -ne $old[$i]) { $cmp = if ($new[$i] -lt $old[$i]) { -1 } else { 1 } }
        }
        if ($cmp -le 0 -and -not $Republish) {
            throw @"
Version $Version is not newer than the published $latestTag.

Every client compares version numbers, so this update would be invisible: they
would all report "up to date" and never fetch it.

Bump APP_VERSION and rebuild, or pass -Republish if you really mean to replace
the assets of a version that is already out.
"@
        }
        if ($cmp -le 0) {
            Write-Host "republishing $Version over $latestTag as requested" -ForegroundColor Yellow
        } else {
            Write-Host "version $Version supersedes the published $latestTag" -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------
$size = (Get-Item $exePath).Length
$sha  = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLower()
$date = (Get-Date).ToString('yyyy-MM-dd')

# The client's parser is line-based key=value and ignores unknown keys. LF and
# no BOM, because these exact bytes are what gets signed and what gets hashed
# on the DOS side.
$manifest = "# FTP4DOS update manifest`n" +
            "version=$Version`n" +
            "date=$date`n" +
            "size=$size`n" +
            "sha256=$sha`n" +
            "file=/FTP4DOS.EXE`n" +
            "notes=$($Notes -replace '[\r\n]', ' ')`n"

$outDir = Join-Path ([System.IO.Path]::GetTempPath()) "ftp4dos-publish"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$infPath = Join-Path $outDir "UPDATE.INF"
$sigPath = Join-Path $outDir "UPDATE.SIG"
[System.IO.File]::WriteAllText($infPath, $manifest, (New-Object System.Text.UTF8Encoding $false))

# Uses the DPAPI-stored passphrase when tools/store-keypass.ps1 has been run for
# this key; otherwise OpenSSL prompts, exactly as before.
Invoke-OpenSslWithKey -OpenSslExe $openssl -PrivateKey $PrivateKey -PassFile $PassFile `
    -Arguments @('dgst', '-sha256', '-sign', $PrivateKey, '-out', $sigPath, $infPath)
if ($LASTEXITCODE -ne 0) { throw "signing failed" }

$sigLen = (Get-Item $sigPath).Length
if ($sigLen -ne 256) { throw "signature is $sigLen bytes, expected 256 (is the key RSA-2048?)" }

# ---------------------------------------------------------------------------
# Guard 2: verify the signature against the keys compiled into the executable
#
# This is the same check the DOS client makes, against the same key material,
# so passing here means an installed FTP4DOS will accept this update. Failing
# means every client would refuse it - the one publishing mistake that cannot
# be noticed after the fact.
# ---------------------------------------------------------------------------
$embedded = Get-EmbeddedPublicKeys -Text $keysText -Dir $outDir
$which = $null
foreach ($name in $embedded.Keys) {
    & $openssl dgst -sha256 -verify $embedded[$name] -signature $sigPath $infPath 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $which = $name; break }
}
if (-not $which) {
    throw @"
The signature does not verify against either public key compiled into $Exe.

Every client would refuse this update. Either sign with the primary or reserve
key, or regenerate src/rsakeys.cpp from the matching public keys and rebuild.
"@
}
Write-Host "signature verifies against $which" -ForegroundColor Green

Write-Host ""
Write-Host "manifest:"
Get-Content $infPath | ForEach-Object { Write-Host "  $_" }
Write-Host ""

if ($DryRun) {
    Write-Host "dry run - nothing uploaded. Files are in $outDir" -ForegroundColor Yellow
    exit 0
}

# ---------------------------------------------------------------------------
# Upload
# ---------------------------------------------------------------------------
$release = & gh release view $Tag --repo $Repo --json tagName,assets 2>&1
if ($LASTEXITCODE -ne 0) {
    throw @"
Release $Tag does not exist in $Repo.

Create it first (with the release notes and the mTCP corresponding-source zip),
then run this script to attach the update-channel assets.
"@
}

$assets = ($release | ConvertFrom-Json).assets.name
if ($assets -notcontains 'ftp4dos-mtcp-src-2025-01-10.zip') {
    Write-Host "WARNING: no mTCP source zip on $Tag - GPLv3 section 6 requires shipping the corresponding sources." -ForegroundColor Yellow
}

# --clobber so re-running replaces the assets instead of failing.
& gh release upload $Tag $exePath $infPath $sigPath --repo $Repo --clobber
if ($LASTEXITCODE -ne 0) { throw "upload failed" }

Write-Host ""
Write-Host "published to $Tag" -ForegroundColor Green
Write-Host "  FTP4DOS.EXE  $size bytes"
Write-Host "  SHA-256      $sha"
Write-Host ""
Write-Host "Put the SHA-256 in the release notes so it can be checked by hand."
Write-Host "Then confirm the channel is live:"
Write-Host "  curl -s -o /dev/null -w '%{http_code}\n' http://ftp4dos-update.ftp4dos-update.workers.dev/UPDATE.INF"
