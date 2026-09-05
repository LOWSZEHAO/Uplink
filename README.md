# Uplink

[![checks](https://github.com/LOWSZEHAO/Uplink/actions/workflows/checks.yml/badge.svg)](https://github.com/LOWSZEHAO/Uplink/actions/workflows/checks.yml)

**A direct data link between an AI agent and a running Unreal Engine editor.**

Uplink is a C++ editor plugin that speaks [MCP](https://modelcontextprotocol.io), plus an optional Node bridge. It hands an AI assistant the editor itself: assets, Blueprints, actors, materials, animation, and every reflected UFUNCTION in your project.

## Why

An agent editing your project is only useful if you can trust what it did. Most of the work here went into that rather than into new features. A tool that writes runs inside its own named editor transaction, so you can undo it by hand. An object path that resolves to nothing is refused instead of quietly written as null. A misspelt parameter comes back with a suggestion instead of being ignored. Lists say when they were cut short.

The other half is reach. `call_function` invokes any `BlueprintCallable` UFUNCTION in the engine or in your project, and `get_property` / `set_property` walk dotted paths through structs and object references. Systems with no dedicated tool are still reachable.

## The loop

Individually these are just tools. What they add up to is a development loop that closes without leaving the conversation:

```
understand  →  author  →  run  →  observe  →  assert  →  repair
     ↑                                                      │
     └──────────────────────────────────────────────────────┘
```

Read an unfamiliar project (`project_entry`, `class_info`, `input_map`), change it (`bp_modify`, `asset_create`, `set_property`), start it (`pie_start`), watch what happens (`watch_events`, `get_world_state`), decide whether that was right (`wait_until`, `run_scenario` expectations), and fix what was not (`bp_find_broken`, `bp_repair`). Then do it again.

Two honest limits on that picture. Every step is one round trip, so the loop is scripted rather than autonomous — you or the agent decide each move, and `run_scenario` is how a decided sequence gets replayed without a human in it. And nothing here plays your game: an agent can drive a character and read the result, but working out what a player would do next is a different problem than this plugin solves.

## Status

**v0.35.0 — 115 tools, one codebase compiling against UE 5.7 and 5.8 (Win64, editor builds).** The scenario suite in [`scenarios/`](scenarios) runs clean on the third-person template for both engines, and one command runs it against your own project.

That is not the same as being sure. Audits keep finding shipped calls that reported success while doing the wrong thing - most recently a scenario runner that never validated its own step parameters, so every regression scenario ran unchecked, and a test runner that could see 429 of 6432 tests and answered "no tests match" for the rest. Both had been green for months. Pre-1.0: the API may still change, and what it needs most is mileage on projects that are not mine.

Recent work has been trustworthiness, and the tools for reading an unfamiliar project. That is where an agent burns the most time: guessing at things it could have looked up.

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

   The link shares the plugin's *source* and gives each project its own `Binaries/` and `Intermediate/`, so one clone can serve a 5.7 project and a 5.8 one at the same time without either clobbering the other's build. (Before v0.27 the whole folder was one junction and you had to delete both by hand when switching engines; if you linked with an older script, re-run it with `-Remove` first.)

4. **Register with Claude Code — no Node required.** The plugin speaks MCP natively over HTTP:

   ```powershell
   claude mcp add --transport http uplink http://127.0.0.1:3777/mcp
   ```

5. Start the editor, then ask your client for the `status` tool. You should get engine, project and PIE state back.

**Optional bridge.** The native HTTP endpoint only exists while the editor runs, so a client shows the server as disconnected when the editor is closed (reconnect after launching it). The Node bridge stays up instead: with the editor closed it exposes only `status`, which answers "not connected" rather than the whole server going away. This is the only part that needs **Node.js 18+**:

```powershell
cd bridge; npm install
claude mcp add uplink -- node "<repo>\bridge\index.js"
```

## What it can do to your project

Worth reading before you point an agent at real work.

- **There is no authentication unless you ask for one.** By default any process on your machine can call the server while the editor is running. It binds loopback only (`127.0.0.1`, never reachable from the network), parses and checks browser `Origin` headers, caps bodies at 2 MB, and refuses to start if the port is taken or the engine's HTTP listener has been pointed at a non-loopback address — but on your own machine it is open by design. Launch the editor with `UPLINK_AUTH_TOKEN` set and every request must then present that bearer token instead.
- **Tools say what they can do.** Alongside the standard `readOnlyHint` and `destructiveHint`, Uplink publishes which tools run caller-named code (`call_function`, `console_command`), which need a live play session, and which routinely take a while — so a client can weigh a call before making it.
- **`call_function` calls arbitrary UFUNCTIONs, deliberately.** It is what makes most of the engine reachable without a tool per feature. It also means the blast radius is anything Blueprint could do, plus the editor scripting libraries.
- **Mutations are undoable, with exceptions.** A tool that writes runs in its own named editor transaction, so its edits roll back with Ctrl+Z or `edit_history` like a hand edit. Nothing transacts while a PIE session is live, because the engine does not record changes to a play world. Tools that drive the session rather than edit assets opt out on purpose: `pie_*`, `input_action`, `input_key`, `navigate_to`, `run_scenario`, `input_replay`, plus `live_compile`, `run_tests` and `edit_history` itself.
- **Nothing is saved unless you ask.** Authoring leaves work dirty in memory and `save` writes it. That cuts both ways: unsaved mistakes vanish on restart, and so does unsaved good work.
- **Use version control.** The same advice as for any tool that edits your project in bulk.

## The toolset

115 tools. Full parameters, conventions and worked recipes are in **[TOOLS.md](TOOLS.md)**. If you have not driven an editor from an agent before, **[PROMPTING.md](PROMPTING.md)** covers what to tell it: the few facts it cannot look up for itself, and the habits that stop it guessing.

| Layer | Tools |
|---|---|
| **Understand an unknown project** | `project_entry` (default map, game mode, and every level — the difference between the map the editor opens and the map a player starts from) · `ui_live` (what is on screen right now, with text and screen rects) · `input_map` (mapping contexts, actions and their keys) · `actor_components` (a live actor's components under their real names) · `streaming_status` (which sublevels are loaded) · `frame_strip` (N frames as one contact sheet — what changed) · `dialog_state` (is a modal blocking every tool) |
| **Play-In-Editor** | PIE lifecycle (`pie_start` waits for BeginPlay, plus stop/status/pause/resume/step) · player control (`input_action`, `input_key`, `click_widget`, `possess`, `player_teleport`, `player_info`, `navigate_to`) |
| **Observe & assert** | `watch_events` / `drain_events` / `unwatch` (any BlueprintAssignable delegate, payloads decoded) · `wait_until` (timeout is a result, not an error) · `get_world_state` · `viewport_annotate` · `perf_stats` · `profile_capture` |
| **Scripted checks** | `run_scenario` — ordered steps, per-step expectations, `$steps[N].field` templating, structured pass/fail |
| **Record & replay** | `input_record` (passive tap on a real play session) · `input_replay` (replays that take back through the engine) — see the caveat in [TOOLS.md](TOOLS.md): a replayed key does not drive Enhanced Input actions, so on most UE5 games this reproduces menus and legacy input, not gameplay |
| **Ask the world** | `trace` (line/sphere/box/capsule, by channel or collision profile) · `ai_query` (behaviour tree **active node** and blackboard) · `material_query` (expressions, connections, **compile errors**) |
| **Blueprint repair** | `bp_find_broken` (every Blueprint that will not compile, grouped by the C++ change that broke it) · `bp_repair` (bulk *Refresh Node* + recompile, reporting what healed and what needs a decision) |
| **Blueprints** | `bp_create` · `bp_query` · `bp_add_component` · `bp_modify` (variables, function graphs, nodes, wiring, node properties — a whole event graph in one batched call, including Branch, Sequence, Cast, Switch, Select, Make/Break Struct and the standard macros: ForEachLoop, WhileLoop, Gate, FlipFlop…) · the declarative half too: event dispatchers, interfaces, and parent overrides · `bp_references` (what calls this function, reads this variable, binds this dispatcher — before you rename it) · `bp_compile` |
| **Content** | widgets · animation & cinematics · motion matching (GASP) · Niagara · PCG · landscape, lighting and foliage |
| **Editor & world** | actors (`level_actors`, `spawn_actor`, `spawn_batch` up to 1000, `delete_actors`, `move_actor`) · screenshots and camera · `edit_history` (undo stack) · `ui_tree` / `capture_widget` (any editor panel, even occluded) · `live_compile` |
| **Reflection** | `get_property` / `set_property` (dotted paths through structs *and* object references) · **`call_function`** (any UFUNCTION) · `class_info` · `find_functions` |
| **Assets & data** | `asset_create` (Widget Blueprints, Materials, Material Instances — anything the Add button makes, factory picked for you and named in the reply) · `asset_search` / `asset_dependencies` / `asset_referencers` / `asset_import` · `save` · DataTables · `run_tests` |

Every world-aware tool takes `world: "editor" | "pie"` and defaults to the live PIE world during play.

## Works with any MCP client

Uplink speaks plain MCP; nothing about it is Claude-specific. It is developed and tested against Claude Code. Other MCP clients (Cursor, Windsurf, Cline, Copilot agent mode, Gemini CLI, Codex CLI) speak the same protocol and should work, but they are not part of the test loop.

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

- **`Plugin/Uplink`** — a C++ editor plugin serving MCP natively over a loopback HTTP endpoint. One codebase compiles against **UE 5.7 and 5.8**; all version divergence lives in [`UplinkCompat.h`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkCompat.h). Two modules: **`UplinkEditor`** is the core — transport, registry, tasks, reflection, worlds, assets, Blueprints, play — and **`UplinkContentTools`** carries the tools with heavy dependencies (Niagara, PCG, materials, animation, landscape, sequencer, AI). The second one registers through `IUplinkToolProvider` exactly like a third-party plugin would, which is both how the core keeps those dependencies out of its own build and how that extension point stays honest: it has a real consumer rather than a worked example.
- **`bridge/`** — an optional Node stdio MCP server for clients that cannot speak HTTP. It stays alive when the editor is closed, so `status` answers "not connected" instead of the MCP server dying.

Every tool declares whether it only reads. Anything that writes runs in its own editor transaction unless it opted out, which is what makes its edits undoable by hand afterwards, and its parameters are checked against its published schema before it runs.

## See it work

```powershell
node demo\demo.js
```

Three acts against your own editor: read what the project is and where a player starts, author a moving platform from nothing — mesh, variables, a **Timeline** with its curve, and ten wires into `SetActorLocation` — then place it, play it, and measure that it moves. The Blueprint does not exist when the script starts, and if the platform does not move the script stops rather than printing a success it did not earn. [`demo/`](demo) has the details; `--clean` removes what it left behind.

## Checks

```powershell
.\scripts\ci.ps1
```

Three gates, cheapest first. `check_repo.ps1` is fifteen static checks that need no engine and finish in seconds — every tool schema parses, names are unique, every scenario step names a real tool and passes only parameters that tool declares, the docs list every tool, the version agrees in all four places that carry it. `build_all.ps1` compiles against both engines. `run_scenarios.ps1` runs the suite against a live editor. Each can be run on its own, and `ci.ps1` reports a stage that did not run as skipped rather than passed.

The static checks are the half a cloud machine can run, so [they run on every push](.github/workflows/checks.yml). Everything they cover fails silently by nature: nothing there breaks a build, which is exactly why it needs checking.

## Extending Uplink with your own tools

Other plugins can add project-specific tools without forking Uplink. Implement [`IUplinkToolProvider`](Plugin/Uplink/Source/UplinkEditor/Public/UplinkToolProvider.h), register it as a modular feature, and your tools appear alongside the built-ins, late-loading plugins included. The header has a complete example.

## Repository layout

```
Plugin/Uplink/     the UE editor plugin (C++)
bridge/            Node stdio MCP server (optional)
scripts/           build, project-linking, checking and scenario-running helpers (PowerShell)
scenarios/         runnable proof - a regression suite in the tool's own format
demo/              one script, three acts: understand a project, author a Timeline, prove it moves
CHANGELOG.md       what changed, and what changed behaviour rather than adding to it
TOOLS.md           full tool reference (parameters, conventions, security model)
PROMPTING.md       how to prompt an agent that is driving your editor
```

## License

[Apache-2.0](LICENSE) © 2026 Low Sze Hao
