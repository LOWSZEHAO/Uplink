# Junction-links Plugin\Uplink into a UE project's Plugins folder.
# A junction means there is only ever ONE copy of the plugin source - edits in
# the repo are immediately visible to every linked project (no copy drift).
# PowerShell 5.1 compatible; junctions need no admin rights.
# Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectDir,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$PluginSrc = Join-Path $RepoRoot "Plugin\Uplink"

if (-not (Test-Path (Join-Path $ProjectDir "*.uproject"))) {
    Write-Error "No .uproject found in: $ProjectDir"
}

$PluginsDir = Join-Path $ProjectDir "Plugins"
$LinkPath = Join-Path $PluginsDir "Uplink"

if ($Remove) {
    if (Test-Path $LinkPath) {
        # Junctions must be removed with rmdir semantics so the target survives.
        [System.IO.Directory]::Delete($LinkPath, $false)
        Write-Host "Removed junction: $LinkPath"
    } else {
        Write-Host "Nothing to remove at: $LinkPath"
    }
    exit 0
}

if (Test-Path $LinkPath) {
    Write-Error "Already exists: $LinkPath (use -Remove first if it is stale)"
}

New-Item -ItemType Directory -Force $PluginsDir | Out-Null
New-Item -ItemType Junction -Path $LinkPath -Target $PluginSrc | Out-Null
Write-Host "Linked $PluginSrc -> $LinkPath"
Write-Host "Open the project and let the editor compile the plugin, or run scripts\build_all.ps1."
