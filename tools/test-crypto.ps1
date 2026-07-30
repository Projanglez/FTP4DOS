<#
.SYNOPSIS
    Tests src/sha256.cpp and src/rsaverify.cpp against OpenSSL.

.DESCRIPTION
    Builds tools/crypttest.cpp natively, runs the SHA-256 self-test, then checks
    RSA verification against signatures OpenSSL actually produced - and, more
    importantly, checks that every way of tampering with them is rejected.

    The negative cases are the point of this script. A verifier that accepts
    valid signatures is easy; one that accepts nothing else is the hard part,
    and it is what stands between a plain-HTTP update channel and arbitrary code
    execution on the user's machine.

    Key material is generated per run and thrown away. Nothing is committed.

.EXAMPLE
    pwsh tools/test-crypto.ps1
#>
[CmdletBinding()]
param(
    [string]$OpenSsl,
    [string]$WatcomRoot = "C:\WATCOM",
    [string]$WorkDir
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$openssl = Resolve-OpenSsl -Explicit $OpenSsl
Enable-Watcom -Root $WatcomRoot

$repo = Split-Path -Parent $PSScriptRoot
if (-not $WorkDir) { $WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) "ftp4dos-crypttest" }
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$script:pass = 0
$script:fail = 0

function Expect {
    param([string]$What, [string]$Expected, [string]$SigFile, [string]$DataFile, [string]$ModFile)

    $out = & "$WorkDir\crypttest.exe" verify $ModFile $SigFile $DataFile 2>&1
    $got = ($out | Select-Object -Last 1).ToString().Trim()
    if ($got -eq $Expected) {
        Write-Host ("  {0,-46} ok" -f $What)
        $script:pass++
    } else {
        Write-Host ("  {0,-46} FAIL (expected $Expected, got '$got')" -f $What) -ForegroundColor Red
        $script:fail++
    }
}

