# Uplink tool reference

All 89 tools, grouped by layer. Conventions used throughout:

- **`world`** — most world-touching tools accept `"world": "editor" | "pie"`. Omitted, they target the live PIE world when a session is running, else the editor world.
- **Actors** are addressed by exact name, exact editor label, or label substring (first match).
- **Vectors** are `{x,y,z}` objects; **rotators** are `{pitch,yaw,roll}`.
- **Long-running calls** — pass `wait_ms` (REST) to bound how long the response waits; if the work is still running you get `{task_id, status:"running"}` back — poll with `task_status`, fetch with `task_result`.

## Meta

| Tool | What it does |
|---|---|
| `status` | Engine version, project, current map, PIE active? |
| `console_command` | Run any console command (`stat fps`, `open Map`, …) and return its captured output. `{command, world?}` |
| `output_log` | Read recent log lines from an in-memory ring buffer. `{since_index?, max?, category?, contains?, verbosity?}` → lines + `next_index` (pass back as `since_index` for incremental reads). |
| `viewport_screenshot` | PNG of the active viewport — the game viewport during PIE, else the editor viewport. Returned as an MCP image block (HTTP: `image_base64`). |
| `task_status` / `task_result` / `task_cancel` / `task_list` | Manage long-running tool calls. Results are retained ~3 minutes. |

## Editor world

| Tool | What it does |
|---|---|
| `level_actors` | List actors with name/label/class/location. `{world?, class_contains?, name_contains?, max?}` |
| `spawn_actor` | Spawn by class path (`/Script/Engine.PointLight` or `/Game/BP_X.BP_X_C`). `{class_path, location?, rotation?, label?, world?}` |
| `spawn_batch` | Spawn up to 1000 actors in one call — scene assembly at scale. Each entry is `{mesh \| class_path, location, rotation?, scale?, label?, material?}`; `mesh` spawns a movable StaticMeshActor with optional material override. Fails fast with the failing index. `{actors:[...], world?}` |
| `delete_actors` | Destroy actors by name/label. `{names:[...], world?}` |
| `move_actor` | Set location/rotation/scale (any subset), physics-safe in PIE. `{actor, location?, rotation?, scale?, world?}` |
| `viewport_camera` | Move the editor viewport camera: `focus_actor` frames an actor (like pressing F), or set `location`/`rotation` directly — pair with `viewport_screenshot`. `{focus_actor?, location?, rotation?}` |
| `live_compile` | Trigger a Live Coding compile and patch the RUNNING editor with changed C++ — no restart. Function-body edits land in seconds; structural changes (new classes/members/virtuals) still need a real build. Resolves when the compiler goes idle; `patched: true` = new code is live. |

## Reflection

| Tool | What it does |
|---|---|
| `get_property` | Read any UPROPERTY as JSON. Target via `object_path`, or `actor` + optional `component`. `property` accepts a **dotted path** (`MyStruct.Inner.Value`) to read struct members — necessary for the few engine structs that will not serialise as a whole. `{..., property, world?}` |
| `set_property` | Write any UPROPERTY from JSON; in the editor world also runs `PostEditChangeProperty`. `{..., property, value, world?}` |
| `call_function` | Call any UFUNCTION by reflection: `args` maps parameter names to JSON values; the response carries the return value and out-params. Unknown arg names are rejected with the expected parameter list. `{..., function, args?, world?}` |

## Assets

> `save` matters more than it looks: authoring tools mark packages dirty and leave the work in memory. Anything unsaved is lost when the editor closes.

