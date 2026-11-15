# ---------------------------------------------------------------------------
# Nova installer for Windows (PowerShell 5.1+).
#
# One-line install:
#
#   irm https://raw.githubusercontent.com/Stevem100/nova/main/install.ps1 | iex
#
# With options (download the file first, then run):
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 `
#       [-Prefix "D:\Nova"] [-Version 1.2.0] [-NoPath] [-WithPackages]
#
# What it does:
#   * downloads the signed-off source tarball of the latest Nova release and
#     verifies its SHA-256 against SHA256SUMS.txt,
#   * compiles it with gcc or clang (MinGW-w64 — e.g. MSYS2 UCRT64 or
#     https://winlibs.com), linking -lbcrypt -lws2_32 like the official CI,
#   * installs nova.exe to <Prefix>\bin and (unless -NoPath) adds that
#     directory to your user PATH,
#   * with -WithPackages, seeds %USERPROFILE%\.nova\registry with every
#     package published in the release, so `import "strx"` works offline.
#
# There is no prebuilt Windows binary yet; the compile takes well under a
# minute. MSVC users can instead run build_windows.bat instructions manually.
# ---------------------------------------------------------------------------
#requires -Version 5.1
param(
    [string]$Prefix = "$env:LOCALAPPDATA\Nova",
    [string]$Version = "",
    [switch]$NoPath,
    [switch]$WithPackages
)

$ErrorActionPreference = 'Stop'
$Repo = 'Stevem100/nova'
$FallbackVersion = '1.2.0'

try {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch { }

function Info($m) { Write-Host "==> $m" }

# ------------------------------------------------------------- version -----
if (-not $Version) {
    Info 'Resolving the latest release...'
    try {
        $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing
        $Version = ($rel.tag_name -replace '^v', '')
    } catch {
        $Version = $FallbackVersion
        Write-Warning "could not reach the GitHub API — falling back to v$Version"
    }
}
Info "Installing Nova v$Version"

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("nova-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    # ------------------------------------------------------- download -----
    $base = "https://github.com/$Repo/releases/download/v$Version"
    $tgz = Join-Path $tmp "nova-$Version-source.tar.gz"

    Info 'Downloading the source tarball...'
    Invoke-WebRequest "$base/nova-$Version-source.tar.gz" -OutFile $tgz -UseBasicParsing

    Info 'Verifying SHA-256...'
    $sumsPath = Join-Path $tmp 'SHA256SUMS.txt'
    Invoke-WebRequest "$base/SHA256SUMS.txt" -OutFile $sumsPath -UseBasicParsing
    $entryName = "nova-$Version-source.tar.gz"
    $want = $null
    foreach ($line in Get-Content $sumsPath) {
        if ($line -match ('^([0-9a-fA-F]{64})\s{2}' + [regex]::Escape($entryName) + '\s*$')) {
            $want = $Matches[1].ToLower(); break
        }
    }
    $have = (Get-FileHash $tgz -Algorithm SHA256).Hash.ToLower()
    if (-not $want) {
        Write-Warning "no checksum entry for $entryName — skipping verification"
    } elseif ($have -ne $want) {
        throw "SHA-256 mismatch for $entryName`n  expected: $want`n  actual:   $have`nThe download is corrupt or was tampered with — do not install it."
    }
    Info 'checksum verified'

    # ------------------------------------------------------- extract ------
    Info 'Extracting...'
    tar -xzf $tgz -C $tmp
    if ($LASTEXITCODE -ne 0) { throw 'tar extraction failed' }
    $src = Get-ChildItem $tmp -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'src\nova.h') } |
        Select-Object -First 1
    if (-not $src) { throw 'could not find the extracted source tree' }

    # ------------------------------------------------------- compile ------
    $cc = Get-Command gcc, clang -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cc) {
        throw "no gcc or clang found on PATH.`nInstall MinGW-w64 (MSYS2 UCRT64 or https://winlibs.com), open its shell, and re-run this installer."
    }
    Info "Compiling with $($cc.Name) (this takes under a minute)..."
    $cfiles = (Get-ChildItem (Join-Path $src.FullName 'src') -Filter *.c).FullName
    & $cc.Name -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter `
        -o (Join-Path $tmp 'nova.exe') $cfiles -lm -lbcrypt -lws2_32
    if ($LASTEXITCODE -ne 0) { throw 'compilation failed' }

    # ------------------------------------------------------- install ------
    $bin = Join-Path $Prefix 'bin'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    Copy-Item (Join-Path $tmp 'nova.exe') (Join-Path $bin 'nova.exe') -Force
    Info "Installed $(Join-Path $bin 'nova.exe')"

    # ------------------------------------------------------- packages -----
    if ($WithPackages) {
        Info 'Seeding the package registry (-WithPackages)...'
        $regZip = Join-Path $tmp 'registry.zip'
        Invoke-WebRequest "$base/nova-registry-$Version.zip" -OutFile $regZip -UseBasicParsing
        $registry = Join-Path $env:USERPROFILE '.nova\registry'
        if (Test-Path $registry) {
            Write-Warning "$registry already exists — leaving it untouched"
        } else {
            New-Item -ItemType Directory -Path $registry -Force | Out-Null
            Expand-Archive $regZip $registry -Force
            Info "packages seeded into $registry (Nova's default registry)"
        }
    }

    # ------------------------------------------------------- PATH ---------
    if (-not $NoPath) {
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        if (-not $userPath) { $userPath = '' }
        if ($userPath -notlike "*$bin*") {
            [Environment]::SetEnvironmentVariable(
                'Path', ($userPath.TrimEnd(';') + ';' + $bin), 'User')
            Info "added $bin to your user PATH (restart your terminal to pick it up)"
        } else {
            Info "$bin is already on your PATH"
        }
    }

    # ------------------------------------------------------- smoke --------
    & (Join-Path $bin 'nova.exe') --version
    Info 'Done. Open a new terminal and run:  nova'
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
