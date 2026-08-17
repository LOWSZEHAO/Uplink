# Uplink tool reference

All 104 tools. Ordered by what you are trying to do: **play and test the game**, then **ask the world questions**, then **author content**, then **drive the editor**, and finally the **reflection escape hatch** that reaches everything without a dedicated tool.

Conventions used throughout:

- **`world`** — most world-touching tools accept `"world": "editor" | "pie"`. Omitted, they target the live PIE world when a session is running, else the editor world.
- **Actors** are addressed by exact name, exact editor label, or label substring (first match). When a lookup fails, the error lists what is actually there.
- **Vectors** are `{x,y,z}` objects; **rotators** are `{pitch,yaw,roll}`.
- **Unknown parameters are rejected**, with the accepted list and a suggestion when one is close. A misspelt name never silently falls back to a default.
- **Every mutating tool runs inside its own editor transaction**, named after the tool, so an agent's edits undo by hand exactly like your own — see `edit_history`.
- **Lists are capped.** Tools that return collections take `max` and report `total` plus `truncated`, so a shortened list is never mistaken for the whole set.
- **Long-running calls** — pass `wait_ms` (REST) to bound how long the response waits; if the work is still running you get `{task_id, status:"running"}` back — poll with `task_status`, fetch with `task_result`.

---

# Understanding a project nobody explained

Start here on an unfamiliar project. Each of these exists because its absence
cost real time driving a real game.

| Tool | What it answers |
|---|---|
| `project_entry` | How do I start this game the way a player does? Default map, editor startup map, global game mode, the level's own override, and every map in the project. **Read this first.** The map the editor opens is often not the map a player starts from — on one real game the editor opened the gameplay level while the game began at a menu, and starting play on the gameplay map gave 40 actors and a black screen where entering through the menu gave 1917 and a playable game. `{max_maps?}` |
| `ui_live` | What is on screen right now: every widget in every live UserWidget, with class, displayed text, screen rect, visibility and whether it is interactive. This is how you find a menu without reading its Blueprint — and the names are what `click_widget` takes. A zero rect means the widget exists but its screen has not been shown. `{contains?, interactive_only?, on_screen_only?, max?, world?}` PIE only. |
| `input_map` | What can I press, and what does it do? Every Enhanced Input mapping context with its actions and bound keys, plus which contexts are actually applied to the live player. Beats searching assets for something called `IA_Move`. `{path_prefix?, applied_only?, max?}` |
| `actor_components` | A live actor's components under the names `get_property` and `set_property` expect. Guessing fails quietly: a character's movement component is `CharMoveComp`, not `CharacterMovement`. `{actor, class_contains?, max?, world?}` |
| `streaming_status` | Which sublevels are loaded, visible, or still coming, with the world's actor count. Without it, an unfinished stream looks identical to a broken level or an empty one. `{world?}` |
| `frame_strip` | What changed over the last few seconds — N frames at a fixed interval returned as ONE contact sheet. Use it for transitions, fades, animations, whether a door actually opened: a single screenshot is one instant, and everything between calls is invisible. Includes the UI layer during play. `{frames?, interval_ms?, columns?, scale?}` |
| `dialog_state` | Is a modal window blocking the editor, and what does it say? Every tool runs on the game thread, so a modal freezes all of them — calls simply hang with no diagnostic. Ask this first when Uplink starts answering again. `dismiss:true` closes it, which answers any question it was asking, so read the title first. `{dismiss?}` |

> **The pattern that works on an unfamiliar game:** `project_entry` to find the real
> entry map → `pie_start` on that map → `ui_live` to see what is on screen →
> `input_map` for the controls → drive it → `streaming_status` to confirm the world
> actually loaded → `frame_strip` when something is meant to be changing.

---

# Running the game

Start a session, drive the player, watch what happens, and assert on it. This is
for checking your own work — walking a level to see whether the collision holds,
confirming an event fires, catching a regression. It will not navigate an
arbitrary game's menus for you: a menu that handles input through its own widget
logic may need `input_key`'s `ui` route, and some will need driving through their
own Blueprint events.

## PIE lifecycle

