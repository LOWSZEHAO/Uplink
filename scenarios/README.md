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

Everything here runs against any project. `05-playtest` needs a default map with
a playable pawn, which every template has.

## Reading the results

Seven of the eight pass in the ordinary way: every step succeeds.

**`04-refuses-bad-input` is inverted.** Every step in it is *meant* to fail —
that is the assertion. It runs with `stop_on_failure: false`, and the runner
knows to score it backwards: a step that succeeds there is the bug. Testing that
bad input is rejected matters as much as testing that good input works, because
the failure this project cares most about is a call that reports success while
doing nothing.

## A scenario is one undo unit

Worth knowing before writing your own. `run_scenario` is itself a mutating tool,
so the whole run executes inside a single editor transaction:

- **During** a run, `edit_history` reports `canUndo: false` — the scenario's own
  transaction is still open, so nothing is finalised yet.
- **After** it, the entire run collapses to one `Uplink: run_scenario` entry.

That is the useful behaviour — a playtest undoes as a single unit rather than
leaving you to unpick twenty steps — but it does mean undo cannot be asserted
from inside a scenario. Check it by hand with `edit_history` afterwards.

## Writing your own

A step is `{tool, params?, expect?, expect_failure?, timeout?}`.

- `expect_failure: true` inverts one step: it passes when the tool refuses and fails when the tool lets it through. That is how `08-authoring-traps` mixes ordinary steps with rejection tests in one green scenario — `04-refuses-bad-input` predates it and inverts the whole file instead.

- `expect` matches fields of that step's **result data**, exactly. Assert on
  fields the tool actually returns — run the tool once and look, rather than
  guessing a plausible name. (Both mistakes in the first draft of these files
  were invented field names that read perfectly well and did not exist.)
- `"$steps[N].field.path"` in a later param is substituted from step N's result,
  so you can spawn something and then act on where it landed.
- Keys starting with `_` are notes for whoever reads the file. The runner strips
  them before sending, so use them freely.
- Clean up after yourself, and delete leftovers **first** as well: an aborted run
  leaves debris behind, and the next run's count assertions then quietly mean
  something different.
