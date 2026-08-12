# Uplink tool reference

All 55 tools, grouped by layer. Conventions used throughout:

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
| `delete_actors` | Destroy actors by name/label. `{names:[...], world?}` |
| `move_actor` | Set location/rotation/scale (any subset), physics-safe in PIE. `{actor, location?, rotation?, scale?, world?}` |

## Reflection

| Tool | What it does |
|---|---|
| `get_property` | Read any UPROPERTY as JSON. Target via `object_path`, or `actor` + optional `component`. `{..., property, world?}` |
| `set_property` | Write any UPROPERTY from JSON; in the editor world also runs `PostEditChangeProperty`. `{..., property, value, world?}` |
| `call_function` | Call any UFUNCTION by reflection: `args` maps parameter names to JSON values; the response carries the return value and out-params. Unknown arg names are rejected with the expected parameter list. `{..., function, args?, world?}` |

## Assets

| Tool | What it does |
|---|---|
| `asset_search` | Name-substring search. `{query?, class_contains?, path_prefix? (default /Game), max?}` — plugin content needs its mount point as `path_prefix` (e.g. `/MyPlugin`). |
| `asset_dependencies` / `asset_referencers` | Package-level dependency graph. `{package}` |

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

## Observation & assertion

| Tool | What it does |
|---|---|
| `watch_events` | Record broadcasts of any dynamic multicast delegate (BlueprintAssignable events), with decoded parameter payloads. `{actor/object_path/component, delegate: <name>\|"*", world?}` → `watch_id`. Watches stop automatically when PIE ends. |
| `drain_events` | Read captured events oldest-first. `{since_seq?, watch_id?, max?}` → events + `next_seq`. |
| `unwatch` | `{watch_id}` or `{all:true}`. |
| `wait_until` | Non-blocking assertion. `{condition:{type: property_equals\|actor_exists\|actor_gone\|event_count\|elapsed, ...}, timeout?, world?}` → `{condition_met, timed_out, waited_seconds}` — a timeout is a result, not an error. |
| `get_world_state` | Actor snapshot with requested property values inline. `{world?, class_contains?, name_contains?, properties?:[...], max?}` |
| `perf_stats` | Smoothed FPS, average frame ms, last delta, used physical memory. |

## Scripted playtests

| Tool | What it does |
|---|---|
| `run_scenario` | Ordered tool steps executed as one task with a structured pass/fail report. `{steps:[{tool, params?, expect?, timeout?}], stop_on_failure?}`. `expect` matches fields of the step's result data; a `wait_until` step whose condition times out fails the scenario unless explicitly expected. |

## Blueprints

| Tool | What it does |
|---|---|
| `bp_create` | New Blueprint asset (in memory, marked dirty). `{path, parent_class?}` |
| `bp_query` | Parent class, compile status, variables, and per-graph nodes with pins/defaults/connections. Node guids are the handles `bp_modify` uses. `{blueprint, graph?, max_nodes?}` |
| `bp_modify` | One edit per call, `{blueprint, op, ..., compile?}`. Ops: `add_variable` `{name, type, default?}` (types: `bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,struct:<path>,array:<inner>`) · `remove_variable` `{name}` · `add_node` `{kind: call_function{class,function} \| custom_event{name} \| event{name e.g. ReceiveBeginPlay — reuses a matching ghost/existing event node instead of stacking a duplicate} \| component_bound_event{component, event} — bind a component's or widget's delegate as a graph event (button OnClicked, NiagaraComponent OnSystemFinished, …) \| variable_get/variable_set{name}, graph?, x?, y?}` · `arrange` `{graph?}` — auto-layout the graph (see style rules below) · `connect` `{from_node, from_pin, to_node, to_pin}` (exec pins are `execute`/`then`) · `break_links` `{node, pin}` · `delete_node` `{node}` · `set_pin_default` `{node, pin, value}` (object pins load the value as an object path). |
| `bp_add_component` | Add a component to an actor Blueprint's construction script, like the editor's Add Component button. `class` is a short engine name (`StaticMeshComponent`, `BoxComponent`, `SceneComponent`) or a full path; `parent` attaches under an existing component (default: the scene root). Template conveniences: `location`/`rotation`/`scale`, `static_mesh` (asset path), `collision_profile` (`OverlapOnlyPawn`, `BlockAll`, …), and `properties` as a generic name→JSON map. The component becomes a Blueprint variable — after a compile its delegates bind with `bp_modify component_bound_event`. `{blueprint, class, name, parent?, …, compile?}` |
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
