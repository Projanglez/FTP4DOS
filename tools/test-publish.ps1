<#
.SYNOPSIS
    Tests the guards in tools/publish-update.ps1.

.DESCRIPTION
    Both guards protect against mistakes that are invisible until a user tries
    to update and cannot, so they are worth testing rather than trusting:

      A  a test-key build must be refused
      B  a signing key the binary does not trust must be refused
      C  the primary key must be accepted
      D  the reserve key must be accepted too

    src/rsakeys.cpp is regenerated during the run and restored afterwards, so
    the working tree is left exactly as it was found. Everything runs with
    -DryRun; nothing is uploaded anywhere.

.EXAMPLE
    pwsh tools/test-publish.ps1
#>
[CmdletBinding()]
param([string]$OpenSsl)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$openssl = Resolve-OpenSsl -Explicit $OpenSsl
$repo = Split-Path -Parent $PSScriptRoot
$work = Join-Path ([System.IO.Path]::GetTempPath()) "ftp4dos-pubtest"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$pass = 0; $fail = 0
function Check {
    param([string]$What, [bool]$Ok, [string]$Detail = "")
    if ($Ok) { Write-Host ("  {0,-52} ok" -f $What); $script:pass++ }
    else     { Write-Host ("  {0,-52} FAIL {1}" -f $What, $Detail) -ForegroundColor Red; $script:fail++ }
}

# Run publish-update.ps1 and capture whether it succeeded plus its output.
function Publish {
    param([string]$Key)
    $out = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'publish-update.ps1') `
        -Version 9.9.9 -PrivateKey $Key -Notes "guard test" -DryRun 2>&1 | Out-String
    return [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Text = $out }
}

$keysCpp = Join-Path $repo 'src\rsakeys.cpp'
$keysH   = Join-Path $repo 'src\rsakeys.h'
$bakCpp  = Join-Path $work 'rsakeys.cpp.bak'
$bakH    = Join-Path $work 'rsakeys.h.bak'
Copy-Item $keysCpp $bakCpp -Force
Copy-Item $keysH   $bakH   -Force

try {
    Write-Host "generating keys"
    foreach ($n in @('prim', 'res', 'other')) {
        & $openssl genrsa -out (Join-Path $work "$n.pem") 2048 2>$null
        & $openssl rsa -in (Join-Path $work "$n.pem") -pubout -out (Join-Path $work "$n-pub.pem") 2>$null
    }

    Write-Host ""
    Write-Host "A: test-key build must be refused"
    # Regenerate deliberately marked as test keys.
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'pubkey-to-c.ps1') `
        -Primary (Join-Path $work 'prim-pub.pem') -Reserve (Join-Path $work 'res-pub.pem') `
        -OutDir (Join-Path $repo 'src') -TestKeys | Out-Null
    $r = Publish -Key (Join-Path $work 'prim.pem')
    Check "test-key build refused" (-not $r.Ok)
    Check "  and says why" ($r.Text -match 'TEST KEY')

    Write-Host ""
    Write-Host "B-D: production-marked build"
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'pubkey-to-c.ps1') `
        -Primary (Join-Path $work 'prim-pub.pem') -Reserve (Join-Path $work 'res-pub.pem') `
        -OutDir (Join-Path $repo 'src') | Out-Null

    $r = Publish -Key (Join-Path $work 'other.pem')
    Check "B: untrusted signing key refused" (-not $r.Ok)
    Check "  and says why" ($r.Text -match 'does not verify against either')

    $r = Publish -Key (Join-Path $work 'prim.pem')
    Check "C: primary key accepted" $r.Ok
    Check "  verified against the embedded primary" ($r.Text -match 'verifies against rsa_key_primary')
    Check "  manifest carries the sha256" ($r.Text -match 'sha256=[0-9a-f]{64}')
    Check "  manifest carries the size" ($r.Text -match 'size=\d+')

    $r = Publish -Key (Join-Path $work 'res.pem')
    Check "D: reserve key accepted" $r.Ok
    Check "  verified against the embedded reserve" ($r.Text -match 'verifies against rsa_key_reserve')
}
finally {
    Copy-Item $bakCpp $keysCpp -Force
    Copy-Item $bakH   $keysH   -Force
    Write-Host ""
    Write-Host "src/rsakeys.cpp restored"
}

Write-Host ""
if ($fail -eq 0) { Write-Host "publisher guard tests passed ($pass checks)" -ForegroundColor Green; exit 0 }
else             { Write-Host "PUBLISHER GUARD TESTS FAILED ($fail of $($pass+$fail))" -ForegroundColor Red; exit 1 }