Push-Location $WorkDir
try {
    # ---- build ------------------------------------------------------------
    Write-Host "building crypttest (wcl386 -bt=nt)"
    $build = & wcl386 -bt=nt -q -ox "-fe=crypttest.exe" `
        "$repo\tools\crypttest.cpp" "$repo\src\sha256.cpp" "$repo\src\rsaverify.cpp" 2>&1
    if (-not (Test-Path "$WorkDir\crypttest.exe")) {
        $build | Write-Host
        throw "build failed"
    }

    # ---- SHA-256 ----------------------------------------------------------
    Write-Host ""
    & "$WorkDir\crypttest.exe" selftest
    if ($LASTEXITCODE -ne 0) { $script:fail++ }

    # ---- key material and a signature -------------------------------------
    Write-Host ""
    Write-Host "RSA-2048 PKCS#1 v1.5 verification"
    & $openssl genrsa -out k1.pem 2048 2>$null
    & $openssl genrsa -out k2.pem 2048 2>$null
    & $openssl rsa -in k1.pem -pubout -out p1.pem 2>$null
    & $openssl rsa -in k2.pem -pubout -out p2.pem 2>$null

    [System.IO.File]::WriteAllBytes("$WorkDir\mod1.bin", (Get-ModulusBytes -OpenSslExe $openssl -PubPem "$WorkDir\p1.pem"))
    [System.IO.File]::WriteAllBytes("$WorkDir\mod2.bin", (Get-ModulusBytes -OpenSslExe $openssl -PubPem "$WorkDir\p2.pem"))

    # A realistic manifest, byte-for-byte what the DOS client would receive.
    $manifest = "# FTP4DOS update manifest`nversion=1.0.2`ndate=2026-08-15`nsize=272104`nsha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08`nfile=/FTP4DOS/FTP4DOS.EXE`nnotes=test`n"
    [System.IO.File]::WriteAllText("$WorkDir\UPDATE.INF", $manifest, (New-Object System.Text.UTF8Encoding $false))

    & $openssl dgst -sha256 -sign k1.pem -out sig.bin UPDATE.INF
    & $openssl dgst -sha256 -sign k2.pem -out sig-otherkey.bin UPDATE.INF

    Expect "genuine signature" "VALID" "$WorkDir\sig.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    # ---- negative cases ---------------------------------------------------
    $sig = [System.IO.File]::ReadAllBytes("$WorkDir\sig.bin")

    $t = $sig.Clone(); $t[200] = $t[200] -bxor 0x01
    [System.IO.File]::WriteAllBytes("$WorkDir\sig-bitflip.bin", $t)
    Expect "flipped bit in signature" "INVALID" "$WorkDir\sig-bitflip.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    $t = $sig.Clone(); $t[0] = $t[0] -bxor 0x80
    [System.IO.File]::WriteAllBytes("$WorkDir\sig-topbit.bin", $t)
    Expect "flipped top bit in signature" "INVALID" "$WorkDir\sig-topbit.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    $tampered = $manifest -replace 'version=1\.0\.2', 'version=9.9.9'
    [System.IO.File]::WriteAllText("$WorkDir\UPDATE-tampered.INF", $tampered, (New-Object System.Text.UTF8Encoding $false))
    Expect "tampered manifest, genuine signature" "INVALID" "$WorkDir\sig.bin" "$WorkDir\UPDATE-tampered.INF" "$WorkDir\mod1.bin"

    Expect "signature from a different key" "INVALID" "$WorkDir\sig-otherkey.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"
    Expect "genuine signature, wrong modulus" "INVALID" "$WorkDir\sig.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod2.bin"

    [System.IO.File]::WriteAllBytes("$WorkDir\sig-zero.bin", [byte[]]::new(256))
    Expect "s = 0" "INVALID" "$WorkDir\sig-zero.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    $one = [byte[]]::new(256); $one[255] = 1
    [System.IO.File]::WriteAllBytes("$WorkDir\sig-one.bin", $one)
    Expect "s = 1" "INVALID" "$WorkDir\sig-one.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    $mod = [System.IO.File]::ReadAllBytes("$WorkDir\mod1.bin")
    [System.IO.File]::WriteAllBytes("$WorkDir\sig-eqn.bin", $mod)
    Expect "s = n" "INVALID" "$WorkDir\sig-eqn.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    $big = $mod.Clone(); $big[0] = 0xFF; $big[1] = 0xFF
    [System.IO.File]::WriteAllBytes("$WorkDir\sig-gtn.bin", $big)
    Expect "s > n" "INVALID" "$WorkDir\sig-gtn.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    [System.IO.File]::WriteAllBytes("$WorkDir\sig-short.bin", $sig[0..254])
    Expect "truncated signature (255 bytes)" "INVALID" "$WorkDir\sig-short.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    [System.IO.File]::WriteAllBytes("$WorkDir\sig-long.bin", ($sig + [byte[]]@(0)))
    Expect "oversized signature (257 bytes)" "INVALID" "$WorkDir\sig-long.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    [System.IO.File]::WriteAllBytes("$WorkDir\sig-empty.bin", [byte[]]::new(0))
    Expect "empty signature" "INVALID" "$WorkDir\sig-empty.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    # A SHA-1 signature must not pass a SHA-256 verifier: the DigestInfo prefix
    # differs, and construct-and-compare is what catches that.
    & $openssl dgst -sha1 -sign k1.pem -out sig-sha1.bin UPDATE.INF
    Expect "SHA-1 signature rejected" "INVALID" "$WorkDir\sig-sha1.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    # PSS padding must not pass a v1.5 verifier either.
    & $openssl dgst -sha256 -sigopt rsa_padding_mode:pss -sign k1.pem -out sig-pss.bin UPDATE.INF
    Expect "PSS signature rejected" "INVALID" "$WorkDir\sig-pss.bin" "$WorkDir\UPDATE.INF" "$WorkDir\mod1.bin"

    # ---- repeat with several fresh keys -----------------------------------
    # One key pair passing proves little: n0inv, the R^2 loop and the final
    # conditional subtraction are all data-dependent.
    Write-Host ""
    Write-Host "round-trip over 8 freshly generated keys"
    $roundOk = $true
    for ($i = 0; $i -lt 8; $i++) {
        & $openssl genrsa -out "r$i.pem" 2048 2>$null
        & $openssl rsa -in "r$i.pem" -pubout -out "rp$i.pem" 2>$null
        [System.IO.File]::WriteAllBytes("$WorkDir\rmod$i.bin", (Get-ModulusBytes -OpenSslExe $openssl -PubPem "$WorkDir\rp$i.pem"))
        & $openssl dgst -sha256 -sign "r$i.pem" -out "rsig$i.bin" UPDATE.INF
        $out = & "$WorkDir\crypttest.exe" verify "$WorkDir\rmod$i.bin" "$WorkDir\rsig$i.bin" "$WorkDir\UPDATE.INF" 2>&1
        if ((($out | Select-Object -Last 1).ToString().Trim()) -ne "VALID") { $roundOk = $false }
    }
    if ($roundOk) {
        Write-Host ("  {0,-46} ok" -f "all 8 verified"); $script:pass++
    } else {
        Write-Host ("  {0,-46} FAIL" -f "all 8 verified") -ForegroundColor Red; $script:fail++
    }
}
finally {
    Pop-Location
}

Write-Host ""
if ($script:fail -eq 0) {
    Write-Host "crypto tests passed ($($script:pass) checks)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "CRYPTO TESTS FAILED ($($script:fail) of $($script:pass + $script:fail))" -ForegroundColor Red
    exit 1
}