| Tool | What it does |
|---|---|
| `asset_search` | Name-substring search. `{query?, class_contains?, path_prefix? (default /Game), max?}` — plugin content needs its mount point as `path_prefix` (e.g. `/MyPlugin`). |
| `asset_dependencies` / `asset_referencers` | Package-level dependency graph. `{package}` |
| `asset_import` | Import a disk file into the project — FBX/OBJ, textures, audio, anything the editor imports — fully automated, no dialogs. `{file, destination, name?, save?}` |

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
| `input_action` | Enhanced Input injection at the action level — works with zero physical devices. `{action: <UInputAction path>, value: bool\|number\|{x,y,z}, mode: pulse\|hold\|update\|release, duration?}`. `hold`+`duration` auto-releases. |
| `input_key` | Raw key on the player controller via the engine's simulated-input path. `{key: W\|SpaceBar\|Gamepad_LeftX\|..., event: tap\|pressed\|released\|axis, amount?, duration?}` |
| `possess` | Switch the player controller to another pawn. `{pawn}` |
| `player_teleport` | Physics-safe pawn teleport + optional facing. `{location, rotation?}` |
| `player_info` | Controller, pawn (name/class/location), control rotation. |
| `navigate_to` | Walk the player pawn to a location or actor using the game's navmesh — like a click-to-move player. Resolves on arrival within `accept_radius`; reports a stall instead of hanging when there is no path (usually a missing NavMeshBoundsVolume). `{location | actor, accept_radius?}` |
| `click_widget` | Click a UMG widget in the running game — menus, buttons, HUD — by synthesizing a real mouse press at the widget's screen position (real hit-testing, like a player's click). `{widget: name (exact, then contains) \| position: {x,y}, world?}` — failure lists the live widget names. |

## Record & replay

| Tool | What it does |
|---|---|
| `input_record` | Capture the human's real input (keys, mouse buttons, axes, mouse moves) through a passive Slate tap — play is unaffected. `{action: start\|stop\|status}`; `stop` returns the timestamped events and keeps them as the last take. Auto-stops when PIE ends. |
| `input_replay` | Replay a take into the running game through the engine's simulated-input path — a regression test from a real play session. `{events? (from a stop; omit = last take), speed?}` |

## Tests & data

| Tool | What it does |
|---|---|
| `run_tests` | Run engine/project automation tests whose name contains `filter`, sequentially, with per-test pass/fail, errors and durations. Be specific — some editor tests open maps or take minutes. `{filter, max?}` |
| `datatable_create` | New DataTable for a row struct. `{path, row_struct}` |
| `datatable_query` | Row struct, columns, and rows as JSON. `{asset, row?, max?}` |
| `datatable_modify` | `add_row` `{row, values?}` · `update_row` `{row, values}` · `remove_row` `{row}` · `rename_row` `{row, new_name}` — `values` maps columns to JSON. |

## Skeleton & cinematics

| Tool | What it does |
|---|---|
| `skeleton_query` | Bone hierarchy (name/parent/index, optional ref-pose transforms) and sockets of a Skeleton or SkeletalMesh. `{asset, transforms?, bone_contains?}` |
| `socket_modify` | Add / update / remove skeleton sockets — attachment points for weapons and props. `{asset, op, name, bone?, location?, rotation?, scale?}` |
| `animbp_query` | Anim Blueprint structure: target skeleton, state machines (states + transitions), and every anim graph node with its title (which names the assets it plays). `{blueprint}` — pair with `bp_query` for the event graph. |
| `sequence_query` | Level Sequence timing truth: playback range and frame rate, bindings (possessable/spawnable) with tracks, and section start/end in seconds. Playback needs no tool: `call_function` `LevelSequencePlayer.CreateLevelSequencePlayer` + `Play`. `{asset}` |

## Motion matching (GASP)

Requires the **PoseSearch** and **Chooser** plugins (both off by default; the Game Animation Sample enables them). Reflection-only, so UplinkEditor still loads without them — the tools just report which plugin to enable. Note that `animbp_query` shows very little for motion-matching graphs: motion matching is precisely what *replaces* state machines, so use these instead.

| Tool | What it does |
|---|---|
| `posesearch_query` | A Pose Search database: its schema (skeleton, sample rate, feature channels) and every animation it can select from. `{database, max_animations?}` |
| `chooser_query` | A Chooser table — the data-driven logic picking which database/asset applies to the current game state. Dumps rows, columns and result structs, including nested choosers. `{chooser, max_rows?}` |
| `motionmatch_debug` | Live motion-matching state off a character during PIE: each motion-matching anim node with its blend/reselect settings, plus the anim blueprint's exposed values (in GASP: MovementMode, RotationMode, MovementState, Gait — the inputs the chooser keys on). `{actor?, world?, max_values?}`, defaults to the player pawn. |

