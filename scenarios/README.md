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
| `04-refuses-bad-input` | The failure behaviour: a mistyped parameter, an unresolvable asset path, and an unknown tool name are all refused, not quietly ignored. |
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
| `20-large-spawn` | Three hundred actors placed in one `spawn_batch`, counted in the world, and deleted again by the names the call returned — the reported count and the actual count are the same number, and the level ends where it started. |

Most of these run against any project. `05-playtest` and `13-pie-editor-world-isolation`
need a default map with a playable pawn, which every template has;
`13` also assumes the level has no `TargetPoint` actors of its own, since it
counts that class as its marker.

## Reading the results

Every scenario but one passes in the ordinary way: every step succeeds. Some
files mix in `expect_failure` steps, which pass by being refused and read green
alongside the rest.

**`04-refuses-bad-input` is inverted.** Every step in it is *meant* to fail —
that is the assertion. It runs with `stop_on_failure: false`, and the runner
knows to score it backwards: a step that succeeds there is the bug. Testing that
bad input is rejected matters as much as testing that good input works, because
the failure this project cares most about is a call that reports success while
doing nothing.

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

- `expect_failure: true` inverts one step: it passes when the tool refuses and fails when the tool lets it through. That is how `08-authoring-traps` mixes ordinary steps with rejection tests in one green scenario — `04-refuses-bad-input` predates it and inverts the whole file instead.

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