| Tool | What it does |
|---|---|
| `pie_start` | Start Play-In-Editor; resolves only after BeginPlay has run. `{mode: viewport\|window, location?, rotation?, game_mode?, map?}` → map + `log_start_index` (feed to `output_log.since_index` for session-only logs). |
| `pie_stop` | End the session; resolves on full shutdown. |
| `pie_status` | `state` (none/starting/running/stopping), paused, map, elapsed, `log_start_index`. |
| `pie_pause` / `pie_resume` | Freeze / unfreeze the game world. |
| `pie_step` | Advance a **paused** session exactly N frames. `{frames?}` |

## Player control (PIE only)

| Tool | What it does |
|---|---|
| `input_action` | Enhanced Input injection at the action level — works with zero physical devices. `{action: <UInputAction path>, value: bool\|number\|{x,y,z}, mode: pulse\|hold\|update\|release, duration?}`. `hold`+`duration` auto-releases. The reply carries `boundHandlers`: injection succeeds identically whether or not the game listens, so a zero there explains a "nothing happened". |
| `input_key` | Raw key in the running game. `{key, event: tap\|pressed\|released\|axis, route?: game\|ui\|both, amount?, duration?}`. **`route` decides who receives it**: `game` (default) goes to the player controller, which is what gameplay input binds to; `ui` sends a real Slate key event to the focused widget, which is the **only** thing a UMG menu overriding `OnKeyDown` will ever see — if a title screen ignores every key you send, this is why; `both` does each, like a real keypress. The reply reports whether anything handled it. |
| `possess` | Switch the player controller to another pawn. `{pawn}` — failure names the pawns that are actually in the running game. |
| `player_teleport` | Physics-safe pawn teleport + optional facing. `{location, rotation?}` |
| `player_info` | Controller, pawn (name/class/location), control rotation. |
| `navigate_to` | Walk the player pawn to a location or actor using the game's navmesh — like a click-to-move player. Resolves on arrival within `accept_radius`; reports a stall instead of hanging when there is no path (usually a missing NavMeshBoundsVolume). `{location \| actor, accept_radius?}` |
| `click_widget` | Click a UMG widget in the running game — menus, buttons, HUD — by synthesizing a real mouse press at the widget's screen position (real hit-testing, like a player's click). `{widget: name (exact, then contains) \| position: {x,y}, world?}`. Reports `handled`: an unhandled click usually means something invisible is over the target. Failure lists the live widget names. |

## Observation & assertion