## PCG (procedural generation)

Requires the PCG plugin. It is **off by default in UE 5.7** and on in 5.8 — `plugin_enable {"name":"PCG"}` then restart. These tools work purely by reflection, so UplinkEditor still loads when PCG is absent; the tools just report that it is not enabled.

| Tool | What it does |
|---|---|
| `pcg_create` | Create an empty PCG graph asset. `{path, name, save?}` |
| `pcg_add_node` | Add a node. `settings_class` takes a friendly name (`SurfaceSampler`, `StaticMeshSpawner`, `GetLandscape`, `TransformPoints`) or a full `/Script/PCG.PCGxxxSettings` path; unknown names come back with close matches. `properties` are applied to the node settings. Returns the node name and its exact input/output pin labels. `{graph, settings_class, properties?, title?}` |
| `pcg_connect` | Wire two nodes, or unwire with `disconnect: true`. `{graph, from_node, from_pin?, to_node, to_pin?, disconnect?}` |
| `pcg_query` | Every node with settings class, title and pin labels — read this before wiring. `{graph}` |
| `pcg_generate` | Add a PCG component to an actor (bounds come from the actor — note `APCGVolume` is a brush volume and cannot be spawned programmatically), assign the graph and generate. `cleanup: true` clears instead. `{actor, graph?, cleanup?, force?, world?}` |

## Environment

| Tool | What it does |
|---|---|
| `lighting_setup` | One-call scene lighting: ensures the standard stack exists (sun, sky light, sky atmosphere, height fog, volumetric clouds, unbound post-process volume) and applies your per-actor settings JSON. The style knowledge is the caller's; this is the atomic apply. |
| `landscape_create` | Heightmap file (8/16-bit grayscale PNG or raw .r16, 32768 = zero) → a real Landscape actor, resampled to a valid layout. Generate the heightmap or use real DEM data. `{heightmap_file, location?, scale?, material?}` |
| `foliage_scatter` | Scatter N instances of a mesh over a circle, each traced down onto the ground — one instanced-mesh actor. `{mesh, count, center, radius, min_scale?, max_scale?, seed?}` |
| `plugin_list` | Engine + project plugins with enabled state and content roots. Enabled plugins are already fully reachable (assets via `path_prefix`, classes via reflection). |
| `plugin_enable` | Enable/disable a plugin in the .uproject (editor restart required to take effect). `{name, enable}` |

## Virtual reality

| Tool | Purpose |
|---|---|
| `xr_simulate` | Drive a VR pawn with no headset. `{action: status|pose|reach|reset, mode?: auto|vr|desktop, device?: hmd|left|right, location?, rotation?, space?: local|world, target?, offset?, look_at?, pawn?, world?}`. `status` reports the headset state, the rig, and whether the hands are usable — start there. `pose` places a device; `reach` puts a hand at an actor (with `offset`) and points it there; `reset` returns hands to a neutral chest pose. |

> Why this works: `UMotionControllerComponent` only overwrites its transform **while tracked** — the engine deliberately keeps the last pose rather than popping the hand to the origin — and `UCameraComponent` only applies an HMD pose when head tracking is allowed. With no headset neither holds, so a written pose persists across ticks. Verified: a posed hand reads the same position three seconds later.

> **VR vs desktop.** Many VR pawns ship a desktop fallback and take it when no headset is present, leaving their controllers deactivated or empty — posing those changes nothing, and the call would still succeed. `status` judges the hands on universal component state only (active? carrying any child?), never on framework conventions, and reports `mode`, `hands.usable` and `hands.notes`. Pass `mode: "vr"` to make a hand pose **fail loudly** rather than pass falsely, or `mode: "desktop"` to leave the hands alone and turn the camera at the target instead. `auto` picks per rig.

> Buttons and triggers are **not** here: a real grip/trigger arrives as an Enhanced Input action, so inject those with `input_action`. Pose the hand, then pulse the grip — that is a grab.

## World queries

