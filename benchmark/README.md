# The playtest benchmark

A four-actor game, built by Uplink, with five bugs that can be switched on one
at a time. It exists to answer one question honestly:

> Given a change and a goal, can an agent play the thing, notice it is broken,
> work out **why**, fix it, and prove the fix — without breaking what already
> worked?

That is a narrower question than "can an AI play my game", and deliberately so.
Playing an arbitrary game is a perception and planning problem. Verifying a
known behaviour against a stated success condition is a tooling problem, and
this is the tooling.

## The game

```
   player  →  [cube]  →  [pressure plate]  →  [door]  →  [exit]
              pick up      stand on it       opens      reach it
```

Four Blueprints, four mechanisms, all overlap-driven — no input mapping, so it
runs on any project with a player pawn:

| Actor | What it does | Variable to read |
|---|---|---|
| `BP_Bench_Cube` | Attaches to the pawn that walks into it, snapped to its origin, and broadcasts `OnPickedUp` | `bIsCarried` |
| `BP_Bench_Plate` | Presses when a **cube** overlaps it — not a pawn — and broadcasts `OnPressed` | `bIsPressed` |
| `BP_Bench_Door` | Finds the plate at BeginPlay, binds `OnPressed`, and lifts by `OpenHeight` | `bIsOpen` |
| `BP_Bench_Exit` | Sets `bReached` when a pawn overlaps it | `bReached` |

The chain matters more than any one link: the plate only presses if the cube is
genuinely carried, so a broken pickup shows up two steps later as a door that
never opens. That is what makes diagnosis a real exercise rather than reading
one boolean.

## Running it

These are ordinary `run_scenario` files, run the same way as everything in
[`scenarios/`](../scenarios), pointed at this folder:

```powershell
.\scripts\run_scenarios.ps1 -Directory benchmark
```

- **`00-build`** — creates the four Blueprints (components, variables,
  dispatchers, full event graphs) and places them. Run it once. It is kept as a
  scenario rather than as `.uasset` files so the benchmark is reproducible in any
  project and readable as a diff.
- **`01-golden-path`** — plays the game and asserts every link. This has to pass
  before any other result here means anything: a failure must be the injected
  bug and not the harness.
- **`02-inject-and-diagnose`** — switches on the first bug, proves it breaks the
  chain in the way it should, and switches it off in `teardown` so it cannot
  poison the next run.

## The five bugs

Each is one deterministic edit that breaks a different link. Only bug 1 ships as
a scenario; the other four are the same one-line shape and are listed here so
you can run them by hand or write the file.

| # | Injected fault | Injection |
|---|---|---|
| 1 | Cube cannot be touched | `call_function` `SetActorEnableCollision(false)` on `Bench_Cube` |
| 2 | Cube already flagged as carried, so the pickup branch never runs | `set_property` `bIsCarried = true` on `Bench_Cube` |
| 3 | Plate cannot detect anything | `call_function` `SetActorEnableCollision(false)` on `Bench_Plate` |
| 4 | Door opens by nothing | `set_property` `OpenHeight = 0` on `Bench_Door` |
| 5 | Exit cannot be touched | `call_function` `SetActorEnableCollision(false)` on `Bench_Exit` |

Use the **setter**, not the stored flag. Writing `Mesh.BodyInstance.CollisionEnabled`
directly stores a value the component never applies, so the bug quietly does not
happen and the run passes — which was the first thing that went wrong while
building this.

Bugs 2 and 4 are the interesting ones. Both leave the state flag **true** —
`bIsCarried` for one, `bIsOpen` for the other — so the boolean an agent would
naturally check reports success while the world disagrees. Diagnosing those
means looking at where things actually are, not at what they claim.

## What to measure

Run one bug, give the agent the goal and nothing else, and score:

1. **Performed the task** — drove the player through the whole chain
2. **Detected the failure** — noticed it did not complete, rather than reporting success
3. **Identified the cause** — named the actual broken link, not a plausible one
4. **Fixed it** — changed the project
5. **Re-ran** — played it again rather than declaring victory
6. **Verified** — the chain completes now
7. **Broke nothing** — the other four links still work

Steps 2 and 3 are the interesting ones. Detection is mostly mechanical: the exit
flag is false. Diagnosis is not, because the symptom is always at the end of the
chain and the cause is usually not.

## Results

Not filled in. Running this properly means driving a real agent through all five
bugs and scoring it, which is a session of its own, and a table of numbers
invented here would be worth less than an empty space.

What has been verified is the harness: the game builds, the golden path
completes, and each of the five bugs breaks the chain and restores cleanly.
