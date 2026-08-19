<#
.SYNOPSIS
    Static checks that need no Unreal Engine and no running editor.

.DESCRIPTION
    Everything here reads text files and finishes in seconds, so it can run on
    a plain cloud machine on every push. It exists because the parts of this
    project that can rot silently all rot in the same direction: the code keeps
    working and something written down beside it stops being true.

    A tool whose schema no longer parses is dropped at startup and no client
    can tell it is missing. A scenario step naming a renamed tool takes the
    whole file offline. A scenario step passing a parameter the tool does not
    declare is ignored and the step still passes, testing less than it claims.
    A trait row left behind by a rename un-marks a destructive tool. None of
    these break a build, so a build gate never sees them.

    The engine-dependent half of the gate - build_all.ps1 and
    run_scenarios.ps1 - has to run on a machine with the engine installed.
    ci.ps1 runs all three in order.

.PARAMETER Quiet
    Print only failures and the verdict.
#>
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

$script:FailedChecks = @()
$script:SkippedChecks = @()
$script:WarnCount = 0
$script:CheckCount = 0

function Write-Info {
    param([string]$Text)
    if (-not $Quiet) { Write-Host $Text }
}

function Test-HasProperty {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $false }
    return ($Object.PSObject.Properties.Name -contains $Name)
}

# A check is a scriptblock returning zero or more problem strings. Returning
# nothing is a pass. This shape is deliberate: a check cannot pass by failing
# to run, because a scriptblock that throws takes the whole script down with
# $ErrorActionPreference = Stop rather than quietly reporting green.
function Invoke-Check {
    param([string]$Name, [scriptblock]$Body)

    $script:CheckCount++
    $problems = @(& $Body | Where-Object { $_ })

    if ($problems.Count -eq 0) {
        Write-Info ("  [PASS] " + $Name)
        return
    }
    Write-Host ("  [FAIL] " + $Name) -ForegroundColor Red
    foreach ($p in $problems) { Write-Host ("         " + $p) -ForegroundColor Red }
    $script:FailedChecks += $Name
}

function Write-Warn {
    param([string]$Text)
    $script:WarnCount++
    Write-Host ("  [WARN] " + $Text) -ForegroundColor Yellow
}

# A check that could not run is reported as skipped, never folded into the pass
# count. "It did not run" and "it found nothing" are different answers.
function Write-Skip {
    param([string]$Name, [string]$Why)
    $script:SkippedChecks += $Name
    Write-Host ("  [SKIP] " + $Name + " - " + $Why) -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Enumerate every registered tool by parsing the source.
#
# Two registration forms exist and both are matched. The count self-check
# below is the important part: it compares what the structured parse found
# against a raw count of call sites, so a call formatted differently fails the
# check loudly instead of silently dropping out of every check that follows.
# ---------------------------------------------------------------------------

$SourceRoot   = Join-Path $RepoRoot "Plugin/Uplink/Source"
$ExcludeFiles = @("UplinkToolRegistry.h", "UplinkToolRegistry.cpp", "UplinkToolProvider.h")

$Tools         = @{}
$ToolOrder     = @()
$ParseProblems = @()
$RawQuick      = 0
$RawLatent     = 0
$FoundQuick    = 0
$FoundLatent   = 0

$SourceFiles = Get-ChildItem -Path $SourceRoot -Recurse -File -Include *.cpp, *.h |
    Where-Object { $ExcludeFiles -notcontains $_.Name } |
    Sort-Object FullName

foreach ($File in $SourceFiles) {
    $Rel   = $File.FullName.Substring($RepoRoot.Length + 1)
    $Lines = @(Get-Content -LiteralPath $File.FullName -Encoding UTF8)

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $Line = $Lines[$i]

        $IsQuick = ($Line -match '\.RegisterQuick\s*\(')
        if ($IsQuick) { $RawQuick++ }

        # One local FUplinkToolInfo per latent tool. Counting the Register call
        # instead would over-count: live_compile registers from two branches of
        # an #if WITH_LIVE_CODING and is still one tool. Headers are skipped
        # because FUplinkToolInfo is also a member of the registry's own entry.
        if (($File.Extension -eq ".cpp") -and ($Line -match '^\s*FUplinkToolInfo\s+Info\s*;')) { $RawLatent++ }

        $Name     = $null
        $NameLine = $i

        if ($IsQuick) {
            # Either the name is on this line, or RegisterQuick( ends the line
            # and the name is on the next one. Both forms are accepted so that
            # reformatting a call site cannot make it invisible.
            if ($Line -match '\.RegisterQuick\s*\(\s*TEXT\("([a-z0-9_]+)"\)') {
                $Name = $Matches[1]
            }
            elseif (($Line -match '\.RegisterQuick\s*\(\s*$') -and (($i + 1) -lt $Lines.Count) -and
                    ($Lines[$i + 1] -match '^\s*TEXT\("([a-z0-9_]+)"\)')) {
                $Name     = $Matches[1]
                $NameLine = $i + 1
            }
            if ($Name) { $FoundQuick++ }
            else { $ParseProblems += ($Rel + ":" + ($i + 1) + " RegisterQuick call site whose tool name could not be read") }
        }
        elseif ($Line -match '^\s*Info\.Name\s*=\s*TEXT\("([a-z0-9_]+)"\)') {
            $Name = $Matches[1]
            $FoundLatent++
        }

        if (-not $Name) { continue }

        # The schema is a single-line raw string within a few lines of the name.
        $Schema     = $null
        $SchemaLine = 0
        $Limit = [Math]::Min($NameLine + 16, $Lines.Count - 1)
        for ($j = $NameLine; $j -le $Limit; $j++) {
            if ($Lines[$j] -match 'R"json\((.*)\)json"') {
                $Schema     = $Matches[1]
                $SchemaLine = $j + 1
                break
            }
        }

        $Tools[$Name] = [pscustomobject]@{
            Name       = $Name
            File       = $Rel
            Line       = $NameLine + 1
            SchemaText = $Schema
            SchemaLine = $SchemaLine
            Schema     = $null
        }
        $ToolOrder += $Name
    }
}

