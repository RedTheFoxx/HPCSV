# Prepares an ASCII copy of the Zed extension.
#
# Zed always builds a dev extension with `--target-dir <extension_dir>/target`.
# On Windows, the MinGW linker cannot open paths containing non-ASCII
# characters (for example "Dépots"), so a dev extension living in such a path
# fails to compile with "ld: cannot find ...".
#
# This script mirrors the extension sources to an ASCII destination. Install
# the dev extension from that destination.
#
# Usage:
#   .\scripts\prepare-dev-extension.ps1
#   .\scripts\prepare-dev-extension.ps1 -Dest C:\hpcs-extension -Build
param(
    [string]$Dest = "C:\hpcs-extension",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$src = Join-Path $root "extension"

if (-not (Test-Path $src)) { throw "Missing extension directory: $src" }

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# Copy only the sources; keep `target` (cargo cache) and `grammars`
# (downloaded by Zed) in place to avoid useless rebuilds/downloads.
foreach ($name in @("Cargo.toml", "Cargo.lock", "extension.toml", "README.md", "src", "languages")) {
    $from = Join-Path $src $name
    if (-not (Test-Path $from)) { continue }
    $to = Join-Path $Dest $name
    if (Test-Path $to) { Remove-Item $to -Recurse -Force }
    Copy-Item $from $to -Recurse -Force
}

Write-Host "Extension copied to: $Dest"
Write-Host ""
Write-Host "In Zed: zed: extensions -> Install Dev Extension -> $Dest"
Write-Host "(re-run this script after changing the extension sources)"

if ($Build) {
    Push-Location $Dest
    try {
        cargo build --target wasm32-wasip2 --release
    } finally {
        Pop-Location
    }
}
