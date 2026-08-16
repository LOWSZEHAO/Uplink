# Uplink

**A direct data link between an AI agent and a running Unreal Engine editor — including the running game.**

Uplink is an Unreal Engine editor plugin plus an optional [MCP](https://modelcontextprotocol.io) bridge. It lets an AI assistant do what other Unreal MCP servers do — search assets, edit Blueprints, spawn and move actors, set properties, read logs, take screenshots — **and then actually play the game**: start a session, drive the player, inject input, watch gameplay events fire, wait for conditions, and return a structured pass/fail answer to "does this level actually work?"

> Most Unreal MCP servers *edit* your project. Uplink also *plays* it.

## What it has actually done

All of the following happened over plain MCP calls, with no human at the keyboard:

- **Ran a complete playtest in one call** — one `run_scenario` request started PIE, teleported the player, walked them with injected input, verified the new position, screenshotted the player's view and shut the session down: **7/7 steps passed in 7.4 seconds**, with a structured report.
- **Played a VR game with no headset** — `xr_simulate` found the rig on the pawn, placed its right hand on a lever at distance 0, and the hand still read the same position three seconds later. A one-second `input_action` hold walked the pawn 300 units through the project's real Enhanced Input mappings.
- **Caught gameplay events with proof** — one `watch_events` call bound all 16 delegates on an actor; destroying it produced captured `OnEndPlay` / `OnDestroyed` events with fully decoded payloads, resolving an asynchronous `wait_until` assertion in 0.33 s.
- **Authored a working Blueprint from nothing** — created an Actor Blueprint, added a variable, placed and wired `ReceiveBeginPlay` → `PrintString`, compiled with zero errors, then spawned it in a running game: the variable read back correctly and its print appeared in the log.
- **Rebuilt the Grand Canyon from real data** — decoded public-domain elevation tiles into a 16 km landscape with the canyon's true 1.8 km relief, imported CC0 PBR sets, authored a slope-blended landscape material node-by-node through `call_function`, lit it with `lighting_setup`, and verified the result by screenshotting from a rim the elevation data itself said was there.

You do not have to take any of that on trust: [`scenarios/`](scenarios) holds a suite of executable `run_scenario` files, and one command runs them against your own editor.

```powershell
.\scripts\run_scenarios.ps1
```

A whole playtest is one request:

```json
{ "steps": [
    { "tool": "pie_start",       "params": { "mode": "viewport" } },
    { "tool": "player_teleport", "params": { "location": { "x": 0, "y": 0, "z": 0 } } },
    { "tool": "input_action",    "params": { "action": "/Game/Input/IA_Move.IA_Move",
                                             "value": { "x": 0, "y": 1 },
                                             "mode": "hold", "duration": 1.0 } },
    { "tool": "wait_until",      "params": { "condition": { "type": "property_equals",
                                             "actor": "MyPawn", "property": "bIsResting",
                                             "value": false } } },
    { "tool": "viewport_screenshot" },
    { "tool": "pie_stop" }
] }
```

## Why

Unreal MCP servers are overwhelmingly *editor authoring* tools — including UE 5.8's official one. An AI can build a level with them, then has to ask a human to test it. Uplink closes the loop: **build → play → control → observe → assert → fix.**

## Status

**v0.23.0 — 102 tools, one codebase compiling against both UE 5.7 and 5.8 (Win64, editor builds).** Every capability is verified against a live editor and a running game before it ships. Pre-1.0: the API may still change, and what it most needs is mileage on more real projects.

Recent work went into two things — testing VR without a headset (`xr_simulate`), and making every call trustworthy: mutating tools run inside named undo transactions, an asset path that resolves to nothing is refused rather than written as null, a misspelt parameter is rejected with a suggestion instead of ignored, and query tools cap their output and say when they truncated.

## Quickstart

**Prerequisites**

- Unreal Engine **5.7 or 5.8**, Win64, editor build
- A **C++ project** — a source plugin cannot compile into a Blueprint-only project. (Add any C++ class from the editor first; that converts it.)
- **Visual Studio 2022** with the *Game development with C++* workload
- An MCP client. Developed against [Claude Code](https://claude.com/claude-code); the protocol is standard, so other MCP clients should work — see below.

**Steps**

1. **Clone:**

   ```powershell
   git clone https://github.com/LOWSZEHAO/Uplink
   ```

2. **Link the plugin into your project** (a junction, so there is only ever one copy):

   ```powershell
   .\scripts\link_into_project.ps1 -ProjectDir "C:\Path\To\YourProject"
   ```

3. **Build** — open the project and let the editor compile it, or verify against both engines standalone:

   ```powershell
   .\scripts\build_all.ps1
   ```

4. **Register with Claude Code — no Node required.** The plugin speaks MCP natively over HTTP:

   ```powershell
   claude mcp add --transport http uplink http://127.0.0.1:3777/mcp
   ```

5. Start the editor, then ask your client for the `status` tool. You should get engine, project and PIE state back.

**Optional bridge.** The native HTTP endpoint only exists while the editor runs, so a client shows the server as disconnected when the editor is closed (reconnect after launching it). If you would rather the tools stayed visible and answered "editor not running", use the Node bridge instead — this is the only part that needs **Node.js 18+**:

```powershell
cd bridge; npm install
claude mcp add uplink -- node "<repo>\bridge\index.js"
```

## What it can do to your project

Worth understanding before you point an agent at real work.

- **There is no authentication.** Any process on your machine can call the server while the editor is running. It binds loopback only (`127.0.0.1`, never reachable from the network), validates browser `Origin` headers, caps bodies at 2 MB, and refuses to start if the port is taken or the engine's HTTP listener has been pointed at a non-loopback address — but on your own machine it is open by design.
- **`call_function` calls arbitrary UFUNCTIONs, deliberately.** That is what makes most of the engine reachable without a tool per feature. It also means the blast radius is "anything Blueprint could do", plus editor scripting libraries.
- **Mutations are undoable.** Every tool that writes runs inside its own named editor transaction, so an agent's edits roll back with Ctrl+Z or `edit_history` like a hand edit. PIE-world changes are not transacted — the engine does not record them.
- **Nothing is saved unless asked.** Authoring leaves work dirty in memory; `save` writes it. That cuts both ways: unsaved mistakes vanish on restart, and so does unsaved good work.
- **Use version control.** The same advice as for any tool that edits your project in bulk.

## The toolset

102 tools. Full parameters, conventions and worked recipes in **[TOOLS.md](TOOLS.md)**.

| Layer | Tools |
|---|---|
| **Understand an unknown project** | `project_entry` (default map, game mode, and every level — the difference between the map the editor opens and the map a player starts from) · `ui_live` (what is on screen right now, with text and screen rects) · `input_map` (mapping contexts, actions and their keys) · `actor_components` (a live actor's components under their real names) · `streaming_status` (which sublevels are loaded) · `frame_strip` (N frames as one contact sheet — what changed) · `dialog_state` (is a modal blocking every tool) |
| **Play the game** | PIE lifecycle (`pie_start` waits for BeginPlay, plus stop/status/pause/resume/step) · player control (`input_action`, `input_key`, `click_widget`, `possess`, `player_teleport`, `player_info`, `navigate_to`) |
| **Virtual reality** | `xr_simulate` — pose the HMD and hands, or `reach` for an actor, with no headset connected; tells VR and desktop rigs apart so a desktop fallback cannot pass a VR test |
| **Observe & assert** | `watch_events` / `drain_events` / `unwatch` (any BlueprintAssignable delegate, payloads decoded) · `wait_until` (timeout is a result, not an error) · `get_world_state` · `viewport_annotate` · `perf_stats` · `profile_capture` |
| **Scripted playtests** | `run_scenario` — ordered steps, per-step expectations, `$steps[N].field` templating, structured pass/fail |
| **Record & replay** | `input_record` (passive tap on a real play session) · `input_replay` (a regression test from a human run) |
| **Ask the world** | `trace` (line/sphere/box/capsule, by channel or collision profile) · `ai_query` (behaviour tree **active node** and blackboard) · `material_query` (expressions, connections, **compile errors**) |
| **Blueprints** | `bp_create` · `bp_query` · `bp_add_component` · `bp_modify` (variables, function graphs, nodes, wiring, node properties — a whole event graph in one batched call) · `bp_compile` |
| **Content** | widgets · animation & cinematics · motion matching (GASP) · Niagara · PCG · landscape, lighting and foliage |
| **Editor & world** | actors (`level_actors`, `spawn_actor`, `spawn_batch` up to 1000, `delete_actors`, `move_actor`) · screenshots and camera · `edit_history` (undo stack) · `ui_tree` / `capture_widget` (any editor panel, even occluded) · `live_compile` |
| **Reflection** | `get_property` / `set_property` (dotted paths through structs *and* object references) · **`call_function`** (any UFUNCTION) · `class_info` · `find_functions` |
| **Assets & data** | `asset_search` / `asset_dependencies` / `asset_referencers` / `asset_import` · `save` · DataTables · `run_tests` |

Every world-aware tool takes `world: "editor" | "pie"` and defaults to the live PIE world during play.

## Works with any MCP client

Uplink speaks plain MCP — nothing is Claude-specific. It is developed and tested against Claude Code; other MCP clients (Cursor, Windsurf, Cline, Copilot agent mode, Gemini CLI, Codex CLI) speak the same protocol and should work, though they are not part of the test loop.

- Clients supporting **HTTP ("streamable HTTP") servers** → point them at `http://127.0.0.1:3777/mcp`:

  ```json
  { "mcpServers": { "uplink": { "url": "http://127.0.0.1:3777/mcp" } } }
  ```

- Clients that only support **stdio servers** → use the bridge:

  ```json
  { "mcpServers": { "uplink": { "command": "node", "args": ["<repo>/bridge/index.js"] } } }
  ```

Exact config file names and key spellings vary per client — check your client's MCP documentation.

## Architecture

```
                        ┌── HTTP 127.0.0.1:3777 ────────────┐
AI agent (MCP client) ──┤   (native MCP — no Node needed)   ├── Plugin/Uplink (UE editor)
                        └── stdio ── bridge/ (optional) ────┘        │
                                                                     │ self-describing tool registry
                                                                     │ game-thread dispatcher + task queue
                                                                     │ world resolver (editor | PIE)
                                                                     └ per-tool editor transactions
```

- **`Plugin/Uplink`** — a C++ editor plugin serving MCP natively over a loopback HTTP endpoint. One codebase compiles against **UE 5.7 and 5.8**; all version divergence lives in [`UplinkCompat.h`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkCompat.h).
- **`bridge/`** — an optional Node stdio MCP server for clients that cannot speak HTTP. It stays alive when the editor is closed, so tools degrade to a clear "not connected" answer instead of the MCP server dying.

Every tool declares whether it only reads. Anything that writes is dispatched inside its own editor transaction — which is what makes an agent's edits undoable by hand afterwards — and its parameters are checked against its own published schema before it runs.

## Extending Uplink with your own tools

Other plugins can contribute project-specific tools without forking Uplink: implement [`IUplinkToolProvider`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkToolProvider.h), register it as a modular feature, and your tools appear alongside the built-ins (late-loading plugins included). See the header for a complete example.

## Repository layout

```
Plugin/Uplink/     the UE editor plugin (C++)
bridge/            Node stdio MCP server (optional)
scripts/           build, project-linking and scenario-running helpers (PowerShell)
scenarios/         runnable proof - a regression suite in the tool's own format
TOOLS.md           full tool reference (parameters, conventions, security model)
```

## License

[Apache-2.0](LICENSE) © 2026 Low Sze Hao
