<#
.SYNOPSIS
    Run every gate this project has, in order, and report one verdict.

.DESCRIPTION
    Three stages, cheapest first:

      1. check_repo.ps1     seconds, needs nothing
      2. build_all.ps1      minutes, needs the engines installed
      3. run_scenarios.ps1  minutes, needs an editor running with the plugin

    Only stage 1 can run on a cloud machine, which is why .github/workflows
    runs that one on every push and this script exists for the rest.

    A stage that was skipped is reported as skipped and never counted as a
    pass. That distinction is the whole point of the summary: the failure mode
    worth designing against is a gate reporting green because it did not run.

.PARAMETER SkipBuild
    Do not compile. Use when the change touches no C++.

.PARAMETER SkipScenarios
    Do not run the scenario suite. Use when no editor is open.

.PARAMETER Engines
    Engine install roots to build against. Defaults to build_all.ps1's own list.

.PARAMETER Endpoint
    Where the editor is listening. Defaults to run_scenarios.ps1's own default.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipScenarios,
    [string[]]$Engines,
    [string]$Endpoint
)

$ErrorActionPreference = "Stop"
$Stages = @()

function Invoke-Stage {
    param([string]$Name, [string]$Script, [hashtable]$Arguments, [bool]$Skip, [string]$SkipReason)

    Write-Host ""
    Write-Host "################################################################"
    if ($Skip) {
        Write-Host ("SKIPPED  " + $Name + " - " + $SkipReason) -ForegroundColor Yellow
        $script:Stages += [pscustomobject]@{ Name = $Name; Result = "SKIPPED" }
        return
    }
    Write-Host ("RUNNING  " + $Name) -ForegroundColor Cyan
    Write-Host "################################################################"

    # Clear it first. A stage script that returns without calling exit leaves
    # $LASTEXITCODE holding whatever the last native command set, so a stale
    # failure from something unrelated reads as this stage failing. Every stage
    # script here exits explicitly; this is the belt to that pair of braces.
    $global:LASTEXITCODE = 0
    & (Join-Path $PSScriptRoot $Script) @Arguments
    $code = $LASTEXITCODE

    $result = "PASSED"
    if ($code -ne 0) { $result = "FAILED" }
    $script:Stages += [pscustomobject]@{ Name = $Name; Result = $result }
}

$BuildArgs = @{}
if ($Engines) { $BuildArgs["Engines"] = $Engines }

$ScenarioArgs = @{}
if ($Endpoint) { $ScenarioArgs["Endpoint"] = $Endpoint }

Invoke-Stage "Repository checks" "check_repo.ps1"    @{}           $false        ""
Invoke-Stage "Dual-engine build" "build_all.ps1"     $BuildArgs    $SkipBuild    "-SkipBuild was passed"
Invoke-Stage "Scenario suite"    "run_scenarios.ps1" $ScenarioArgs $SkipScenarios "-SkipScenarios was passed"

Write-Host ""
Write-Host "################################################################"
foreach ($s in $Stages) {
    $colour = "Green"
    if ($s.Result -eq "FAILED")  { $colour = "Red" }
    if ($s.Result -eq "SKIPPED") { $colour = "Yellow" }
    Write-Host ("  " + $s.Result.PadRight(8) + $s.Name) -ForegroundColor $colour
}

$FailedStages = @($Stages | Where-Object { $_.Result -eq "FAILED" })
if ($FailedStages.Count -gt 0) {
    Write-Host ""
    Write-Host ("CI FAILED: " + (@($FailedStages | ForEach-Object { $_.Name }) -join ", ")) -ForegroundColor Red
    exit 1
}

$SkippedStages = @($Stages | Where-Object { $_.Result -eq "SKIPPED" })
Write-Host ""
if ($SkippedStages.Count -gt 0) {
    Write-Host ("CI PASSED, but " + $SkippedStages.Count + " stage(s) did not run: " +
        (@($SkippedStages | ForEach-Object { $_.Name }) -join ", ")) -ForegroundColor Yellow
    exit 0
}
Write-Host "CI PASSED: every gate ran and every gate is green." -ForegroundColor Green
