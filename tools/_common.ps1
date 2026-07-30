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

# --- Stored key passphrase -------------------------------------------------
#
# The signing key stays passphrase-protected. To avoid retyping it on every
# release, the passphrase may be stored beside the key as <key>.pass, encrypted
# with DPAPI (ConvertFrom-SecureString without -Key). That blob is decryptable
# only by the Windows account that wrote it, on the machine that wrote it, so a
# copy, backup or cloud sync of the file is worthless to anyone else.
#
# Deliberately NOT supported: a plaintext passphrase file. That would be exactly
# as good as stripping the passphrase from the key, while looking safer.

# The conventional location of the stored passphrase for a given key.
function Get-PassFilePath {
    param([Parameter(Mandatory)][string]$PrivateKey, [string]$Explicit)

    if ($Explicit) { return $Explicit }
    $dir  = Split-Path -Parent $PrivateKey
    $base = [System.IO.Path]::GetFileNameWithoutExtension($PrivateKey)
    return (Join-Path $dir "$base.pass")
}

# Decrypt a stored passphrase. Returns $null when there is no such file, so
# callers fall back to letting OpenSSL prompt.
function Read-StoredPassphrase {
    param([Parameter(Mandatory)][string]$PassFile)

    if (-not (Test-Path $PassFile)) { return $null }
    try {
        $sec = ConvertTo-SecureString (Get-Content -Raw $PassFile).Trim()
    } catch {
        throw @"
$PassFile could not be decrypted.

DPAPI blobs are bound to the Windows account and machine that created them, so
this usually means the file was written by another account, copied from another
machine, or corrupted. Recreate it:
  pwsh tools/store-keypass.ps1 -PrivateKey <key.pem>
"@
    }
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try   { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# Run an OpenSSL command that needs the private key, supplying a stored
# passphrase when one exists. The passphrase goes through an environment
# variable rather than a temporary file, so it never touches the disk in the
# clear; the variable is removed again even if OpenSSL fails.
function Invoke-OpenSslWithKey {
    param(
        [Parameter(Mandatory)][string]$OpenSslExe,
        [Parameter(Mandatory)][string]$PrivateKey,
        [string]$PassFile,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $pf   = Get-PassFilePath -PrivateKey $PrivateKey -Explicit $PassFile
    $pass = if (Test-Path $pf) { Read-StoredPassphrase $pf } else { $null }

    if ($pass) {
        # -passin has to sit among the OPTIONS, not after the input file:
        # "openssl dgst [options] file" would otherwise read "env:..." as a
        # second file and fail with "Can only sign or verify one file".
        $argv = @($Arguments[0], '-passin', 'env:FTP4DOS_KEYPASS') + $Arguments[1..($Arguments.Count - 1)]
        $env:FTP4DOS_KEYPASS = $pass
        try     { & $OpenSslExe @argv }
        finally { Remove-Item Env:\FTP4DOS_KEYPASS -ErrorAction SilentlyContinue }
    } else {
        & $OpenSslExe @Arguments        # OpenSSL prompts, as it always did
    }
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