$ToolNames = @($ToolOrder | Sort-Object -Unique)

Write-Info ""
Write-Info "================================================================"
Write-Info ("Registry  " + $ToolNames.Count + " tools across " + $SourceFiles.Count + " source files")
Write-Info "================================================================"

Invoke-Check "Every registration call site was parsed" {
    if ($RawQuick -ne $FoundQuick) {
        "found $RawQuick RegisterQuick call sites but could only read a tool name from $FoundQuick of them"
    }
    if ($RawLatent -ne $FoundLatent) {
        "found $RawLatent FUplinkToolInfo declarations but only $FoundLatent Info.Name assignments - a tool is registered that this check cannot see"
    }
    foreach ($p in $ParseProblems) { $p }
}

Invoke-Check "Tool names are unique and conventional" {
    $Seen = @{}
    foreach ($n in $ToolOrder) {
        if ($Seen.ContainsKey($n)) {
            "duplicate registration of '$n' - the registry serves whichever module loads last"
        }
        $Seen[$n] = $true
    }
    foreach ($n in $ToolNames) {
        if ($n -notmatch '^[a-z][a-z0-9_]*$') { "tool name '$n' is not lower_snake_case" }
    }
}

Invoke-Check "Every tool has a schema that parses as JSON" {
    foreach ($n in $ToolNames) {
        $t = $Tools[$n]
        if (-not $t.SchemaText) {
            "$($t.File):$($t.Line) tool '$n' has no schema literal within reach of its registration"
            continue
        }
        try {
            $t.Schema = $t.SchemaText | ConvertFrom-Json
        }
        catch {
            "$($t.File):$($t.SchemaLine) schema for '$n' is not valid JSON - Register refuses it at startup and the tool never appears: $($_.Exception.Message)"
        }
    }
}

Invoke-Check "Schemas hold their structural invariants" {
    foreach ($n in $ToolNames) {
        $s = $Tools[$n].Schema
        if ($null -eq $s) { continue }
        $where = "$($Tools[$n].File):$($Tools[$n].SchemaLine)"

        if ($s.type -ne "object") { "$where schema for '$n' declares type '$($s.type)', not 'object'" }

        if (-not (Test-HasProperty $s "properties")) {
            # Without a properties block the runtime validator returns true for
            # anything, so every parameter check for this tool is off.
            "$where schema for '$n' has no 'properties' - parameter validation is disabled for it entirely"
            continue
        }
        $declared = @($s.properties.PSObject.Properties.Name)

        if (Test-HasProperty $s "required") {
            foreach ($r in @($s.required)) {
                if ($declared -notcontains $r) { "$where '$n' requires '$r' but never declares it - the tool can never be called" }
            }
        }
    }
}

