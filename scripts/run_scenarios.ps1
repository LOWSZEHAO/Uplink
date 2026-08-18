# Run every scenario in scenarios/ against a running editor and report.
#
#   .\scripts\run_scenarios.ps1
#   .\scripts\run_scenarios.ps1 -Filter playtest
#
# Requires the editor to be open with the Uplink plugin enabled. Each file is
# posted to run_scenario, which executes the steps in order and returns a
# per-step report.
# Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

[CmdletBinding()]
param(
    [string] $Filter = "",
    [string] $Endpoint = "http://127.0.0.1:3777",
    [int]    $TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
$scenarioDir = Join-Path (Split-Path -Parent $PSScriptRoot) "scenarios"

if (-not (Test-Path $scenarioDir)) {
    Write-Host "no scenarios directory at $scenarioDir" -ForegroundColor Red
    exit 1
}

try {
    $status = Invoke-RestMethod -Uri "$Endpoint/tool/status" -Method Post -Body "{}" -TimeoutSec 10
    Write-Host "editor: $($status.data.engine)  project: $($status.data.project)  map: $($status.data.map)`n"
} catch {
    Write-Host "No editor answering on $Endpoint." -ForegroundColor Red
    Write-Host "Open your project with the Uplink plugin enabled, then run this again."
    exit 1
}

$files = Get-ChildItem -Path $scenarioDir -Filter *.json | Sort-Object Name
if ($Filter) { $files = $files | Where-Object { $_.Name -like "*$Filter*" } }
if (-not $files) { Write-Host "no scenario files matched '$Filter'" -ForegroundColor Yellow; exit 1 }

$passed = 0
$failed = 0
$failedNames = @()

foreach ($file in $files) {
    $doc = Get-Content $file.FullName -Raw | ConvertFrom-Json

    # Keys beginning with _ are notes for the reader, not part of the request.
    $request = @{ steps = $doc.steps }
    if ($null -ne $doc.stop_on_failure) { $request.stop_on_failure = $doc.stop_on_failure }
    $body = $request | ConvertTo-Json -Depth 20

    $name = if ($doc._name) { $doc._name } else { $file.BaseName }
    Write-Host ("=" * 64)
    Write-Host $name -ForegroundColor Cyan
    if ($doc._purpose) { Write-Host $doc._purpose -ForegroundColor DarkGray }

    try {
        $result = Invoke-RestMethod -Uri "$Endpoint/tool/run_scenario" -Method Post `
                                    -Body $body -TimeoutSec $TimeoutSeconds
    } catch {
        Write-Host "  request failed: $($_.Exception.Message)" -ForegroundColor Red
        $failed++; $failedNames += $name; continue
    }

    foreach ($step in $result.data.steps) {
        $ok = [bool] $step.success
        if ($ok) { $mark = "PASS"; $colour = "Green" } else { $mark = "FAIL"; $colour = "Red" }
        $secs = ""
        if ($null -ne $step.seconds) { $secs = " ({0:N2}s)" -f $step.seconds }
        Write-Host ("  [{0}] {1}{2}" -f $mark, $step.tool, $secs) -ForegroundColor $colour
        if (-not $ok -and $step.message) {
            Write-Host ("         {0}" -f $step.message) -ForegroundColor DarkYellow
        }
    }

    # 04 inverts the usual reading: every step there is meant to be refused.
    $inverted    = $file.Name -like "*refuses*"
    $stepsPassed = @($result.data.steps | Where-Object { $_.success }).Count
    $stepsTotal  = @($result.data.steps).Count

    if ($inverted) {
        $scenarioOk = ($stepsPassed -eq 0) -and ($stepsTotal -gt 0)
    } else {
        $scenarioOk = ($result.data.passed -eq $true)
    }

    if ($scenarioOk) {
        if ($inverted) { $note = "all $stepsTotal steps correctly refused" }
        else           { $note = "$stepsPassed/$stepsTotal steps" }
        Write-Host "  => PASSED ($note)" -ForegroundColor Green
        $passed++
    } else {
        if ($inverted) { $note = "$stepsPassed step(s) succeeded that should have been refused" }
        else           { $note = "$stepsPassed/$stepsTotal steps" }
        Write-Host "  => FAILED ($note)" -ForegroundColor Red
        $failed++; $failedNames += $name
    }
    Write-Host ""
}

Write-Host ("=" * 64)
if ($failed) { $summaryColour = "Red" } else { $summaryColour = "Green" }
Write-Host ("{0} scenario(s) passed, {1} failed" -f $passed, $failed) -ForegroundColor $summaryColour
if ($failed) { $failedNames | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red } }
exit ([int]($failed -gt 0))
