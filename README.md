# Uplink

**A direct data link between an AI agent and a running Unreal Engine editor — including the running game.**

Uplink is an Unreal Engine editor plugin plus a thin [MCP](https://modelcontextprotocol.io) bridge that lets an AI assistant (Claude Code, or any MCP client) do everything existing Unreal MCPs do — search assets, edit Blueprints, spawn and move actors, set properties, read logs, take screenshots — **and the thing none of them do: actually play the game.**

> Other Unreal MCPs *edit* your game. Uplink also *plays* it.

Start a Play-In-Editor session, drive the player, inject input, call any gameplay function, watch gameplay events fire, wait for conditions, and get a structured pass/fail answer to "does this level actually work?" — without a human touching the keyboard.

## Why

Every notable Unreal MCP today (including UE 5.8's official MCP plugin) is an *editor authoring* tool. An AI can build a level with them, but must then ask a human to test it. Uplink closes the loop the AI actually needs: **build → play → control → observe → assert → fix.**

## Status

🚧 **Early development, already usable for editor work.** Compiles against both UE 5.7 and 5.8.

**Available now (36 tools):**

- **Play the game:** `pie_start` (waits until BeginPlay has actually run; viewport or new window, spawn/game-mode/map overrides) · `pie_stop` · `pie_status` · `pie_pause` / `pie_resume` · `pie_step` (advance a paused game exactly N frames)
- **Control the player:** `input_action` (Enhanced Input injection — pulse, timed hold, live value updates; no physical device needed) · `input_key` (raw key taps/edges/axis through the engine's simulated-input path) · `possess` · `player_teleport` · `player_info`
- **Observe & assert:** `watch_events` / `drain_events` / `unwatch` (record any BlueprintAssignable delegate firing, with decoded parameter payloads) · `wait_until` (non-blocking assertions: property values, actor existence, event counts, elapsed time) · `get_world_state` (actor snapshot with arbitrary property values) · `perf_stats`
- **Scripted playtests:** `run_scenario` — a whole playtest in one call: ordered tool steps with per-step expectations, timeouts, and a structured pass/fail report. Example: start PIE → teleport → injected-input walk → assert position → screenshot → stop, in a single request.
- **Editor & world:** `status` · `console_command` · `output_log` (incremental log reads — pair with `pie_status.log_start_index` to read just the current session) · `viewport_screenshot` (PNG image results) · `level_actors` · `spawn_actor` · `delete_actors` · `move_actor`
- **Reflection:** `get_property` / `set_property` (any UPROPERTY as JSON) · **`call_function`** (any UFUNCTION — args in, return value and out-params back as JSON)
- **Assets:** `asset_search` / `asset_dependencies` / `asset_referencers`
- **Tasks:** `task_status` / `task_result` / `task_cancel` / `task_list`

Every world-aware tool takes `world: "editor" | "pie"` and defaults to the live PIE world during play.

**Coming next:** Blueprint tools (`bp_query` · `bp_modify` · `bp_compile`).

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

**Security note:** the server binds to loopback only (`127.0.0.1` — your own machine; it is never reachable from the network), validates browser `Origin` headers, and refuses to start if the engine's HTTP listener has been reconfigured to a non-loopback address.

## Repository layout

```
Plugin/Uplink/     the UE editor plugin (C++)
bridge/            Node stdio MCP server
scripts/           build + project-linking helpers (PowerShell)
```

## License

[MIT](LICENSE) © 2026 Low Sze Hao