# The world parameter means the same thing on every tool that takes one, so a
# tool declaring it differently is a tool that accepts a different set of
# worlds. An enum of editor|pie is the trap: it refuses the ids the worlds
# tool hands out, so a multi-client playtest fails schema validation on some
# tools and works on others.
Invoke-Check "The world parameter is declared identically everywhere" {
    $Shapes = @{}
    foreach ($n in $ToolNames) {
        if (-not $Tools[$n].SchemaText) { continue }
        if ($Tools[$n].SchemaText -match '("world":\{[^}]*\})') {
            $shape = $Matches[1]
            if (-not $Shapes.ContainsKey($shape)) { $Shapes[$shape] = @() }
            $Shapes[$shape] += $n
        }
    }
    if ($Shapes.Keys.Count -le 1) { return }

    "the world parameter is declared $($Shapes.Keys.Count) different ways, so it does not accept the same worlds on every tool:"
    foreach ($k in ($Shapes.Keys | Sort-Object)) {
        "  $k"
        "      on: " + (@($Shapes[$k] | Sort-Object) -join ", ")
    }
}

# ---------------------------------------------------------------------------
# The trait table, which is matched to tools by name and nothing else.
# ---------------------------------------------------------------------------

Invoke-Check "Trait table rows name registered tools, are unique and sorted" {
    $TraitFile = Join-Path $RepoRoot "Plugin/Uplink/Source/UplinkEditor/Private/UplinkToolTraits.cpp"
    if (-not (Test-Path $TraitFile)) { "trait table not found at $TraitFile"; return }

    $Rows = @()
    $TraitLines = @(Get-Content -LiteralPath $TraitFile -Encoding UTF8)
    for ($i = 0; $i -lt $TraitLines.Count; $i++) {
        if ($TraitLines[$i] -match '^\s*\{\s*TEXT\("([a-z0-9_]+)"\)\s*,\s*Trait_') {
            $Rows += [pscustomobject]@{ Name = $Matches[1]; Line = $i + 1 }
        }
    }
    if ($Rows.Count -eq 0) { "no trait rows parsed - the table format changed and this check has stopped looking at anything"; return }

    $Seen = @{}
    foreach ($r in $Rows) {
        if ($ToolNames -notcontains $r.Name) {
            "UplinkToolTraits.cpp:$($r.Line) marks '$($r.Name)', which is not a registered tool - a rename left the trait behind and the tool now reports no traits at all"
        }
        if ($Seen.ContainsKey($r.Name)) { "UplinkToolTraits.cpp:$($r.Line) duplicate row for '$($r.Name)'" }
        $Seen[$r.Name] = $true
    }
    $Names  = @($Rows | ForEach-Object { $_.Name })
    $Sorted = @($Names | Sort-Object)
    if (($Names -join ",") -ne ($Sorted -join ",")) {
        "trait rows are not in alphabetical order, which the file's own comment relies on for spotting a missing row by eye"
    }
}

# ---------------------------------------------------------------------------
# Scenarios. These are the regression suite, so a scenario that has quietly
# stopped testing what it says it tests is worse than one that fails.
# ---------------------------------------------------------------------------

$ScenarioDir   = Join-Path $RepoRoot "scenarios"
$ScenarioFiles = @(Get-ChildItem -Path $ScenarioDir -Filter *.json -File | Sort-Object Name)
$TransportKeys = @("world", "wait_ms", "timeout_s")

Write-Info ""
Write-Info "================================================================"
Write-Info ("Scenarios  " + $ScenarioFiles.Count + " files")
Write-Info "================================================================"

$script:Scenarios = @()
Invoke-Check "Every scenario file is valid JSON" {
    foreach ($f in $ScenarioFiles) {
        try {
            $json = Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            $script:Scenarios += [pscustomobject]@{ File = $f.Name; Json = $json }
        }
        catch {
            "scenarios/$($f.Name) is not valid JSON: $($_.Exception.Message)"
        }
    }
}

