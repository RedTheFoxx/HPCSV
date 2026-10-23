param(
    [string]$Zig = "zig",
    [string]$Configuration = "ReleaseFast"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & $Zig build "-Doptimize=$Configuration"
    if ($LASTEXITCODE -ne 0) { throw "zig build failed" }
    Write-Host ""
    Write-Host "Built:"
    Get-ChildItem "zig-out\bin" | Select-Object Name, Length | Format-Table
    Write-Host "Add $root\zig-out\bin to PATH to use the Zed extension."
} finally {
    Pop-Location
}
