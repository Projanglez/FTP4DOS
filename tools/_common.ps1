# Shared helpers for the FTP4DOS tooling scripts. Dot-source this.

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

# Puts the Open Watcom tools on PATH for the current process. The native
# (wcl386 -bt=nt) build is only used for host-side tests; the shipped DOS binary
# is always built by MAKEFILE via wmake.
function Enable-Watcom {
    param([string]$Root = "C:\WATCOM")

    if (-not (Test-Path $Root)) { throw "Open Watcom not found at '$Root'" }
    $env:WATCOM = $Root
    $bin = if (Test-Path "$Root\binnt64") { "$Root\binnt64" } else { "$Root\binnt" }
    $env:PATH = "$bin;$Root\binnt;$env:PATH"
    $env:INCLUDE = "$Root\h;$Root\h\nt"
}

# Reads an RSA modulus out of a PEM public key and returns it as 256 raw
# big-endian bytes - the exact layout rsaverify.cpp expects.
function Get-ModulusBytes {
    param([string]$OpenSslExe, [string]$PubPem)

    $line = (& $OpenSslExe rsa -pubin -in $PubPem -modulus -noout 2>&1) -join ""
    if ($LASTEXITCODE -ne 0) { throw "openssl -modulus failed: $line" }
    $hex = ($line -replace '^Modulus=', '').Trim()
    if ($hex.Length -ne 512) { throw "modulus is $($hex.Length) hex chars, expected 512" }

    $bytes = [byte[]]::new(256)
    for ($i = 0; $i -lt 256; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return $bytes
}
