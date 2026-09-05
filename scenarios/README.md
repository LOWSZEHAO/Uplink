# Scenarios

Runnable proof. Each file is a `run_scenario` request — an ordered list of tool
steps with expectations — that exercises one part of Uplink against a real
editor and returns a structured pass/fail report.

They serve three purposes at once: a regression suite to run after changing the
plugin, evidence a visitor can execute rather than take on trust, and worked
examples of how the tools compose.

## Running them

Open any project with the plugin enabled, then:

```powershell
.\scripts\run_scenarios.ps1
.\scripts\run_scenarios.ps1 -Filter playtest    # just one
```

The script exits non-zero if anything fails, so it drops straight into a build
step if you want one.

| Scenario | What it proves |
|---|---|
| `01-smoke` | The editor is reachable and the basics answer. Changes nothing. |
| `02-edits-and-cleanup` | A mutating tool really changes the level, later steps see the change, and the scenario puts the world back. |
| `03-blueprint-authoring` | A Blueprint can be authored from nothing — variable, function graph, clean compile — and the engine agrees when read back. |
| `04-refuses-bad-input` | The failure behaviour: a mistyped parameter name, a wrongly-typed value, a value outside a declared enum, and an unresolvable asset path are each refused, not quietly ignored. Every step asserts its own refusal, and the teardown proves the first one by looking for the asset it must never have created. |
| `05-playtest` | Verification in a running game: start it, move the player, query the world, capture the view, shut down — one request. |
| `06-asset-creation` | The assets `bp_create` cannot make: a Widget Blueprint whose generated class resolves and takes a root widget, and a Material Instance parented through its factory. Names the factory it expects. |
| `07-graph-flow-control` | A gameplay graph with decisions in it: Branch, Sequence, Cast, a ForLoop macro, Make/Break Struct, Switch, Select and Self, built in one batched call and compiled clean. |
| `08-authoring-traps` | Five authoring calls that used to succeed while doing the wrong thing — array node class, function return values, widget-blueprint parent, duplicate event binding, single-child panel overfill. |
| `09-interfaces-and-dispatchers` | The parts of a Blueprint that are not nodes: a typed event dispatcher, an interface implemented from a second asset, a parent event overridden and called through, and `bp_references` answering "what uses this?". Two steps are regressions — overriding twice used to duplicate the event node, and a variable read in its own graph used to report nothing. |
| `11-invalid-object-reference` | A path that names nothing is refused rather than accepted as null — a missing class to `spawn_actor`, a missing asset to `get_property`, a bad reference written by `set_property` or passed as a `call_function` argument, and an edit aimed at a Blueprint that is not there. |
| `12-invalid-function-args` | How `call_function` handles a wrong argument: a name matching no parameter and a value of the wrong shape are both refused before the function runs. A *missing* argument is not refused — it zero-fills — and the last steps prove that rather than pretend otherwise. |
| `13-pie-editor-world-isolation` | `world:"editor"` and `world:"pie"` are two different worlds. Both probes are spawned after play begins, so each world sees exactly one of them, and the game's copy is gone when the session ends. |
| `14-multi-step-failure-rollback` | What a `bp_modify` batch does when an op in the middle fails: it stops, says which op and how many had been applied, and leaves those earlier ops in place. There is no rollback, and both halves are proved separately. |
| `15-blueprint-recompile` | A Blueprint that another one inherits from and calls into. A compatible change to the base leaves the dependent compiling clean; removing the function it calls leaves the dependent reported broken instead of quietly stale. |
| `16-volume-geometry` | A volume spawned through `spawn_volume` has real brush geometry, and a class that is not a volume is refused rather than half-built. `spawn_actor` reaches a volume class and produces one that bounds nothing — correctly named, correctly placed, no shape, no complaint — which is a nav bounds volume that generates no navmesh. |
| `17-property-path-indexing` | A dotted property path indexes an array element and keeps going through it, because the value worth asserting on is usually inside one entry of a list. Covers both misuses too: an out-of-range index reports the real length rather than "not found", which would read as a wrong property name. |
| `18-unknown-tool` | A step naming a tool that does not exist is refused before anything runs, and the refusal names the step. The one failure a scenario reports on the call rather than on a step, which is why it has a file to itself — sharing one, it aborted the file and the steps above it never ran. |
| `20-large-spawn` | Three hundred actors placed in one `spawn_batch`, counted in the world, and deleted again by the names the call returned — the reported count and the actual count are the same number, and the level ends where it started. |
| `21-event-params-and-struct-members` | A custom event created with parameters really carries them, a User Defined Struct takes members under the names it was given, and an Input Action can be placed as a graph event — each read back from the asset rather than trusted from the call. Includes the two refusals that guard engine behaviour worse than a plain failure: the `MemberVar_<n>` rename that arms a `check()`, and the last-member removal the engine declines at Log verbosity. |
| `22-property-spelling-and-subsystems` | A correct write is not reported as a failed one: an enum sent as a number, fully qualified, or an object path inside an array all count as landed even though the property reads back in its own spelling. The check keeps its teeth — a number no enumerator answers to is refused before the write, and a non-instance-editable variable reset by the construction scripts still fails. Plus `get_subsystem` across three subsystem families, each needing a different node class. |
| `23-variable-on-another-object` | A variable node about an object other than the blueprint holding it: `PlayerState` read off the `NewPlayer` controller inside `OnPostLogin`, wired through the Target pin that only an external reference has. Asserts zero *warnings* as well as zero errors, because an unresolved variable reference is only ever a warning. Plus the three refusals, and proof that omitting `class` still means self. |
| `24-enum-authoring` | An enum you can create is now one you can fill. A freshly created User Defined Enum has **zero** entries, so this adds them, reads them back, and uses the enum in a graph where the switch finally has case pins. Pins the four refusals — including the `move` index that lands on the hidden `_MAX` slot, which the engine's own bounds check admits and then inserts past the end of. |

