# Uplink

**A direct data link between an AI agent and a running Unreal Engine editor — including the running game.**

Uplink is an Unreal Engine editor plugin plus a thin [MCP](https://modelcontextprotocol.io) bridge that lets an AI assistant (Claude Code, or any MCP client) do everything existing Unreal MCPs do — search assets, edit Blueprints, spawn and move actors, set properties, read logs, take screenshots — **and the thing none of them do: actually play the game.**

> Other Unreal MCPs *edit* your game. Uplink also *plays* it.

Start a Play-In-Editor session, drive the player, inject input, call any gameplay function, watch gameplay events fire, wait for conditions, and get a structured pass/fail answer to "does this level actually work?" — without a human touching the keyboard.

## Why

Every notable Unreal MCP today (including UE 5.8's official MCP plugin) is an *editor authoring* tool. An AI can build a level with them, but must then ask a human to test it. Uplink closes the loop the AI actually needs: **build → play → control → observe → assert → fix.**

## Status

🚧 **Early development.** The plugin skeleton (HTTP server on `127.0.0.1:3777` with `/status` and `/tools`) and the MCP stdio bridge are in place and compile against both UE 5.7 and 5.8. The tool registry, editor tools, and the PIE layer land next.

## Capability roadmap

| Layer | Tools (planned) | Phase |
|---|---|---|
| Meta | `status` · `console_command` · `output_log` · `viewport_screenshot` · task polling | 1 |
| Assets | `asset_search` · `asset_dependencies` · `asset_referencers` · `asset_open` | 1 |
| Blueprints | `bp_query` · `bp_modify` · `bp_compile` | 1 |
| Editor world | `level_actors` · `spawn_actor` · `delete_actors` · `move_actor` · `get/set_property` · `open_level` | 1 |
| **PIE lifecycle** | `pie_start` · `pie_stop` · `pie_status` · `pie_pause` · `pie_resume` · `pie_step` | 2 |
| **PIE control** | `input_action` (Enhanced Input injection) · `input_key` · `call_function` (any UFUNCTION, JSON in/out) · `possess` · `player_teleport` | 3 |
| **PIE observation** | `watch_events` / `drain_events` (delegate capture) · `wait_until` · `get_world_state` · `pie_screenshot` · `perf_stats` | 4 |
| Scenarios | `run_scenario` — scripted play/wait/assert sequences with structured reports | 5 |

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

3. **Install the bridge and register it with Claude Code:**

   ```powershell
   cd bridge; npm install
   claude mcp add uplink -- node "<repo>\bridge\index.js"
   ```

4. In Claude Code, ask for the `status` tool — with the editor running you'll get engine/project/PIE state; without it, a clean "not connected".

## Repository layout

```
Plugin/Uplink/     the UE editor plugin (C++)
bridge/            Node stdio MCP server
scripts/           build + project-linking helpers (PowerShell)
```

## License

[MIT](LICENSE) © 2026 Low Sze Hao
