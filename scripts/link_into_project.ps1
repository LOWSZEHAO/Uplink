# Links Plugin\Uplink into a UE project's Plugins folder.
#
# The source directories are junctions, so there is only ever ONE copy of the
# plugin source and an edit in the repo is immediately visible to every linked
# project. Binaries and Intermediate are deliberately NOT shared: they are real
# per-project folders, because build artifacts belong to the engine version that
# produced them. Linking one clone into a 5.7 project and a 5.8 project used to
# mean whichever you built last won, and the other opened with "modules are
# missing or built with a different engine version".
#
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

# Directories shared from the repo. Anything not listed here (Binaries,
# Intermediate) stays local to the project.
$SharedDirs = @("Source", "Config", "Content", "Resources")

function Test-IsReparsePoint([string] $Path) {
    if (-not (Test-Path $Path)) { return $false }
    $item = Get-Item $Path -Force
    return [bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
}

function Remove-Junction([string] $Path) {
    # Directory.Delete on a junction removes the link and leaves the target
    # alone. Remove-Item -Recurse can follow a junction and delete what it
    # points at, which here would be the repo's own source.
    [System.IO.Directory]::Delete($Path, $false)
}

if ($Remove) {
    if (-not (Test-Path $LinkPath)) {
        Write-Host "Nothing to remove at: $LinkPath"
        exit 0
    }

    if (Test-IsReparsePoint $LinkPath) {
        # The old layout: the whole plugin folder was one junction.
        Remove-Junction $LinkPath
        Write-Host "Removed junction: $LinkPath"
        exit 0
    }

    # Current layout: unlink each shared directory before deleting anything, so
    # nothing recursive is ever pointed at the repo.
    foreach ($name in $SharedDirs) {
        $sub = Join-Path $LinkPath $name
        if (Test-IsReparsePoint $sub) {
            Remove-Junction $sub
            Write-Host "  unlinked $name"
        }
    }
    Remove-Item -Recurse -Force $LinkPath
    Write-Host "Removed: $LinkPath (local Binaries/Intermediate deleted with it)"
    exit 0
}

if (Test-Path $LinkPath) {
    if (Test-IsReparsePoint $LinkPath) {
        Write-Error "$LinkPath is a whole-folder junction from an older version of this script. Run with -Remove first, then link again to get per-project build artifacts."
    }
    Write-Error "Already exists: $LinkPath (use -Remove first if it is stale)"
}

New-Item -ItemType Directory -Force $LinkPath | Out-Null

foreach ($name in $SharedDirs) {
    $target = Join-Path $PluginSrc $name
    if (-not (Test-Path $target)) { continue }
    New-Item -ItemType Junction -Path (Join-Path $LinkPath $name) -Target $target | Out-Null
    Write-Host "  linked $name"
}

# The descriptor is a single file, and junctions only work on directories. A
# hard link keeps it genuinely the same file so a version bump in the repo is
# picked up without relinking; that needs both paths on one volume, so fall
# back to a copy and say so rather than failing.
$UpluginSrc = Join-Path $PluginSrc "Uplink.uplugin"
$UpluginDst = Join-Path $LinkPath "Uplink.uplugin"
$sameVolume = [System.IO.Path]::GetPathRoot((Resolve-Path $PluginSrc).Path) -eq
              [System.IO.Path]::GetPathRoot((Resolve-Path $LinkPath).Path)

if ($sameVolume) {
    New-Item -ItemType HardLink -Path $UpluginDst -Target $UpluginSrc | Out-Null
    Write-Host "  linked Uplink.uplugin"
} else {
    Copy-Item $UpluginSrc $UpluginDst
    Write-Host "  copied Uplink.uplugin (different volume - re-run this script after a version bump)"
}

Write-Host ""
Write-Host "Linked $PluginSrc -> $LinkPath"
Write-Host "Binaries and Intermediate are local to this project, so several projects and engine versions can share this clone."
Write-Host "Open the project and let the editor compile the plugin, or run scripts\build_all.ps1."
