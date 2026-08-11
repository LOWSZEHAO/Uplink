# Uplink dual-engine build gate: compiles the plugin standalone against every
# engine listed below (no host project needed - uses UAT BuildPlugin).
# Run before committing any C++ change. PowerShell 5.1 compatible.
# Copyright (c) 2026 Low Sze Hao. MIT License.

param(
    [string[]]$Engines = @(
        "C:\Program Files\Epic Games\UE_5.7",
        "C:\Program Files\Epic Games\UE_5.8"
    ),
    [string]$OutputRoot = "$env:TEMP\UplinkBuild"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Uplugin = Join-Path $RepoRoot "Plugin\Uplink\Uplink.uplugin"

if (-not (Test-Path $Uplugin)) {
    Write-Error "Plugin descriptor not found: $Uplugin"
}

$Failed = @()
foreach ($Engine in $Engines) {
    $Uat = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
    $Name = Split-Path $Engine -Leaf
    if (-not (Test-Path $Uat)) {
        Write-Warning "Skipping $Name (RunUAT not found at $Uat)"
        continue
    }

    $Package = Join-Path $OutputRoot $Name
    Write-Host ""
    Write-Host "=== Building Uplink against $Name ===" -ForegroundColor Cyan
    & $Uat BuildPlugin -Plugin="$Uplugin" -Package="$Package" -TargetPlatforms=Win64 -Rocket
    if ($LASTEXITCODE -ne 0) {
        Write-Host "=== $Name FAILED (exit $LASTEXITCODE) ===" -ForegroundColor Red
        $Failed += $Name
    } else {
        Write-Host "=== $Name OK ===" -ForegroundColor Green
    }
}

Write-Host ""
if ($Failed.Count -gt 0) {
    Write-Host ("BUILD GATE FAILED: " + ($Failed -join ", ")) -ForegroundColor Red
    exit 1
}
Write-Host "BUILD GATE PASSED: all engines compiled." -ForegroundColor Green
