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
    [string] $Directory = "scenarios",
    [string] $Endpoint = "http://127.0.0.1:3777",
    [int]    $TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
$scenarioDir = Join-Path (Split-Path -Parent $PSScriptRoot) $Directory

if (-not (Test-Path $scenarioDir)) {
    Write-Host "no scenario directory at $scenarioDir" -ForegroundColor Red
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

# Start from a known state. A session left running by whoever used the editor
# last makes every scenario that calls pie_start fail on its first step, with a
# refusal that describes the session rather than the test - and the run then
# looks like a regression in whatever those scenarios cover. Scenarios take
# care of their own cleanup through teardown; this is the other end of that.
try {
    $pie = Invoke-RestMethod -Uri "$Endpoint/tool/pie_status" -Method Post -Body "{}" -TimeoutSec 10
    if ($pie.data.state -ne "none") {
        Write-Host "a PIE session was already running ($($pie.data.state)) - stopping it first`n" -ForegroundColor Yellow
        Invoke-RestMethod -Uri "$Endpoint/tool/pie_stop" -Method Post -Body '{"wait_ms":40000}' -TimeoutSec 60 | Out-Null
    }
} catch {
    Write-Host "could not check PIE state: $($_.Exception.Message)" -ForegroundColor DarkYellow
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
    # Everything else a scenario declares has to be forwarded: a phase this
    # script quietly drops does not fail, it just never runs, and a teardown
    # that never runs leaves the next scenario a dirty world to start in.
    $request = @{ steps = $doc.steps }
    foreach ($key in @("setup", "teardown", "artifacts", "budget_seconds", "stop_on_failure")) {
        if ($null -ne $doc.$key) { $request.$key = $doc.$key }
    }
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

    function Show-Phase($label, $rows) {
        if (-not $rows) { return }
        if ($label) { Write-Host "  -- $label --" -ForegroundColor DarkGray }
        foreach ($step in $rows) {
            $ok = [bool] $step.success
            if ($ok) { $mark = "PASS"; $colour = "Green" } else { $mark = "FAIL"; $colour = "Red" }
            $secs = ""
            if ($null -ne $step.seconds) { $secs = " ({0:N2}s)" -f $step.seconds }
            Write-Host ("  [{0}] {1}{2}" -f $mark, $step.tool, $secs) -ForegroundColor $colour
            $why = if ($step.fail_reason) { $step.fail_reason } else { $step.message }
            if (-not $ok -and $why) {
                Write-Host ("         {0}" -f $why) -ForegroundColor DarkYellow
            }
        }
    }

    Show-Phase "setup" $result.data.setup
    Show-Phase $null   $result.data.steps
    Show-Phase "teardown" $result.data.teardown

    # Count from a real array. @($null).Count is 1 in PowerShell, not 0, so
    # reading .steps off an absent .data used to report one step - and a
    # scenario refused before any step ran therefore read as a step that had
    # run and behaved. That is how a file whose whole job is proving refusals
    # sat green while executing nothing.
    $stepRows = @()
    if ($null -ne $result.data -and $null -ne $result.data.steps) {
        $stepRows = @($result.data.steps)
    }
    $stepsPassed = @($stepRows | Where-Object { $_.success }).Count
    $stepsTotal  = $stepRows.Count

    # A scenario naming a tool that does not exist is refused as a whole,
    # because there is no invocation to build - so it asserts on the call
    # rather than on any step, and says so with _expect_scenario_refused.
    if ($doc._expect_scenario_refused) {
        if ($result.success -ne $true -and $stepsTotal -eq 0) {
            Write-Host ("  => PASSED (the call was refused: {0})" -f $result.message) -ForegroundColor Green
            $passed++
        } else {
            Write-Host "  => FAILED (the call was expected to be refused outright)" -ForegroundColor Red
            $failed++; $failedNames += $name
        }
        Write-Host ""
        continue
    }

    if ($result.data.passed -eq $true) {
        Write-Host ("  => PASSED ({0}/{1} steps)" -f $stepsPassed, $stepsTotal) -ForegroundColor Green
        $passed++
    } else {
        $note = "{0}/{1} steps" -f $stepsPassed, $stepsTotal
        if ($stepsTotal -eq 0) { $note = "no steps ran: $($result.message)" }
        Write-Host ("  => FAILED ({0})" -f $note) -ForegroundColor Red
        $failed++; $failedNames += $name
    }
    Write-Host ""
}

Write-Host ("=" * 64)
if ($failed) { $summaryColour = "Red" } else { $summaryColour = "Green" }
Write-Host ("{0} scenario(s) passed, {1} failed" -f $passed, $failed) -ForegroundColor $summaryColour
if ($failed) { $failedNames | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red } }
exit ([int]($failed -gt 0))