| Tool | Purpose |
|---|---|
| `trace` | Ask the physics scene what is there. `{from, to? | direction?+distance?, shape?: line\|sphere\|box\|capsule, radius?, half_height?, channel?, profile?, multi?, complex?, ignore_actors?, draw_seconds?, world?}`. Hits report actor, label, class, component, impact point, normal, distance, physical material and bone. Trace **by profile** (e.g. `Azr_Collision`) to check the setup a project actually uses. A downward trace is how you find ground height — do not parse heightmaps for it. |
| `ai_query` | What the AI is doing: per controller, its pawn, behaviour tree, **active node**, active task chain and full blackboard. `{actor?, blackboard?, max?, world?}`. The active node is not exposed to Blueprint, so nothing else can answer "why is it doing that". A non-behaviour-tree brain (StateTree, custom) is named rather than hidden. AI only exists in a running game. |
| `material_query` | Read a material or material instance: `errors` first (compile errors, taken from the compiled resource — a failed material renders black and logs nothing useful), then expressions, which output inputs are connected, and parameter values. `{material, expressions?, parameters?, recompile?, max?}`. An instance also reports its `parent`; a null parent renders as default material and looks exactly like a broken graph. |
## Observation & assertion

| Tool | What it does |
|---|---|
| `watch_events` | Record broadcasts of any dynamic multicast delegate (BlueprintAssignable events), with decoded parameter payloads. `{actor/object_path/component, delegate: <name>\|"*", world?}` → `watch_id`. Watches stop automatically when PIE ends. |
| `drain_events` | Read captured events oldest-first. `{since_seq?, watch_id?, max?}` → events + `next_seq`. |
| `unwatch` | `{watch_id}` or `{all:true}`. |
| `wait_until` | Non-blocking assertion. `{condition:{type: property_equals\|actor_exists\|actor_gone\|event_count\|elapsed, ...}, timeout?, world?}` → `{condition_met, timed_out, waited_seconds}` — a timeout is a result, not an error. |
| `get_world_state` | Actor snapshot with requested property values inline. `{world?, class_contains?, name_contains?, properties?:[...], max?}` |
| `perf_stats` | Smoothed FPS, average frame ms, last delta, used physical memory. |
| `profile_capture` | Sample frame times for N seconds: avg FPS/ms, p50/p95/p99, worst frame, hitch counts (>33ms, >100ms) — the frame-budget truth over time. `{seconds?}` |

## Scripted playtests

| Tool | What it does |
|---|---|
| `run_scenario` | Ordered tool steps executed as one task with a structured pass/fail report. `{steps:[{tool, params?, expect?, timeout?}], stop_on_failure?}`. `expect` matches fields of the step's result data; a `wait_until` step whose condition times out fails the scenario unless explicitly expected. |

## Blueprints

| Tool | What it does |
|---|---|
| `bp_create` | New Blueprint asset (in memory, marked dirty). `{path, parent_class?}` |
| `bp_query` | Parent class, compile status, variables, and per-graph nodes with pins/defaults/connections. Node guids are the handles `bp_modify` uses. `{blueprint, graph?, max_nodes?}` |
| `bp_modify` | One edit — or a whole batch: `{blueprint, ops: [{op:..., ...}, ...], compile?}` builds an entire event graph in a single call. Give `add_node` ops a `ref` name and later ops address that node as `@ref` in `from_node`/`to_node`/`node`. A failed op stops the batch (earlier ops stay applied). Single-op form: `{blueprint, op, ..., compile?}`. Ops: `add_variable` `{name, type, default?}` (types: `bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,struct:<path>,array:<inner>`) · `remove_variable` `{name}` · `add_node` `{kind: call_function{class,function} \| custom_event{name} \| event{name e.g. ReceiveBeginPlay — reuses a matching ghost/existing event node instead of stacking a duplicate} \| component_bound_event{component, event} — bind a component's or widget's delegate as a graph event (button OnClicked, NiagaraComponent OnSystemFinished, …) \| variable_get/variable_set{name}, graph?, x?, y?}` · `arrange` `{graph?}` — auto-layout the graph (see style rules below) · `connect` `{from_node, from_pin, to_node, to_pin}` (exec pins are `execute`/`then`) · `break_links` `{node, pin}` · `delete_node` `{node}` · `set_pin_default` `{node, pin, value}` (object pins load the value as an object path). |
| `bp_add_component` | Add a component to an actor Blueprint's construction script, like the editor's Add Component button. `class` is a short engine name (`StaticMeshComponent`, `BoxComponent`, `SceneComponent`) or a full path; `parent` attaches under an existing component (default: the scene root). Template conveniences: `location`/`rotation`/`scale`, `static_mesh` (asset path), `collision_profile` (`OverlapOnlyPawn`, `BlockAll`, …), and `properties` as a generic name→JSON map. The component becomes a Blueprint variable — after a compile its delegates bind with `bp_modify component_bound_event`. `{blueprint, class, name, parent?, …, compile?}` |

