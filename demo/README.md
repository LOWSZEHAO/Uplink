# Demo

One script, three acts, against a running editor. Everything in it is a real
MCP tool call — the Blueprint does not exist when the script starts, and the
numbers at the end are read back out of the running game.

```powershell
node demo\demo.js
```

| | |
|---|---|
| `--pace 2` | seconds between beats, for narrating over it (default 1.5) |
| `--clean` | remove what a previous run left behind, and stop |

Reads `UPLINK_URL` and `UPLINK_AUTH_TOKEN`, the same two the [bridge](../bridge)
uses. Needs Node 18+ and an editor with the plugin enabled; nothing else.

## What it does

**Arrive.** `project_entry` answers where a player actually starts — default
map, editor startup map, game mode, every playable map. The map the editor
opens is often not the one the game begins at, and that is worth knowing before
touching anything.

**Change.** Author a moving platform from nothing: a new Actor Blueprint, a
static mesh component, two variables, and a **Timeline** — the template, its
curve, and the graph node — then ten connections wiring
`Timeline Update → Lerp(start, start + Z·Height) → SetActorLocation`. Compiled
clean, in one batched call.

**Prove.** Place one, start play, and measure it. The platform sweeps
`z=150 → 450 → 150`, travelling exactly the 300 cm its `Height` asks for.

That last act is the point. A demo that ends at "the Blueprint compiled" has
not shown the loop closing — the interesting claim is that the thing an agent
wrote sixty seconds ago is now moving the world, and that the same tools can
measure it. If the platform does not move, the script stops rather than
printing a success it did not earn.

## Running it on your own project

It works anywhere with an actor-based Blueprint and a playable map. It touches
only `/Game/Demo`, spawns one actor labelled `DemoPlatform`, and `--clean`
removes both. The mesh is `/Engine/BasicShapes/Cube.Cube`, which ships with the
engine, so there is nothing to import.

A first play on an unfamiliar map compiles shaders and can look broken on
camera. Run it once as a warm-up before recording.