Most of these run against any project. `05-playtest` and `13-pie-editor-world-isolation`
need a default map with a playable pawn, which every template has;
`13` also assumes the level has no `TargetPoint` actors of its own, since it
counts that class as its marker.

## Reading the results

Every scenario but one passes in the ordinary way: every step succeeds. Some
files mix in `expect_failure` steps, which pass by being refused and read green
alongside the rest.

**Refusals are asserted per step, with `expect_failure`.** Testing that bad
input is rejected matters as much as testing that good input works, because the
failure this project cares most about is a call that reports success while doing
nothing. `04-refuses-bad-input` used to be scored backwards by the runner as a
whole file; it no longer is, because a per-step assertion says which refusal it
wanted and a whole-file inversion does not.

**`18-unknown-tool` is the exception**, and the only scenario that asserts on
the call rather than on a step. A tool name that does not resolve is refused at
parse time, so no step runs at all — there is nothing to attach a per-step
assertion to. It declares `_expect_scenario_refused` and the runner requires
the call to come back refused with zero steps reported.

## Undo does not reach inside a scenario

Worth knowing before writing your own, and the reason there is no undo scenario.

Called on its own, every mutating tool runs inside its own transaction named
`Uplink: <tool>`, and `edit_history` walks those exactly like your own edits.
Inside a scenario neither half of that holds. `run_scenario` declares itself
non-transactional — a scenario that starts play would otherwise be fighting the
engine, which cancels any open transaction to begin PIE — and the runner drives
each step's invocation directly rather than through the task manager, so no
per-step transaction opens either. Whatever the engine transacts internally
still lands on the stack; nothing is grouped under an `Uplink:` name.

So `edit_history {action:"undo"}` from inside a scenario would not undo the
scenario. It would walk whatever was on the stack **before** the run, which
belongs to whoever was using the editor. Check undo by hand with `edit_history`
after a run instead, and do not put an `undo` step in a file.

## Writing your own

A step is `{tool, params?, expect?, expect_failure?, timeout?}`.

- `expect_failure: true` inverts one step: it passes when the tool refuses and fails when the tool lets it through. That is how `08-authoring-traps` and `04-refuses-bad-input` mix ordinary steps with rejection tests in one green scenario. It covers schema-level refusals too — a mistyped parameter name, a wrong type, a value outside an enum — but only since step params were validated inside `run_scenario`; before that the validator ran on the HTTP path only, so those refusals never happened in a scenario and a step asserting one passed for the wrong reason.

- `expect` matches fields of that step's **result data**, exactly. Assert on
  fields the tool actually returns — run the tool once and look, rather than
  guessing a plausible name. (Both mistakes in the first draft of these files
  were invented field names that read perfectly well and did not exist.)
- `expect` and `expect_failure` do not combine. A refused step never reaches its
  expectations, so there is nothing to assert about the refusal itself — assert
  what it left behind in the *next* step. `14-multi-step-failure-rollback` is
  built that way: the refusal is one step, and what survived it is three more.
- `"$steps[N].field.path"` in a later param is substituted from step N's result,
  so you can spawn something and then act on where it landed. Whole values
  substitute, not just strings — `20-large-spawn` hands the array of names its
  batch returned straight to `delete_actors`.
- Keys starting with `_` are notes for whoever reads the file. The runner strips
  them before sending, so use them freely.
- **Parameter names are not checked inside a scenario.** The "unknown parameter"
  rejection runs on the MCP path, not on the one `run_scenario` drives, so a
  misspelt parameter here is ignored rather than refused. Never write a step
  whose only assertion is that a bad parameter name gets caught: inside a file it
  will not be, and the step then passes or fails for some other reason entirely.
- Clean up after yourself, and delete leftovers **first** as well: an aborted run
  leaves debris behind, and the next run's count assertions then quietly mean
  something different.

## What is deliberately not here

Being explicit about the gap is worth more than a file that pretends to cover it.

- **Undo and redo.** The obvious scenario — make an edit, see it in
  `edit_history`, undo it, watch the world reverse — cannot be written honestly,
  for the reason in *Undo does not reach inside a scenario* above. A file that
  called `edit_history {action:"undo"}` would reverse whatever the person at the
  keyboard last did and prove nothing about its own edits. Undo is checked by
  hand.
- **World Partition and level streaming.** None of the templates these run
  against are partitioned, so a scenario would be asserting against a level shape
  that is not there, and would still say nothing about a project that has one.
- **Multiplayer.** `pie_start` brings up a single client. There is no
  second-client scenario because there is no tool that addresses a second client.
- **Hot reload and live coding.** A scenario runs inside the editor it is
  testing. Reloading that editor's own modules mid-run is not something a step in
  flight can survive, let alone assert on.