| Tool | What it does |
|---|---|
| `watch_events` | Record broadcasts of any dynamic multicast delegate (BlueprintAssignable events), with decoded parameter payloads. `{actor/object_path/component, delegate: <name>\|"*", world?}` → `watch_id`. Watches stop automatically when PIE ends. |
| `drain_events` | Read captured events oldest-first. `{since_seq?, watch_id?, max?}` → events + `next_seq`. |
| `unwatch` | `{watch_id}` or `{all:true}`. |
| `wait_until` | Non-blocking assertion. `{condition:{type: property_equals\|actor_exists\|actor_gone\|event_count\|elapsed, ...}, timeout?, world?}` → `{condition_met, timed_out, waited_seconds}` — a timeout is a result, not an error. |
| `get_world_state` | Actor snapshot with requested property values inline. `{world?, class_contains?, name_contains?, properties?:[...], max?}` |
| `viewport_annotate` | Screenshot the running game **and** report where each matching actor is on screen — name, class, screen-space rect `[x,y,w,h]`, centre and distance — so a claim about what is visible is grounded in coordinates instead of pixel guessing. Off-screen matches are listed with `on_screen:false`. PIE only (it uses the player's camera). `include_image:false` skips the PNG for a cheap positions-only read. `{class_contains?, name_contains?, max?, include_image?}` |
| `perf_stats` | Smoothed FPS, average frame ms, last delta, used physical memory. |
| `profile_capture` | Sample frame times for N seconds: avg FPS/ms, p50/p95/p99, worst frame, hitch counts (>33ms, >100ms) — the frame-budget truth over time. `{seconds?}` |

## Scripted playtests

| Tool | What it does |
|---|---|
| `run_scenario` | Ordered tool steps executed as one task with a structured pass/fail report. `{steps:[{tool, params?, expect?, timeout?}], stop_on_failure?}`. `expect` matches fields of the step's result data; a `wait_until` step whose condition times out fails the scenario unless explicitly expected. |

## Record & replay

| Tool | What it does |
|---|---|
| `input_record` | Capture the human's real input (keys, mouse buttons, axes, mouse moves) through a passive Slate tap — play is unaffected. `{action: start\|stop\|status}`; `stop` returns the timestamped events and keeps them as the last take. Auto-stops when PIE ends. |
| `input_replay` | Replay a take into the running game through the engine's simulated-input path — a regression test from a real play session. `{events? (from a stop; omit = last take), speed?}` |

---

# Asking the world questions

## World queries

| Tool | What it does |
|---|---|
| `trace` | Ask the physics scene what is there. `{from, to? \| direction?+distance?, shape?: line\|sphere\|box\|capsule, radius?, half_height?, channel?, profile?, multi?, complex?, ignore_actors?, draw_seconds?, world?}`. Hits report actor, label, class, component, impact point, normal, distance, physical material and bone. Trace **by profile** to check the collision setup a project actually uses. A downward trace is how you find ground height — do not parse heightmaps for it. |
| `ai_query` | What the AI is doing: per controller, its pawn, behaviour tree, **active node**, active task chain and full blackboard. `{actor?, blackboard?, max?, world?}`. The active node is not exposed to Blueprint, so nothing else can answer "why is it doing that". A non-behaviour-tree brain (StateTree, custom) is named rather than hidden. AI only exists in a running game. |
| `material_query` | Read a material or material instance: `errors` first (compile errors, taken from the compiled resource — a failed material renders black and logs nothing useful), then expressions, which output inputs are connected, and parameter values. `{material, expressions?, parameters?, recompile?, max?}`. An instance also reports its `parent`; a null parent renders as default material and looks exactly like a broken graph. |

---

# Authoring

## Blueprints

| Tool | What it does |
|---|---|
| `bp_create` | New Blueprint asset. It exists in memory and is marked dirty — call `save`, or an editor restart discards it. `{path, parent_class?}` (a full class path, e.g. `/Script/Engine.Pawn`; defaults to Actor). |
| `bp_query` | Parent class, compile status, variables, and per-graph nodes with pins/defaults/connections. Node guids are the handles `bp_modify` uses. `{blueprint, graph?, max_nodes?}` |
| `bp_modify` | One edit — or a whole batch: `{blueprint, ops: [{op:..., ...}, ...], compile?, save?}` builds an entire event graph in a single call. Give `add_node` ops a `ref` name and later ops address that node as `@ref` in `from_node`/`to_node`/`node`. A failed op stops the batch and reports `failedAt` and `applied`, since earlier ops stay applied. `save` writes the blueprint to disk, and is skipped when `compile` reported errors so a broken asset is not what gets persisted. |
| `bp_add_component` | Add a component to an actor Blueprint's construction script, like the editor's Add Component button. `class` is a short engine name (`StaticMeshComponent`, `BoxComponent`) or a full path (needed for anything outside the common set, e.g. `/Script/HeadMountedDisplay.MotionControllerComponent`); `parent` attaches under an existing component. Template conveniences: `location`/`rotation`/`scale`, `static_mesh`, `collision_profile`, and `properties` as a generic name→JSON map. The component becomes a Blueprint variable — after a compile its delegates bind with `bp_modify component_bound_event`. `{blueprint, class, name, parent?, …, compile?}` |
| `bp_compile` | Compile and return errors/warnings/messages. `{blueprint}` |
| `bp_find_broken` | Every Blueprint that does not compile, **grouped by what broke it**. The tool for a project that stopped working after a C++ change: one renamed function can break dozens of assets and produce hundreds of error lines that all say the same thing. On a real project it found 79 broken assets and traced 60 of them to a single missing function. `{path_prefix?, max?, max_assets_per_cause?, include_warnings?}` Compiling is real work — raise `max` deliberately. |
| `bp_repair` | Apply the editor's own repair in bulk: replace deprecated nodes, refresh every node against its current signature, recompile, report what healed. This is right-click *Refresh Node* across every affected asset, which is the real fix for a stale pin or a node whose function changed shape. It cannot invent a function that no longer exists — those come back in `stillBroken` with the reason and need a decision. `{blueprints?, path_prefix?, max?, save?, dry_run?}` Only assets that compile clean are saved. |

`bp_modify` operations (single, or batched through `ops` with `@ref` handles):

| Op | What it does |
|---|---|
| `add_variable` / `remove_variable` | Member variables by friendly type string: `bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,struct:<path>,array:<inner>`. Removing a name the blueprint does not have is refused, with the real names listed. |
| `add_function` / `remove_function` | Create or delete a real **function graph** — not just nodes in the event graph. `{name, thread_safe?, pure?, category?, inputs:[{name, type, by_ref?, const?}]}`. `thread_safe` is required for anim-graph node functions, which cannot run on the game thread; `by_ref`+`const` are required to match a prototype-validated signature such as an anim node binding, which declares its parameters const-reference. |
| `add_node` | `call_function` `{class (default self), function}` · `custom_event` `{name}` · `event` `{name}` (e.g. `ReceiveBeginPlay`; reuses a matching ghost/existing node rather than stacking a duplicate) · `component_bound_event` `{component, event}` — bind a component's or widget's delegate as a graph event (button `OnClicked`, `NiagaraComponent` `OnSystemFinished`) · `variable_get` / `variable_set` `{name}`. Plus `graph?, x?, y?, ref?`. |
| `connect` / `break_links` / `delete_node` | Wiring, with schema rejection reasons on failure. Exec pins are `execute` / `then`. |
| `set_pin_default` | Literal pin values, object-aware. Verified after writing: a value the pin's type will not take is refused rather than silently dropped. |
| `set_node_property` | Set any property on a **graph node** addressed by guid, with dotted paths into structs — this is how an anim graph node's function binding is set. `{graph, node, property, value, reconstruct?}` |
| `arrange` | Lay the graph out: dependency columns, straight exec lanes, reroute knots at turns. |

**Graph style rules** (always on for Uplink-authored graphs): nodes never overlap — new nodes are nudged into free space, or auto-placed on a fresh row when `x`/`y` are omitted; `arrange` lays a whole graph out as left-to-right dependency columns with each exec chain on one horizontal lane (level pins = straight wires) and pure data nodes tucked below the lane they feed. Wires that must change height get a **reroute knot** at the turn — the wire leaves its pin dead level, hits the dot just before the target, and the engine's own spline makes the drop — the same way a person tidies a graph by hand (`arrange` re-places its knots on every run). Wire rendering itself is stock Unreal. For material graphs, run `MaterialEditingLibrary.LayoutMaterialExpressions` after authoring.

## Widgets

| Tool | What it does |
|---|---|
| `widget_tree` | A Widget Blueprint's hierarchy: name, class, parent, root flag, and `is_variable` (only variables can have events bound). `{blueprint, max?}` |
| `widget_add` | Construct a widget into the tree, marked as a variable so its events are immediately bindable with `bp_modify component_bound_event`. `{blueprint, class, name, parent?}` |

## Animation & cinematics

| Tool | What it does |
|---|---|
| `anim_query` | Montage/sequence timing truth: play length, frame rate + frame count (sequences), montage sections with times, and every notify with exact trigger time, duration, track and class. `{asset}` |
| `anim_modify` | `add_notify` `{name, time \| frame, track?, notify_class?}` — a name-only notify becomes a skeleton notify (fires `AnimNotify_<Name>` / montage `OnNotifyBegin`); `remove_notify` `{name \| index}`. Assets are marked dirty, not saved. |
| `animbp_query` | Anim Blueprint structure: target skeleton, state machines (states + transitions), and every anim graph node with its title (which names the assets it plays). State machines come back complete; the flat node list is capped. `{blueprint, max?}` — pair with `bp_query` for the event graph. |
| `skeleton_query` | Bone hierarchy (name/parent/index, optional ref-pose transforms) and sockets of a Skeleton or SkeletalMesh. `{asset, transforms?, bone_contains?}` |
| `socket_modify` | Add / update / remove skeleton sockets — attachment points for weapons and props. `{asset, op, name, bone?, location?, rotation?, scale?}` |
| `sequence_query` | Level Sequence timing truth: playback range and frame rate, bindings (possessable/spawnable) with tracks, and section start/end in seconds. Playback needs no tool: `call_function` `LevelSequencePlayer.CreateLevelSequencePlayer` + `Play`. `{asset, max?}` |

## Motion matching (GASP)

Requires the **PoseSearch** and **Chooser** plugins (both off by default; the Game Animation Sample enables them). Reflection-only, so UplinkEditor still loads without them — the tools just report which plugin to enable. Note that `animbp_query` shows very little for motion-matching graphs: motion matching is precisely what *replaces* state machines, so use these instead.

| Tool | What it does |
|---|---|
| `posesearch_query` | A Pose Search database: its schema (skeleton, sample rate, feature channels) and every animation it can select from. `{database, max_animations?}` |
| `chooser_query` | A Chooser table — the data-driven logic picking which database/asset applies to the current game state. Dumps rows, columns and result structs, including nested choosers. `{chooser, max_rows?}` |
| `motionmatch_debug` | Live motion-matching state off a character during PIE: each motion-matching anim node with its blend/reselect settings, plus the anim blueprint's exposed values (in GASP: MovementMode, RotationMode, MovementState, Gait — the inputs the chooser keys on). `{actor?, world?, max_values?}`, defaults to the player pawn. |

## Niagara (authoring: UE 5.8+ · user parameters: 5.7+)

Built on Epic's `UNiagaraExternalEditUtilities` (new in 5.8, marked experimental by Epic — expect churn between engine versions). On 5.7 the stack-authoring tools report unsupported.

| Tool | What it does |
|---|---|
| `niagara_create` | New Niagara System asset, optionally from a template system and/or with an emitter from a template (engine templates like `/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain` are always mounted). `{path, template_system?, emitter_template?, emitter_name?}` |
| `niagara_query` | System summary (emitters, renderers), compile state per script, and stack issues **with Epic's own error/fix descriptions** — read this after edits. `{asset}` |
| `niagara_add_module` | Add a module script to a stack (e.g. `/Niagara/Modules/Update/Forces/VortexForce.VortexForce` into `ParticleUpdateScript`). Pass `module` to insert after an existing module — **placement matters**: force modules must precede `SolveForcesAndVelocity`. Returns the module's input topology. `{asset, emitter?, script?, module_asset, module?}` |
| `niagara_module_inputs` | A module's inputs with names, types, values and editability — inputs hidden by static switches are flagged and write-protected. `{asset, emitter?, script?, module}` |
| `niagara_set_input` | Set a module input's local value (`float,int,bool,vec3,color`); array-form `input` addresses nested dynamic-input chains. `{asset, emitter?, script?, module, input, type, value}` |
| `niagara_remove_module` | Remove a module from a stack. `{asset, emitter?, script?, module}` |
| `niagara_renderer` | Read or write a renderer's properties as JSON — most usefully the sprite material: `{"Material":{"refPath":"/Game/FX/M_Dust.M_Dust"}}`. Partial updates merge over current values. `{asset, emitter, renderer_index?, properties?}` |
| `niagara_set_user_param` | Set an exposed User parameter default on the asset (works on 5.7 too). `{asset, name, type, value}` |
| `niagara_compile` | Compile + wait + report state and `ready_to_run`. Forced by default — newly added emitters only receive compiled data on a forced pass. `{asset, force?}` |

Instance-level control needs no dedicated tools: `call_function` reaches `NiagaraFunctionLibrary.SpawnSystemAtLocation` and every `NiagaraComponent.SetVariable*`; `watch_events` binds `OnSystemFinished`. Created and edited assets are dirty in memory — persist them with `save`.

## PCG (procedural generation)

Requires the PCG plugin. It is **off by default in UE 5.7** and on in 5.8 — `plugin_enable {"name":"PCG"}` then restart. These tools work purely by reflection, so UplinkEditor still loads when PCG is absent; the tools just report that it is not enabled.

| Tool | What it does |
|---|---|
| `pcg_create` | Create an empty PCG graph asset. `{path, name, save?}` |
| `pcg_add_node` | Add a node. `settings_class` takes a friendly name (`SurfaceSampler`, `StaticMeshSpawner`, `GetLandscape`, `TransformPoints`) or a full `/Script/PCG.PCGxxxSettings` path; unknown names come back with close matches. `properties` are applied to the node settings. Returns the node name and its exact input/output pin labels. `{graph, settings_class, properties?, title?}` |
| `pcg_connect` | Wire two nodes, or unwire with `disconnect: true`. `{graph, from_node, from_pin?, to_node, to_pin?, disconnect?}` |
| `pcg_query` | Every node with settings class, title and pin labels — read this before wiring. `{graph, max?}` |
| `pcg_generate` | Run a graph on an actor: adds a PCG component if it has none, assigns the graph, and generates — bounds come from the actor. Pair with `spawn_actor {"class_path": "/Script/PCG.PCGVolume"}` to get a bounded volume to populate. `cleanup: true` clears previously generated content instead. `{actor, graph?, cleanup?, force?, world?}` |

## Environment

| Tool | What it does |
|---|---|
| `lighting_setup` | One-call scene lighting: ensures the standard stack exists (sun, sky light, sky atmosphere, height fog, volumetric clouds, unbound post-process volume) and applies your per-actor settings JSON. The style knowledge is the caller's; this is the atomic apply. |
| `landscape_create` | Heightmap file (8/16-bit grayscale PNG or raw .r16, 32768 = zero) → a real Landscape actor, resampled to a valid layout. Generate the heightmap or use real DEM data. `{heightmap_file, location?, scale?, material?}` |
| `foliage_scatter` | Scatter N instances of a mesh over a circle, each traced down onto the ground — one instanced-mesh actor. `{mesh, count, center, radius, min_scale?, max_scale?, seed?}` |

---

# The editor itself

## Editor world

| Tool | What it does |
|---|---|
| `level_actors` | List actors with name/label/class/location. `{world?, class_contains?, name_contains?, max?}` |
| `spawn_actor` | Spawn by class path (`/Script/Engine.PointLight` or `/Game/BP_X.BP_X_C`). `{class_path, location?, rotation?, label?, world?}` |
| `spawn_batch` | Spawn up to 1000 actors in one call — scene assembly at scale. Each entry is `{mesh \| class_path, location, rotation?, scale?, label?, material?}`; `mesh` spawns a movable StaticMeshActor with optional material override. Fails fast with the failing index. `{actors:[...], world?}` |
| `delete_actors` | Destroy actors by name/label. `{names:[...], world?}` |
| `move_actor` | Set location/rotation/scale (any subset), physics-safe in PIE. `{actor, location?, rotation?, scale?, world?}` |
| `viewport_screenshot` | PNG of the active viewport — the game viewport during PIE, else the editor viewport, which is **redrawn first** so a window that is not in front never returns a stale frame. `{refresh?}`. Returned as an MCP image block (HTTP: `image_base64`). |
| `viewport_camera` | Move the editor viewport camera: `focus_actor` frames an actor (like pressing F), or set `location`/`rotation` directly — pair with `viewport_screenshot`. `{focus_actor?, location?, rotation?}` |
| `edit_history` | Inspect and walk the undo stack. `{action: list\|undo\|redo, steps?}`. Every mutating tool runs as its own transaction named `Uplink: <tool>`, so an agent's edit rolls back exactly like a hand edit; `undo`/`redo` report the transactions they actually walked, not a count. PIE-world edits are not transacted — the engine does not record them. |
| `live_compile` | Trigger a Live Coding compile and patch the RUNNING editor with changed C++ — no restart. Function-body edits land in seconds; structural changes (new classes/members/virtuals) still need a real build. Resolves when the compiler goes idle; `patched: true` = new code is live. |

## Editor UI (Slate)

See the editor itself — every window and panel, not just viewports. `ui_tree` is the map, `capture_widget` is the camera.

| Tool | What it does |
|---|---|
| `ui_tree` | Query the live Slate widget hierarchy. Default: structure of the main window to `max_depth`. `window` picks a window by title substring; `find` searches every window, full depth, for widgets whose type or text contains the string (`find:"NiagaraSystemViewport"`, `find:"Content Browser"`). Rows carry a path (`w0/1/0/3`), type, label text where the widget has one, and `rect` `[x,y,w,h]` in desktop pixels. `{window?, find?, path?, max_depth?, max_nodes?}` |
| `capture_widget` | Screenshot any editor window or single widget as a PNG — asset-editor previews, graph panels, details panels — even when the window is behind others. Target by `path` (from `ui_tree`) or `type` (first descendant type-name match, e.g. `SNiagaraSystemViewport`, `SGraphEditor`); with neither, the whole window. `{window?, path?, type?}` |

## Assets

> `save` matters more than it looks: authoring tools mark packages dirty and leave the work in memory. Anything unsaved is lost when the editor closes.

| Tool | What it does |
|---|---|
| `asset_search` | Name-substring search. `{query?, class_contains?, path_prefix? (default /Game), max?}` — plugin content needs its mount point as `path_prefix` (e.g. `/MyPlugin`). |
| `asset_dependencies` / `asset_referencers` | Package-level dependency graph, capped. `{package, max?}` → the list plus `total` and `truncated`. |
| `asset_create` | Create an empty asset of any class the content browser's Add button offers — Widget Blueprints, Materials, Material Instances, Data Tables, Curves — which is everything `bp_create` cannot make. The factory is chosen the way the Add menu would and reported back with any runners-up, so a surprising pick is visible rather than silent; `factory` overrides it. `parent_class` sets the base class for Blueprint-shaped factories, `properties` configures the factory itself (`{"InitialParent": "/Game/M_Glass.M_Glass"}` for a material instance). Blueprint results are compiled before returning, so `Path.Path_C` resolves. Never prompts. `{path, class, parent_class?, factory?, properties?, save?}` |
| `asset_import` | Import a disk file into the project — FBX/OBJ, textures, audio, anything the editor imports — fully automated, no dialogs. `{file, destination, name?, save?}` |
| `save` | Write edited assets to disk. No arguments saves everything dirty including the level; `asset` saves one package by path; `list_only` reports what is unsaved without writing. Never prompts — these tools run unattended, and a modal dialog would hang the editor. `{asset?, list_only?, include_level?}` |

## Tests & data

| Tool | What it does |
|---|---|
| `run_tests` | Run engine/project automation tests whose name contains `filter`, sequentially, with per-test pass/fail, errors and durations. Be specific — some editor tests open maps or take minutes. `{filter, max?}` |
| `datatable_create` | New DataTable for a row struct. `{path, row_struct}` |
| `datatable_query` | Row struct, columns, and rows as JSON. `{asset, row?, max?}` |
| `datatable_modify` | `add_row` `{row, values?}` · `update_row` `{row, values}` · `remove_row` `{row}` · `rename_row` `{row, new_name}` — `values` maps columns to JSON. |

## Project & session

| Tool | What it does |
|---|---|
| `status` | Engine version, project, current map, PIE active? |
| `console_command` | Run any console command (`stat fps`, `open Map`, …) and return its captured output. `{command, world?}` |
| `output_log` | Read recent log lines from an in-memory ring buffer. `{since_index?, max?, category?, contains?, verbosity?}` → lines + `next_index` (pass back as `since_index` for incremental reads). |
| `plugin_list` | Engine + project plugins with enabled state and content roots. Enabled plugins are already fully reachable (assets via `path_prefix`, classes via reflection). |
| `plugin_enable` | Enable/disable a plugin in the .uproject (editor restart required to take effect). `{name, enable}` |
| `task_status` / `task_result` / `task_cancel` / `task_list` | Manage long-running tool calls. Results are retained ~3 minutes. |

---

# Reflection — reaching everything else

## Reading and writing anything

| Tool | What it does |
|---|---|
| `get_property` | Read any UPROPERTY as JSON. Target via `object_path`, or `actor` + optional `component`. `{..., property, world?}` |
| `set_property` | Write any UPROPERTY from JSON; in the editor world also runs `PostEditChangeProperty`, so the editor reacts like a Details-panel edit. `{..., property, value, world?}` |
| `call_function` | Call any UFUNCTION by reflection: `args` maps parameter names to JSON values; the response carries the return value and out-params. `{..., function, args?, world?}` |

Both property tools take a **dotted path** that steps through structs *and* object references, so a path can cross from an actor into a component and on into that component's structs: `RootComponent.RelativeLocation.X`. A dead end names the class it stopped at, and a null link says which segment was null. (Dotted paths are also the way to read the few engine structs that refuse to serialise as a whole.)

**Named object references are checked.** A path that resolves to nothing is refused rather than written as null — that failure mode once produced a material with no parent that rendered black with nothing in the logs. Unknown argument names are rejected with the expected parameter list.

## Discovery

Two tools turn the engine's whole reflected surface into something searchable, so systems without a dedicated tool are still reachable:

| Tool | What it does |
|---|---|
| `class_info` | A class's properties and functions with types and flags — what you can set and call on it. |
| `find_functions` | Search callable functions across loaded classes by name substring, with their signatures. Also useful for checking what a Blueprint function actually compiled to. |

**Worked pattern — anything with an editor scripting library:**

```json
// 1. find it:
{ "query": "CreateMaterialExpression" }

// 2. call it:
{ "object_path": "/Script/MaterialEditor.Default__MaterialEditingLibrary",
  "function": "CreateMaterialExpression",
  "args": { "Material": "/Game/M_Thing.M_Thing",
            "ExpressionClass": "/Script/Engine.MaterialExpressionMultiply",
            "NodePosX": -400 } }
```

Object and class arguments accept asset/class paths as strings. The same pattern reaches `EditorAssetLibrary`, `AnimationLibrary`, `WidgetBlueprintLibrary`, and every other scripting library in the engine or your project.

**Subsystems**: `object_path` accepts `subsystem:<Class>` to resolve live subsystem instances, whose real object paths are unguessable. Editor and engine subsystems resolve anytime; game-instance/world/local-player subsystems resolve against the chosen world (`world:"pie"` during play). Example — open any asset's editor: `call_function {object_path:"subsystem:AssetEditorSubsystem", function:"OpenEditorForAssets", args:{Assets:["/Game/Path/Asset"]}}` — then `capture_widget` can screenshot the editor it opened. Note the CDO guard: instance methods must be called on instances like these, never through `Default__` paths, which would run without a valid instance and can take the editor down.

**Worked recipe — material graph authoring** (every call verified):

```json
// new material:          asset_create {path, class:"Material"}   (or DuplicateAsset to start from an existing one)
// duplicate a base:      EditorAssetLibrary.DuplicateAsset {SourceAssetPath, DestinationAssetPath}
// add an expression:     MaterialEditingLibrary.CreateMaterialExpression {Material, ExpressionClass, NodePosX, NodePosY}
// wire two expressions:  MaterialEditingLibrary.ConnectMaterialExpressions {FromExpression, FromOutputName, ToExpression, ToInputName}
// wire to an output pin: MaterialEditingLibrary.ConnectMaterialProperty {FromExpression, FromOutputName, Property (e.g. MP_BaseColor)}
// tidy + rebuild:        MaterialEditingLibrary.LayoutMaterialExpressions {Material} · RecompileMaterial {Material}
```

Expression object paths come back from each create call; set their fields (e.g. a Constant3Vector's `Constant`) with `set_property`, and check the result with `material_query`. Set the material's own `MaterialDomain` / `BlendMode` / `ShadingModel` with `set_property` before wiring — a post-process material rejects the SceneColor node, and a Surface one rejects `SceneTexture:PostProcessInput0`, so the domain decides which nodes are legal.

---

## Security model

The HTTP server binds loopback only (never network-reachable), refuses to start if the engine's HTTP listener has been reconfigured to a non-loopback address or the port is taken by another process, validates browser `Origin` headers on every route, and caps request bodies at 2 MB.
