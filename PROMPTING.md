# How to prompt an agent using Uplink

Uplink gives an assistant real access to your editor. What you get out of it
depends almost entirely on how much you say up front. This is not about magic
words — it is about the handful of facts an agent cannot discover on its own, and
the habits that keep it from guessing.

---

## Start every session with this

Paste it once, at the top. It costs you thirty seconds and saves a great deal of
flailing.

```
Project: <name> · Unreal <5.7 or 5.8> · <C++ / Blueprint-only>
Entry map for playing: <the map a PLAYER starts from>
Editor opens on: <the map that opens when you launch — often a different one>
What I'm working on: <one sentence>
Don't touch: <folders, assets, or systems that are off limits>
```

The first two lines matter more than they look. The map your editor opens is
frequently **not** the map the game starts from, and starting play on the wrong
one gives a world that is loaded but empty. Ask the agent to run `project_entry`
if you are not sure which is which — it reports both.

---

## The prompts that work

### Understanding a project

```
Run project_entry and tell me how this game is meant to be started.
```
```
This project isn't mine. Map out how it's put together: the entry map, the
game mode, the main character Blueprint, and which input actions exist.
```
```
What's in this level? Give me the actor classes by count, not a list of 2000
names.
```

### Fixing what is broken

This is where Uplink is strongest, and where a vague prompt wastes the most time.

```
Run bp_find_broken across /Game and show me the causes, not the errors.
```
```
60 assets are broken by the same missing function. Try bp_repair on them
first, then tell me which ones still need a real decision from me.
```
```
I renamed a C++ function. Find every Blueprint that referenced the old name
and tell me the blast radius before we change anything.
```

Say **"show me the causes"** rather than "show me the errors". Hundreds of error
lines usually come from two or three root changes, and the grouping is what turns
that into a decision you can actually make.

### Authoring

```
Create an Actor Blueprint at /Game/Test/BP_Door with a static mesh and a box
trigger, wire BeginPlay to print "ready", compile it, and save.
```
```
Build the whole event graph in ONE bp_modify call, then arrange it.
```
```
Add a float variable to BP_Player, then show me bp_query so I can see it
landed.
```

Ask for **one batched call** when building a graph. It is faster, and a failed
batch tells you exactly which step broke.

### Running and checking

```
Start play on <entry map>, wait for the pawn, walk it forward for two seconds
with the project's own move action, and tell me where it ended up.
```
```
Watch every delegate on BP_Door, trigger it, and show me what fired with the
payloads.
```
```
Write this as a run_scenario file in scenarios/ so I can re-run it later.
```

### When something looks wrong

```
Uplink stopped responding. Check dialog_state — I think a modal is blocking it.
```
```
The screenshot looks empty. Check streaming_status: are the sublevels actually
loaded, or is the level just not there yet?
```
```
Use frame_strip over the next three seconds — I want to see what changes, not
one frozen frame.
```

---

## Habits that make a large difference

**Ask for verification, not just action.** "Add the variable **and show me
bp_query**" is a different instruction from "add the variable". The second can
report success without you ever seeing proof.

**Say what "done" looks like.** "Make the door work" has no end state. "The door
should open when the player overlaps the trigger — start play and show me the
event firing" does.

**Name the file or asset when you know it.** Searching is cheap but guessing is
not, and a wrong guess can be confidently wrong.

**Let it read before it writes.** On an unfamiliar system, "read the Blueprint
and tell me how it works, don't change anything yet" is almost always the right
first move.

**Ask for the plan on anything bulk.** `bp_repair` has `dry_run`. So does your
own judgement — "show me what you'd change first" costs one message.

**Say when something is a guess.** If you are not sure the entry map is right,
say so. An agent told "I think it's this one" will verify; an agent told "it's
this one" will build on it.

---

## Things to be careful with

- **Nothing is saved unless you ask.** Authoring leaves work dirty in memory and
  an editor restart discards it. Say "and save" when you mean it.
- **Edits are undoable, but only in the editor.** Every mutating tool runs in its
  own named transaction — Ctrl+Z works, and so does `edit_history`. Changes made
  while the game is running are not transacted.
- **`call_function` can call anything Blueprint could.** That is what makes the
  engine reachable without a tool per feature. It also means "delete all the
  assets in this folder" is a thing it can do if you ask for it.
- **Use version control.** The same advice as for any tool that edits in bulk.
- **Big scans cost time.** `bp_find_broken` compiles every Blueprint it checks.
  On a large project, start with a folder rather than all of `/Game`.

---

## A worked session

```
Project: Nightfall · Unreal 5.8 · C++
Entry map for playing: /Game/Maps/MainMenu
Editor opens on: /Game/Maps/Overworld
What I'm working on: the combat prototype
Don't touch: /Game/Cinematics

First, run bp_find_broken across /Game/Combat and show me the causes.
```

```
Right — the missing SaveBool is mine, I renamed it. Run bp_repair on those
seven with dry_run first.
```

```
Go ahead, and save the ones that compile clean.
```

```
Now start play on the entry map, walk the character forward two seconds, and
screenshot what the player sees.
```

Four messages, each with a clear end state, each verifiable. That is the shape
that works.
