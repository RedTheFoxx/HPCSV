# Generates compile_commands.json for clangd.
#
# Zed's C support runs clangd. Without a compilation database clangd cannot
# find the project headers nor the C library (there is no MSVC/Windows SDK on
# this machine; Zig ships the MinGW headers). This script asks the Zig
# installation for its include directories and writes a clangd database.
#
# Usage:
#   .\scripts\gen_compile_commands.ps1
#   .\scripts\gen_compile_commands.ps1 -Zig C:\tools\zig\zig.exe
param(
    [string]$Zig = "zig",
    [string]$Target = "x86_64-w64-windows-gnu"
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$zigPath = $null
if (Test-Path $Zig -PathType Leaf) {
    $zigPath = (Resolve-Path $Zig).Path
} else {
    $cmd = Get-Command $Zig -ErrorAction SilentlyContinue
    if ($cmd) { $zigPath = $cmd.Source }
}
if (-not $zigPath) {
    throw "Zig not found. Pass its path with -Zig, or add it to PATH."
}

$zigRoot = Split-Path $zigPath -Parent
$sysIncludes = @(
    (Join-Path $zigRoot "lib\include"),
    (Join-Path $zigRoot "lib\libc\include\any-windows-any")
)
foreach ($dir in $sysIncludes) {
    if (-not (Test-Path $dir)) { throw "Missing Zig include directory: $dir" }
}

$common = @(
    "clang",
    "--target=$Target",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wformat=2"
)
foreach ($dir in $sysIncludes) {
    $common += "-isystem"
    $common += $dir
}

$files = @(
    @{ file = "core/src/hpcs.c";       inc = @("-Icore/include") },
    @{ file = "core/tests/test_core.c"; inc = @("-Icore/include") },
    @{ file = "cli/src/main.c";        inc = @("-Icore/include") },
    @{ file = "lsp/src/main.c";        inc = @("-Icore/include") },
    @{ file = "app/src/main.c";        inc = @("-Icore/include", "-Iapp") },
    @{ file = "app/src/render.c";      inc = @("-Icore/include", "-Iapp") }
)

$entries = @()
foreach ($f in $files) {
    $defs = @("-D_WIN32_WINNT=0x0a00")
    $args = @($common) + $f.inc + $defs + @("-c", $f.file)
    $entries += [ordered]@{
        directory = $root
        file      = (Join-Path $root $f.file)
        arguments = $args
    }
}

$json = $entries | ConvertTo-Json -Depth 6
$out = Join-Path $root "compile_commands.json"
[IO.File]::WriteAllText($out, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote $out"
Write-Host "Zig headers: $($sysIncludes -join ', ')"