`bp_modify` operations (single, or batched through `ops` with `@ref` handles):

| Op | What it does |
|---|---|
| `add_variable` / `remove_variable` | Member variables by friendly type string, including `struct:/Script/Module.StructName`. |
| `remove_function` | Delete a function graph by name. Refuses a name the blueprint does not have, and lists the ones it has. |
| `add_function` | Create a real **function graph** — not just nodes in the event graph. `{name, thread_safe?, pure?, category?, inputs:[{name, type, by_ref?, const?}]}` — `by_ref`+`const` are required to match a prototype-validated signature such as an anim node binding, which declares its parameters const-reference. `thread_safe` is required for anim-graph node functions, which cannot run on the game thread. |
| `add_node` | `call_function` (with `class`, default `self`) · `custom_event` · `event` · `component_bound_event` · `variable_get` / `variable_set` (variable named via `name`). |
| `connect` / `break_links` / `delete_node` | Wiring, with schema rejection reasons on failure. |
| `set_pin_default` | Literal pin values, object-aware. |
| `set_node_property` | Set any property on a **graph node** addressed by guid, with dotted paths into structs — e.g. an anim graph node's `Node.OnMotionMatchingStateUpdated.FunctionName`. `{graph, node, property, value, reconstruct?}` |
| `arrange` | Lay the graph out: dependency columns, straight exec lanes, reroute knots at turns. |
| `bp_compile` | Compile and return errors/warnings/messages. `{blueprint}` |

**Graph style rules** (always on for Uplink-authored graphs): nodes never overlap — new nodes are nudged into free space, or auto-placed on a fresh row when `x`/`y` are omitted; `arrange` lays a whole graph out as left-to-right dependency columns with each exec chain on one horizontal lane (level pins = straight wires) and pure data nodes tucked below the lane they feed. Wires that must change height get a **reroute knot** at the turn — the wire leaves its pin dead level, hits the dot just before the target, and the engine's own spline makes the drop — the same way a person tidies a graph by hand (`arrange` re-places its knots on every run). Wire rendering itself is stock Unreal. For material graphs, run `MaterialEditingLibrary.LayoutMaterialExpressions` after authoring.

## Editor UI (Slate)

See the editor itself — every window and panel, not just viewports. `ui_tree` is the map, `capture_widget` is the camera.

| Tool | What it does |
|---|---|
| `ui_tree` | Query the live Slate widget hierarchy. Default: structure of the main window to `max_depth`. `window` picks a window by title substring; `find` searches every window, full depth, for widgets whose type or text contains the string (`find:"NiagaraSystemViewport"`, `find:"Content Browser"`). Rows carry a path (`w0/1/0/3`), type, label text where the widget has one, and `rect` `[x,y,w,h]` in desktop pixels. `{window?, find?, path?, max_depth?, max_nodes?}` |
| `capture_widget` | Screenshot any editor window or single widget as a PNG — asset-editor previews, graph panels, details panels — even when the window is behind others. Target by `path` (from `ui_tree`) or `type` (first descendant type-name match, e.g. `SNiagaraSystemViewport`, `SGraphEditor`); with neither, the whole window. `{window?, path?, type?}` |

## Widgets

| Tool | What it does |
|---|---|
| `widget_tree` | A Widget Blueprint's hierarchy: name, class, parent, root flag, and `is_variable` (only variables can have events bound). `{blueprint}` |
| `widget_add` | Construct a widget into the tree, marked as a variable so its events are immediately bindable with `bp_modify component_bound_event`. `{blueprint, class, name, parent?}` |