# Steps marked expect_failure are asserting a refusal, so their params are
# meant to be wrong. A scenario marked _expect_scenario_refused is the same
# thing one level up: it names a tool that does not exist on purpose, and the
# refusal of the whole call is the assertion. Everything else is held to the
# rules.
function Get-CheckableSteps {
    param($Entry)
    $j = $Entry.Json
    if ((Test-HasProperty $j "_expect_scenario_refused") -and $j._expect_scenario_refused) { return @() }

    $out = @()
    foreach ($phase in @("setup", "steps", "teardown")) {
        if (-not (Test-HasProperty $j $phase)) { continue }
        $n = 0
        foreach ($step in @($j.$phase)) {
            $n++
            if ((Test-HasProperty $step "expect_failure") -and $step.expect_failure) { continue }
            $out += [pscustomobject]@{ Step = $step; Where = "scenarios/$($Entry.File) $phase[$n]" }
        }
    }
    return $out
}

Invoke-Check "Every scenario step names a registered tool" {
    foreach ($e in $script:Scenarios) {
        foreach ($s in (Get-CheckableSteps $e)) {
            $tool = $s.Step.tool
            if (-not $tool) { "$($s.Where) has no 'tool'"; continue }
            if ($ToolNames -notcontains $tool) {
                "$($s.Where) calls '$tool', which is not registered - the runner refuses the whole scenario before any step runs"
            }
        }
    }
}

