<#
.SYNOPSIS
    Runs every host-side test suite.

.DESCRIPTION
    One entry point for what can be checked without a DOS machine:

      test-crypto.ps1   SHA-256 against the FIPS vectors, RSA verification
                        against real OpenSSL signatures, and every way of
                        tampering with them
      test-publish.ps1  the guards in publish-update.ps1

    What this deliberately does NOT cover, because it needs an actual DOS
    machine, is listed in the summary at the end. The QEMU VM under vm/ covers
    most of it; timing has to come from real 386 hardware.

.EXAMPLE
    pwsh tools/run-tests.ps1
#>
[CmdletBinding()]
param([string]$OpenSsl)

$ErrorActionPreference = 'Continue'
$failed = @()

foreach ($suite in @('test-crypto.ps1', 'test-publish.ps1')) {
    Write-Host ""
    Write-Host ("=" * 64)
    Write-Host $suite
    Write-Host ("=" * 64)
    $args = @('-NoProfile', '-File', (Join-Path $PSScriptRoot $suite))
    if ($OpenSsl) { $args += @('-OpenSsl', $OpenSsl) }
    & pwsh @args
    if ($LASTEXITCODE -ne 0) { $failed += $suite }
}

Write-Host ""
Write-Host ("=" * 64)
if ($failed.Count -eq 0) {
    Write-Host "all host-side suites passed" -ForegroundColor Green
} else {
    Write-Host ("FAILED: " + ($failed -join ', ')) -ForegroundColor Red
}

Write-Host ""
Write-Host "Not covered here (needs a DOS machine):"
Write-Host "  - the 16-bit build of sha256/rsaverify   -> tools/crypttest.cpp, 'selftest'"
Write-Host "  - HTTP against a live server             -> tools/httptest.cpp"
Write-Host "  - the verify-first update chain          -> tools/updtest.cpp"
Write-Host "  - TUI, exit swap, execv restart          -> vm/ (see vm/README.md)"
Write-Host "  - crypto timing                          -> 'crypttest bench' on real 386 hardware"

exit ($failed.Count -eq 0 ? 0 : 1)