## Animation

| Tool | What it does |
|---|---|
| `anim_query` | Montage/sequence timing truth: play length, frame rate + frame count (sequences), montage sections with times, and every notify with exact trigger time, duration, track and class. `{asset}` |
| `anim_modify` | `add_notify` `{name, time \| frame, track?, notify_class?}` — a name-only notify becomes a skeleton notify (fires `AnimNotify_<Name>` / montage `OnNotifyBegin`); `remove_notify` `{name \| index}`. Assets are marked dirty, not saved. |

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

Instance-level control needs no dedicated tools: `call_function` reaches `NiagaraFunctionLibrary.SpawnSystemAtLocation` and every `NiagaraComponent.SetVariable*`; `watch_events` binds `OnSystemFinished`. Created/edited assets are marked dirty — persist with `EditorAssetLibrary.SaveAsset` before closing the editor.

## Discovery — reaching everything else

Two tools turn the engine's whole reflected surface into something searchable, so systems without a dedicated tool (materials, animation, editor asset operations, …) are still reachable:

| Tool | What it does |
|---|---|
| `class_info` | Reflect any class: properties (→ `get_property`/`set_property`), functions with full signatures (→ `call_function`), multicast delegates (→ `watch_events`). `{class, contains?, include_inherited?, max?}` |
| `find_functions` | Search every loaded class for functions by name keyword. Static library functions include `call_via_object_path` — the exact object path to pass to `call_function`. `{query, class_contains?, callable_only?, max?}` |

The pattern, using Epic's scripting libraries (all static BlueprintCallable):

```json
// 1. discover:  find_functions {"query": "MaterialExpression"}
//    -> MaterialEditingLibrary::CreateMaterialExpression(...)  [call_via_object_path: /Script/MaterialEditor.Default__MaterialEditingLibrary]
// 2. call it:
{ "object_path": "/Script/MaterialEditor.Default__MaterialEditingLibrary",
  "function": "CreateMaterialExpression",
  "args": { "Material": "/Game/M_Thing.M_Thing",
            "ExpressionClass": "/Script/Engine.MaterialExpressionMultiply",
            "NodePosX": -400 } }
```

Object and class arguments accept asset/class paths as strings. The same pattern reaches `EditorAssetLibrary` (duplicate/save/delete assets), `AnimationLibrary`, `WidgetBlueprintLibrary`, and every other scripting library in the engine or your project.

**Subsystems**: `object_path` accepts `subsystem:<Class>` to resolve live subsystem instances, whose real object paths are unguessable. Editor and engine subsystems resolve anytime; game-instance/world/local-player subsystems resolve against the chosen world (`world:"pie"` during play). Example — open any asset's editor: `call_function {object_path:"subsystem:AssetEditorSubsystem", function:"OpenEditorForAssets", args:{Assets:["/Game/Path/Asset"]}}` — then `capture_widget` can screenshot the editor it opened. Note the CDO guard: instance methods must be called on instances like these, never through `Default__` paths.

**Worked recipe — material graph authoring** (no dedicated tools needed; every call verified):

```json
// duplicate a base:      EditorAssetLibrary.DuplicateAsset {SourceAssetPath, DestinationAssetPath}
// add an expression:     MaterialEditingLibrary.CreateMaterialExpression {Material, ExpressionClass, NodePosX, NodePosY}
// wire two expressions:  MaterialEditingLibrary.ConnectMaterialExpressions {FromExpression, FromOutputName, ToExpression, ToInputName}
// wire to an output pin: MaterialEditingLibrary.ConnectMaterialProperty {FromExpression, FromOutputName, Property (e.g. MP_BaseColor)}
// tidy + rebuild:        MaterialEditingLibrary.LayoutMaterialExpressions {Material} · RecompileMaterial {Material}
```

Expression object paths come back from each create call; set their fields (e.g. a Constant3Vector's `Constant`) with `set_property`.

## Security model

The HTTP server binds loopback only (never network-reachable), refuses to start if the engine's HTTP listener has been reconfigured to a non-loopback address or the port is taken by another process, validates browser `Origin` headers on every route, and caps request bodies at 2 MB.
