# Uplink

**A direct data link between an AI agent and a running Unreal Engine editor — including the running game.**

Uplink is an Unreal Engine editor plugin plus a thin [MCP](https://modelcontextprotocol.io) bridge that lets an AI assistant (Claude Code, or any MCP client) do everything existing Unreal MCPs do — search assets, edit Blueprints, spawn and move actors, set properties, read logs, take screenshots — **and the thing none of them do: actually play the game.**

> Other Unreal MCPs *edit* your game. Uplink also *plays* it.

Start a Play-In-Editor session, drive the player, inject input, call any gameplay function, watch gameplay events fire, wait for conditions, and get a structured pass/fail answer to "does this level actually work?" — without a human touching the keyboard.

## Why

Every notable Unreal MCP today (including UE 5.8's official MCP plugin) is an *editor authoring* tool. An AI can build a level with them, but must then ask a human to test it. Uplink closes the loop the AI actually needs: **build → play → control → observe → assert → fix.**

## Status

**v0.19.0 — procedural, asset and motion-matching pipelines.** 89 tools across every layer, one codebase compiling against both **UE 5.7 and 5.8**, every capability verified against a live editor and running game. Terrain can now come from real elevation data: public-domain DEM tiles decode into `landscape_create` heightmaps, CC0 PBR texture sets import via `asset_import`, and a slope-blended landscape material gets authored node-by-node over `call_function` — the Grand Canyon demo map is built from the actual USGS-derived elevation of the canyon. `spawn_batch` places up to 1000 actors in one call for scene assembly at scale. Graphs it authors follow strict style rules: nodes never overlap, exec chains lay out as straight horizontal lanes, and turning wires get reroute knots so runs stay level and clean (`bp_modify` `arrange`; wire rendering is stock Unreal). Remaining before 1.0: broader real-project mileage and API polish from feedback.

**The toolset (89 tools):**

- **Play the game:** `pie_start` (waits until BeginPlay has actually run; viewport or new window, spawn/game-mode/map overrides) · `pie_stop` · `pie_status` · `pie_pause` / `pie_resume` · `pie_step` (advance a paused game exactly N frames)
- **Control the player:** `input_action` (Enhanced Input injection — pulse, timed hold, live value updates; no physical device needed) · `input_key` (raw key taps/edges/axis through the engine's simulated-input path) · `click_widget` (click UMG menus and buttons in the running game via real hit-testing) · `possess` · `player_teleport` · `player_info` · `navigate_to` (walk the pawn to a goal on the navmesh, like a click-to-move player)
- **Observe & assert:** `watch_events` / `drain_events` / `unwatch` (record any BlueprintAssignable delegate firing, with decoded parameter payloads) · `wait_until` (non-blocking assertions: property values, actor existence, event counts, elapsed time) · `get_world_state` (actor snapshot with arbitrary property values) · `perf_stats` · `profile_capture` (frame-time distribution + hitch counts over N seconds) · `viewport_annotate` (screenshot + screen-space rects of visible actors — grounded seeing)
- **Scripted playtests:** `run_scenario` — a whole playtest in one call: ordered tool steps with per-step expectations, timeouts, `$steps[N].field` templating between steps, and a structured pass/fail report. Example: start PIE → teleport → injected-input walk → assert position → screenshot → stop, in a single request.
- **Record & replay:** `input_record` (capture a real play session's keys, mouse and axes through a passive input tap) · `input_replay` (play the take back through the simulated-input path — a regression test from a human run)
- **Tests & data:** `run_tests` (run engine/project automation tests by filter with per-test pass/fail) · `datatable_create` / `datatable_query` / `datatable_modify` (tables and rows as JSON)
- **Animation & cinematics:** `anim_query` / `anim_modify` (montage/sequence timing truth, notify placement) · `animbp_query` (state machines, states, transitions, anim nodes) · `skeleton_query` (bone hierarchy + sockets) · `socket_modify` (add/move/remove attachment sockets) · `sequence_query` (Level Sequence bindings, tracks and section timings in seconds)
- **Blueprints:** `bp_create` · `bp_query` (variables, components, graphs, nodes, pins, connections) · `bp_add_component` (build a component hierarchy — meshes, triggers, scene parents — with transforms, mesh assignment and collision profiles) · `bp_modify` (variables; call-function / custom-event / engine-event / component-bound-event / variable get-set nodes; wiring; pin defaults — **batchable: a whole event graph in one call** via `ops` with `@ref` node handles) · `bp_compile` (full diagnostics)
- **Editor UI:** `ui_tree` (query the live Slate widget hierarchy — every window, tab and panel, with labels and screen rects) · `capture_widget` (screenshot any editor window or single panel — asset-editor previews included — even when occluded)
- **Niagara (UE 5.8+):** `niagara_create` (from engine templates) · `niagara_query` (stack, compile state, issues with fix hints) · `niagara_add_module` / `niagara_remove_module` · `niagara_module_inputs` · `niagara_set_input` · `niagara_renderer` (read/write renderer properties, e.g. swap the sprite material) · `niagara_set_user_param` · `niagara_compile`
- **Editor & world:** `status` · `console_command` · `output_log` (incremental log reads — pair with `pie_status.log_start_index` to read just the current session) · `viewport_screenshot` (PNG image results) · `viewport_camera` (frame an actor or set the editor camera before capturing) · `level_actors` · `spawn_actor` · `spawn_batch` (up to 1000 actors in one call — meshes with material overrides or any actor class) · `delete_actors` · `move_actor` · `live_compile` (Live Coding patch of the running editor — C++ iteration without restarts)
- **Reflection:** `get_property` / `set_property` (any UPROPERTY as JSON) · **`call_function`** (any UFUNCTION — args in, return value and out-params back as JSON)
- **Assets:** `asset_search` / `asset_dependencies` / `asset_referencers` / `asset_import` (bring FBX/textures/audio from disk into the project, automated)
- **Environment:** `lighting_setup` (whole scene lighting stack in one call) · `landscape_create` (heightmap → real Landscape terrain) · `foliage_scatter` (instanced meshes traced onto the ground) · `plugin_list` / `plugin_enable` (see and toggle the project's plugins)
- **PCG (procedural generation):** `pcg_create` (new PCG graph asset) · `pcg_add_node` (nodes by friendly name — SurfaceSampler, StaticMeshSpawner, GetLandscape — with settings applied inline) · `pcg_connect` (wire or unwire pins) · `pcg_query` (nodes, settings classes and exact pin labels) · `pcg_generate` (attach a PCG component to an actor, assign the graph, generate). Reflection-based, so the plugin still loads in projects where PCG is disabled.
- **Motion matching / GASP:** `posesearch_query` (a Pose Search database: schema, sample rate, feature channels and every animation it can select) · `chooser_query` (the Chooser table deciding WHICH database runs for the current game state) · `motionmatch_debug` (live state off a character in the running game — the motion-matching nodes plus the anim values driving them). Reflection-based; the plugin still loads whether or not PoseSearch/Chooser are enabled.
- **Tasks:** `task_status` / `task_result` / `task_cancel` / `task_list`

Every world-aware tool takes `world: "editor" | "pie"` and defaults to the live PIE world during play. Full parameter reference: [TOOLS.md](TOOLS.md).

## Seen in action

Things Uplink has actually done over plain MCP calls, no human at the keyboard:

- **Played a VR game with no headset and no keyboard** — a one-second `input_action` hold on an Enhanced Input move action walked the player pawn 300 units; a raw `input_key` W-tap moved it through the project's real key mappings.
- **Ran a complete playtest in one call** — a single `run_scenario` request started PIE, teleported the player, walked them with injected input, verified the new position, took a screenshot of the player's view, and shut the session down: 7/7 steps passed in 7.4 seconds with a structured report.
- **Caught gameplay events with proof** — one `watch_events` call bound all 16 delegates on an actor; destroying it produced captured `OnEndPlay` / `OnDestroyed` events with fully decoded payloads, resolving an asynchronous `wait_until` assertion in 0.33 s.
- **Authored a working Blueprint from nothing** — created an Actor Blueprint, added a variable, placed and wired `ReceiveBeginPlay` → `PrintString`, compiled with zero errors, then spawned it in a running game: the variable read back correctly and its print appeared in the log.
- **Rebuilt the Grand Canyon from real data** — decoded public-domain elevation tiles into a 16 km landscape with the canyon's true 1.8 km relief, imported CC0 rock and sand PBR sets from the open web, authored a slope-blended landscape material node-by-node through `call_function`, lit it with `lighting_setup`, and verified the result by screenshotting from a rim the DEM itself said was there.

A taste of the scenario format:

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

## Extending Uplink with your own tools

Other plugins can contribute project-specific tools without forking Uplink: implement [`IUplinkToolProvider`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkToolProvider.h), register it as a modular feature, and your tools appear alongside the built-ins (late-loading plugins included). See the header for a complete example.

## Architecture

```
AI agent (MCP client) ── stdio ── bridge/ (Node) ── HTTP 127.0.0.1:3777 ── Plugin/Uplink (UE editor)
                                   │                                        │
                                   │ auto-generates MCP tools from /tools   │ self-describing tool registry
                                   │ async submit/poll for long operations  │ game-thread dispatcher + task queue
                                   └ degrades gracefully when editor closed └ world resolver (editor | PIE)
```

Two pieces, one contract:

- **`Plugin/Uplink`** — a C++ editor plugin exposing a localhost REST API. One codebase compiles against **UE 5.7 and 5.8** (all version divergences live in [`UplinkCompat.h`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkCompat.h)).
- **`bridge/`** — a small Node stdio MCP server that translates MCP to the plugin's REST API. It stays alive when the editor is closed (tools degrade to a clear "not connected" status instead of the MCP server dying), handles async task polling, and keeps MCP protocol churn out of compiled C++.

## Requirements

- Unreal Engine **5.7 or 5.8** (Win64, editor builds)
- **Node.js 18+** for the bridge
- An MCP client — [Claude Code](https://claude.com/claude-code) is the primary target

## Quickstart

1. **Get the plugin into your project** (junction, so there is only ever one copy):

   ```powershell
   .\scripts\link_into_project.ps1 -ProjectDir "C:\Path\To\YourProject"
   ```

2. **Build** — open the project (the editor offers to compile), or verify against both engines standalone:

   ```powershell
   .\scripts\build_all.ps1
   ```

3. **Register with Claude Code — once, no Node required.** The plugin speaks MCP natively over HTTP:

   ```powershell
   claude mcp add --transport http uplink http://127.0.0.1:3777/mcp
   ```

   From then on the flow is just: enable the plugin, start the editor, and the tools are live.

4. In Claude Code, ask for the `status` tool — you'll get engine/project/PIE state back.

**Optional bridge mode.** The native HTTP endpoint only exists while the editor runs, so Claude Code shows the server as disconnected when the editor is closed (reconnect via `/mcp` after launching it). If you prefer tools that stay visible with a clean "editor not running" answer instead, use the Node bridge:

```powershell
cd bridge; npm install
claude mcp add uplink -- node "<repo>\bridge\index.js"
```

## Works with any MCP client

Uplink speaks plain MCP — nothing here is Claude-specific. Any client that supports MCP servers can use it: Cursor, Windsurf, Cline, GitHub Copilot agent mode, Gemini CLI, Codex CLI, ChatGPT connectors, and so on.

- Clients supporting **HTTP ("streamable HTTP") servers** → point them at `http://127.0.0.1:3777/mcp`. Typical config shape:

  ```json
  { "mcpServers": { "uplink": { "url": "http://127.0.0.1:3777/mcp" } } }
  ```

- Clients that only support **stdio servers** → use the bridge:

  ```json
  { "mcpServers": { "uplink": { "command": "node", "args": ["<repo>/bridge/index.js"] } } }
  ```

Exact config file names and key spellings vary per client — check your client's MCP documentation.

**Security note:** the server binds to loopback only (`127.0.0.1` — your own machine; it is never reachable from the network), validates browser `Origin` headers on every route, caps request bodies at 2 MB, and refuses to start if the port is already taken or the engine's HTTP listener has been reconfigured to a non-loopback address.

## Repository layout

```
Plugin/Uplink/     the UE editor plugin (C++)
bridge/            Node stdio MCP server (optional)
scripts/           build + project-linking helpers (PowerShell)
TOOLS.md           full tool reference (parameters, conventions, security model)
```

## License

[MIT](LICENSE) © 2026 Low Sze Hao