Invoke-Check "Every scenario parameter is one the tool declares" {
    foreach ($e in $script:Scenarios) {
        foreach ($s in (Get-CheckableSteps $e)) {
            $tool = $s.Step.tool
            if (-not $tool -or ($ToolNames -notcontains $tool)) { continue }
            $schema = $Tools[$tool].Schema
            if ($null -eq $schema -or -not (Test-HasProperty $schema "properties")) { continue }
            $declared = @($schema.properties.PSObject.Properties.Name)

            $passed = @()
            if (Test-HasProperty $s.Step "params") {
                $passed = @($s.Step.params.PSObject.Properties.Name)
            }
            foreach ($p in $passed) {
                if (($declared -notcontains $p) -and ($TransportKeys -notcontains $p)) {
                    # The scenario runner does not validate parameters - only the
                    # transport path does - so this is dropped in silence and the
                    # step passes while testing something weaker than intended.
                    "$($s.Where) passes '$p' to '$tool', which does not declare it - the runner ignores it and the step still passes"
                }
            }
            if (Test-HasProperty $schema "required") {
                foreach ($r in @($schema.required)) {
                    if ($passed -notcontains $r) { "$($s.Where) omits '$r', which '$tool' requires" }
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Documentation. Nothing generates these files and nothing else verifies them,
# so they are the only hand-copied surface in the repo that can rot without a
# symptom.
# ---------------------------------------------------------------------------

Write-Info ""
Write-Info "================================================================"
Write-Info "Documentation and versions"
Write-Info "================================================================"

function Get-TableIdents {
    param([string]$Path)
    $idents = @()
    foreach ($line in @(Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line -notmatch '^\|') { continue }
        $cell = ($line -split '\|')[1]
        if (-not $cell) { continue }
        foreach ($m in [regex]::Matches($cell, '`([A-Za-z0-9_\-]+)`')) { $idents += $m.Groups[1].Value }
    }
    return $idents
}

Invoke-Check "TOOLS.md documents every registered tool" {
    $Documented = @(Get-TableIdents (Join-Path $RepoRoot "TOOLS.md"))
    foreach ($n in $ToolNames) {
        if ($Documented -notcontains $n) { "'$n' is registered but appears in no TOOLS.md table row" }
    }
    # The reverse direction is deliberately not checked: the same column
    # legitimately documents bp_modify sub-operations and run_scenario keys.
}

Invoke-Check "Tool counts written in the docs are true" {
    $Real = $ToolNames.Count

    $ToolsMd = Get-Content -LiteralPath (Join-Path $RepoRoot "TOOLS.md") -Raw -Encoding UTF8
    if ($ToolsMd -match 'All (\d+) tools') {
        if ([int]$Matches[1] -ne $Real) { "TOOLS.md says 'All $($Matches[1]) tools'; $Real are registered" }
    }
    else { "TOOLS.md no longer states a tool count in the form 'All N tools'" }

    $ReadmeLines = @(Get-Content -LiteralPath (Join-Path $RepoRoot "README.md") -Encoding UTF8)
    $Claims = 0
    for ($i = 0; $i -lt $ReadmeLines.Count; $i++) {
        foreach ($m in [regex]::Matches($ReadmeLines[$i], '(\d+) tools')) {
            $Claims++
            if ([int]$m.Groups[1].Value -ne $Real) { "README.md:$($i + 1) claims $($m.Groups[1].Value) tools; $Real are registered" }
        }
    }
    if ($Claims -eq 0) { "README.md no longer states a tool count" }
}

Invoke-Check "scenarios/README.md lists every scenario file" {
    $Listed = @(Get-TableIdents (Join-Path $ScenarioDir "README.md"))
    $OnDisk = @($ScenarioFiles | ForEach-Object { $_.BaseName })
    foreach ($s in $OnDisk) { if ($Listed -notcontains $s) { "scenarios/$s.json exists but is in no scenarios/README.md row" } }
    foreach ($l in $Listed) { if ($OnDisk -notcontains $l) { "scenarios/README.md lists '$l', which is not a file in scenarios/" } }
}

Invoke-Check "The version is the same number everywhere it is written" {
    $Found = @{}

    $Header = Get-Content -LiteralPath (Join-Path $RepoRoot "Plugin/Uplink/Source/UplinkEditor/Public/UplinkVersion.h") -Raw -Encoding UTF8
    if ($Header -match '#define\s+UPLINK_VERSION\s+TEXT\("([^"]+)"\)') { $Found["UplinkVersion.h"] = $Matches[1] }
    else { "UplinkVersion.h no longer defines UPLINK_VERSION in the expected form" }

    $Uplugin = Get-Content -LiteralPath (Join-Path $RepoRoot "Plugin/Uplink/Uplink.uplugin") -Raw -Encoding UTF8 | ConvertFrom-Json
    $Found["Uplink.uplugin"] = $Uplugin.VersionName

    $Pkg = Get-Content -LiteralPath (Join-Path $RepoRoot "bridge/package.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $Found["bridge/package.json"] = $Pkg.version

    $Readme = Get-Content -LiteralPath (Join-Path $RepoRoot "README.md") -Raw -Encoding UTF8
    if ($Readme -match '\*\*v([0-9]+\.[0-9]+\.[0-9]+)') { $Found["README.md"] = $Matches[1] }
    else { "README.md no longer carries a release line in the form **vN.N.N" }

    $Distinct = @($Found.Values | Sort-Object -Unique)
    if ($Distinct.Count -gt 1) {
        $detail = @($Found.Keys | Sort-Object | ForEach-Object { $_ + "=" + $Found[$_] }) -join ", "
        "the plugin reports its version to every client, so these must agree: $detail"
    }

    # The changelog legitimately runs ahead while a release is accumulating,
    # so a mismatch there is worth saying out loud but is not a failure.
    foreach ($line in @(Get-Content -LiteralPath (Join-Path $RepoRoot "CHANGELOG.md") -Encoding UTF8)) {
        if ($line -match '^##\s+([0-9]+\.[0-9]+\.[0-9]+)') {
            if (($Distinct.Count -eq 1) -and ($Matches[1] -ne $Distinct[0])) {
                Write-Warn "CHANGELOG.md leads at $($Matches[1]) while the plugin reports $($Distinct[0]) - fine mid-release, wrong once it ships"
            }
            break
        }
    }
}

Invoke-Check "The plugin descriptor and the source tree agree on modules" {
    $Uplugin  = Get-Content -LiteralPath (Join-Path $RepoRoot "Plugin/Uplink/Uplink.uplugin") -Raw -Encoding UTF8 | ConvertFrom-Json
    $Declared = @($Uplugin.Modules | ForEach-Object { $_.Name } | Sort-Object)
    $OnDisk   = @(Get-ChildItem -Path $SourceRoot -Directory | ForEach-Object { $_.Name } | Sort-Object)

    foreach ($m in $Declared) { if ($OnDisk -notcontains $m) { "Uplink.uplugin declares module '$m' with no directory under Plugin/Uplink/Source" } }
    foreach ($m in $OnDisk)   { if ($Declared -notcontains $m) { "Plugin/Uplink/Source/$m exists but Uplink.uplugin does not declare it, so it is never built" } }
    foreach ($m in $Declared) {
        if ($OnDisk -notcontains $m) { continue }
        if (-not (Test-Path (Join-Path (Join-Path $SourceRoot $m) ($m + ".Build.cs")))) { "module '$m' has no $m.Build.cs" }
    }
}

Invoke-Check "No machine-specific paths are committed in the plugin source or bridge" {
    foreach ($root in @($SourceRoot, (Join-Path $RepoRoot "bridge"))) {
        if (-not (Test-Path $root)) { continue }
        $files = Get-ChildItem -Path $root -Recurse -File -Include *.cpp, *.h, *.cs, *.js, *.json |
            Where-Object { $_.FullName -notmatch 'node_modules' }
        foreach ($f in $files) {
            $rel   = $f.FullName.Substring($RepoRoot.Length + 1)
            $lines = @(Get-Content -LiteralPath $f.FullName -Encoding UTF8)
            for ($i = 0; $i -lt $lines.Count; $i++) {
                # Strip URLs first: a scheme followed by a host and port looks
                # exactly like a drive-letter path to a naive pattern.
                $clean = $lines[$i] -replace '[A-Za-z][A-Za-z0-9+.-]*://[^\s"]*', ''
                if ($clean -match '(?<![A-Za-z0-9_])[A-Za-z]:[\\/]') {
                    "${rel}:$($i + 1) contains an absolute path: $($lines[$i].Trim())"
                }
                if ($clean -match '(?i)[\\/](Users|home)[\\/][A-Za-z0-9_.-]+[\\/]') {
                    "${rel}:$($i + 1) contains a home directory: $($lines[$i].Trim())"
                }
            }
        }
    }
}

# This repo is developed on a machine that also holds unrelated and private
# work, and prose written while looking at one of those projects carries its
# vocabulary out with it - a channel name in a comment, a class name in an
# example. Nothing about the code reveals it, so only a name search finds it.
#
# The list of names lives in scripts/private_terms.local.txt, which is
# gitignored: committing the terms would publish exactly what the check exists
# to keep out. One term per line, # for comments. Absent, the check is skipped
# rather than passed - a cloud runner has no such file and never will.
$PrivateTermsFile = Join-Path $PSScriptRoot "private_terms.local.txt"
if (-not (Test-Path $PrivateTermsFile)) {
    Write-Skip "No private project names appear in committed text" "no scripts/private_terms.local.txt on this machine"
}
else {
    Invoke-Check "No private project names appear in committed text" {
        $Terms = @(Get-Content -LiteralPath $PrivateTermsFile -Encoding UTF8 |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and (-not $_.StartsWith("#")) })
        if ($Terms.Count -eq 0) { return }

        $Targets = @()
        foreach ($dir in @("Plugin", "bridge", "scenarios", "scripts", ".github")) {
            $path = Join-Path $RepoRoot $dir
            if (Test-Path $path) {
                $Targets += Get-ChildItem -Path $path -Recurse -File -Include *.cpp, *.h, *.cs, *.js, *.json, *.md, *.ps1, *.yml, *.uplugin |
                    Where-Object { $_.FullName -notmatch 'node_modules|Binaries|Intermediate' }
            }
        }
        $Targets += Get-ChildItem -Path $RepoRoot -File -Filter *.md

        foreach ($f in $Targets) {
            if ($f.Name -eq "private_terms.local.txt") { continue }
            $rel   = $f.FullName.Substring($RepoRoot.Length + 1)
            $lines = @(Get-Content -LiteralPath $f.FullName -Encoding UTF8)
            for ($i = 0; $i -lt $lines.Count; $i++) {
                foreach ($t in $Terms) {
                    if ($lines[$i] -match [regex]::Escape($t)) {
                        # The term itself is not printed: this output ends up in
                        # CI logs and terminal scrollback.
                        "${rel}:$($i + 1) contains a private project name"
                    }
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------

Write-Info ""
Write-Host "================================================================"
if ($script:FailedChecks.Count -gt 0) {
    Write-Host ("CHECKS FAILED: " + ($script:FailedChecks -join "; ")) -ForegroundColor Red
    Write-Host ("$($script:FailedChecks.Count) of $($script:CheckCount) checks failed.") -ForegroundColor Red
    exit 1
}
$suffix = ""
if ($script:WarnCount -gt 0) { $suffix = " ($($script:WarnCount) warning(s))" }
if ($script:SkippedChecks.Count -gt 0) {
    $suffix += " $($script:SkippedChecks.Count) check(s) did not run: " + ($script:SkippedChecks -join ", ") + "."
}
Write-Host ("CHECKS PASSED: all $($script:CheckCount) checks$suffix") -ForegroundColor Green

# Exit explicitly: a script that falls off the end leaves $LASTEXITCODE at
# whatever the previous command set, and a caller reading it sees a stale
# failure from something unrelated.
exit 0
