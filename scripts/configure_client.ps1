# Point an MCP client at a running Uplink server.
#
#   .\scripts\configure_client.ps1 -Client claude
#   .\scripts\configure_client.ps1 -All -ProjectDir "C:\Path\To\YourProject"
#   .\scripts\configure_client.ps1 -All -Remove
#
# Every one of these clients reads a per-project config file, so the files are
# written into the PROJECT you are driving, not into your home directory. Run it
# from anywhere; pass -ProjectDir to say which project, or let it default to the
# current directory.
#
# Existing entries are preserved. The file is read, the 'uplink' server is added
# or replaced, and everything else is written back untouched - these files
# usually hold other servers, and clobbering them to save a few lines would be a
# poor trade.
# Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

[CmdletBinding()]
param(
    [ValidateSet("claude", "cursor", "vscode", "gemini", "codex")]
    [string[]] $Client,
    [switch]   $All,
    [string]   $ProjectDir = (Get-Location).Path,
    [int]      $Port = 3777,
    [string]   $Name = "uplink",
    [switch]   $Remove
)

$ErrorActionPreference = "Stop"

# Shapes taken from each client's own documentation. They agree on almost
# nothing: the section is 'mcpServers' for three of them, 'servers' for VS Code
# and 'mcp_servers' for Codex; the URL key is 'url' except for Gemini, which
# wants 'httpUrl'; and Claude Code and VS Code additionally want the transport
# named. Codex is TOML rather than JSON.
$Targets = @{
    claude = @{ File = ".mcp.json";             Section = "mcpServers"; UrlKey = "url";     Type = "http" }
    cursor = @{ File = ".cursor/mcp.json";      Section = "mcpServers"; UrlKey = "url";     Type = $null  }
    vscode = @{ File = ".vscode/mcp.json";      Section = "servers";    UrlKey = "url";     Type = "http" }
    gemini = @{ File = ".gemini/settings.json"; Section = "mcpServers"; UrlKey = "httpUrl"; Type = $null  }
    codex  = @{ File = ".codex/config.toml";    Section = "mcp_servers"; UrlKey = "url";    Type = $null; Toml = $true }
}

if ($All) { $Client = $Targets.Keys | Sort-Object }
if (-not $Client) {
    Write-Host "Name a client with -Client, or -All for every one of them." -ForegroundColor Yellow
    Write-Host "  clients: $($Targets.Keys | Sort-Object)"
    exit 1
}

if (-not (Test-Path $ProjectDir)) { Write-Host "no such directory: $ProjectDir" -ForegroundColor Red; exit 1 }
$ProjectDir = (Resolve-Path $ProjectDir).Path
$Url = "http://127.0.0.1:$Port/mcp"

Write-Host ""
Write-Host "project : $ProjectDir"
Write-Host "server  : $Url"
Write-Host ""

function Get-Prop($Object, $Key) {
    if ($null -eq $Object) { return $null }
    $Found = $Object.PSObject.Properties | Where-Object { $_.Name -eq $Key }
    if ($Found) { return $Found.Value }
    return $null
}

function Set-Prop($Object, $Key, $Value) {
    if ($Object.PSObject.Properties.Name -contains $Key) { $Object.$Key = $Value }
    else { $Object | Add-Member -NotePropertyName $Key -NotePropertyValue $Value }
}

function Write-JsonClient($Path, $Spec) {
    # PSCustomObject rather than a hashtable throughout, because
    # ConvertFrom-Json -AsHashtable does not exist in Windows PowerShell 5.1 and
    # that is what most people have.
    $Root = $null
    if (Test-Path $Path) {
        $Raw = Get-Content $Path -Raw
        if ($Raw -and $Raw.Trim()) {
            # A malformed file is left alone rather than replaced - more likely
            # a config someone is midway through editing than one that wants
            # overwriting.
            try { $Root = $Raw | ConvertFrom-Json }
            catch { Write-Host "  SKIPPED - $Path is not valid JSON, leaving it alone" -ForegroundColor Yellow; return $false }
        }
    }
    if ($null -eq $Root) { $Root = [PSCustomObject]@{} }

    $Servers = Get-Prop $Root $Spec.Section
    if ($null -eq $Servers) { $Servers = [PSCustomObject]@{}; Set-Prop $Root $Spec.Section $Servers }

    if ($Remove) {
        if ($Servers.PSObject.Properties.Name -notcontains $Name) {
            Write-Host "  not present" -ForegroundColor DarkGray; return $false
        }
        $Servers.PSObject.Properties.Remove($Name)
    }
    else {
        $Entry = [PSCustomObject]@{}
        if ($Spec.Type) { Set-Prop $Entry "type" $Spec.Type }
        Set-Prop $Entry $Spec.UrlKey $Url
        Set-Prop $Servers $Name $Entry
    }

    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    ($Root | ConvertTo-Json -Depth 12) | Set-Content $Path -Encoding utf8
    return $true
}

function Write-TomlClient($Path, $Spec) {
    # Codex is the only TOML one. Rather than parse TOML, the existing file is
    # kept verbatim and only this server's own table is replaced - a table runs
    # from its header to the next header or the end of the file.
    $Header = "[$($Spec.Section).$Name]"
    $Lines = @()
    if (Test-Path $Path) { $Lines = @(Get-Content $Path) }

    $Kept = New-Object System.Collections.Generic.List[string]
    $InOurTable = $false
    foreach ($Line in $Lines) {
        if ($Line.Trim() -eq $Header) { $InOurTable = $true; continue }
        if ($InOurTable -and $Line.Trim().StartsWith("[")) { $InOurTable = $false }
        if (-not $InOurTable) { $Kept.Add($Line) }
    }

    if (-not $Remove) {
        while ($Kept.Count -gt 0 -and -not $Kept[$Kept.Count - 1].Trim()) { $Kept.RemoveAt($Kept.Count - 1) }
        if ($Kept.Count -gt 0) { $Kept.Add("") }
        $Kept.Add($Header)
        $Kept.Add("$($Spec.UrlKey) = `"$Url`"")
    }
    elseif ($Lines.Count -eq $Kept.Count) {
        Write-Host "  not present" -ForegroundColor DarkGray; return $false
    }

    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    ($Kept -join "`n") + "`n" | Set-Content $Path -Encoding utf8 -NoNewline
    return $true
}

$Written = 0
foreach ($Key in $Client) {
    $Spec = $Targets[$Key]
    $Path = Join-Path $ProjectDir $Spec.File
    Write-Host ("{0,-8} {1}" -f $Key, $Spec.File)

    $Ok = if ($Spec.Toml) { Write-TomlClient $Path $Spec } else { Write-JsonClient $Path $Spec }
    if ($Ok) {
        Write-Host ("  {0} {1}" -f $(if ($Remove) { "removed from" } else { "wrote" }), $Path) -ForegroundColor Green
        $Written++
    }
}

Write-Host ""
if ($Written -eq 0) { Write-Host "nothing changed." -ForegroundColor Yellow; exit 0 }

Write-Host "$Written file(s) updated." -ForegroundColor Green
if (-not $Remove) {
    Write-Host ""
    Write-Host "The server only answers while the editor is open. Restart the client so it"
    Write-Host "re-reads its config, then ask it for the 'status' tool."
}
