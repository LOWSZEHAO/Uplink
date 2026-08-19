# Changelog

Notable changes, newest first. Pre-1.0, so the API may still move between minor
versions; anything that changed behaviour rather than adding to it is called out.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.30.1

### Fixed
- **A key sent with `route: "ui"` no longer takes focus away from the widget it
  is aimed at.** `SendSlateKey` claimed keyboard focus for the game viewport
  before every send, so a menu that had focused itself lost it a moment before
  the key arrived; the viewport answered handled and the menu never moved. That
  is the whole reason the route exists, and it could not work. Focus is now
  claimed only when it is sitting outside the game. The question is put to the
  focus path through `FSlateUser::IsWidgetInFocusPath`, which is the same test
  the engine runs internally, on the user index `GetUserIndexForKeyboard()`
  names — the dispatched key now goes to that user too, instead of a hardcoded
  0 that only agreed with the focus test by luck. Verified against a commercial
  title: its "press any key" screen, W/S menu navigation and E to confirm all
  respond to simulated keys now, and its in-game tutorial pages advance.
- `click_widget` reports the press and the release separately (`downHandled`,
  `upHandled`). A UMG Button raises `OnClicked` on the release after capturing
  the press, so a press swallowed by something else — an editor viewport hosting
  the session, a panel over the top — leaves the button unfired. Folding both
  edges into one `handled` reported that as a click. `handled` is unchanged for
  callers that read it; the message now names which edge was lost.
- `input_key` reports where the key actually landed on every route that
  touches Slate, taps included: `focusedWidget`, `focusInGameViewport`, and
  `focusForced` (whether this call had to move focus itself). When a menu
  ignores a key that came back handled, that is the diagnosis — `SPIEViewport`
  with `focusForced: true` means the key reached the game because nothing in
  the UI held focus. The message says it in words too: "sent to the focused
  widget (SObjectWidget), which handled it".

## 0.30.0

### Added
- **Two modules.** `UplinkContentTools` takes the tools with heavy dependencies
  (Niagara, material, animation, landscape, sequencer, AI) into a module of its
  own, registering through `IUplinkToolProvider` exactly as a third-party plugin
  would. The core stops pulling `Niagara`, `MaterialEditor`, `AnimGraph`,
  `Landscape`, `LevelSequence` and `MovieScene` into every build, and the
  extension point gains its first real consumer instead of a worked example.
  Tools are attributed to the module that registered them.
- `worlds` — every live world with an id you can pass as `world`: the level
  being edited, one per PIE instance, previews. Reports type, net mode, map,
  PIE instance, actor count, and which one an omitted `world` resolves to.
  `world` now accepts those ids (`pie:1`) as well as `editor` and `pie`.
- `artifacts: {evidence_on_failure: true}` on `run_scenario`. Detecting a
  failure and diagnosing one are different problems, and a boolean is only the
  first. At the first failed step, before teardown, it captures the frame, an
  `observe` snapshot, which watched events fired, and what the log said — each
  recorded with the question it answers. No inferred cause: the bundle is facts.
- `.gitattributes`, normalising line endings. The repo had drifted to 106 files
  LF and three CRLF.

### Fixed
- `UPLINK_VERSION` had been left at `0.26.0` while the plugin shipped `0.29.0`,
  so `/status` and the MCP `serverInfo` handshake reported the wrong version for
  four releases.

## 0.29.0

### Changed
- `UplinkTools_Blueprint.cpp` (2561 lines) split into `Blueprint/`, one subject
  per file. `UplinkTools_Object.cpp` split into `Object/`, with `get_property`,
  `set_property` and `call_function` a file each.
- JSON and Unreal reflection now convert through one layer,
  `UplinkValueConverter`, rather than inline wherever it was first needed — the
  arrangement where an edge case gets fixed in one place and stays broken in
  three others.
- Tools record which provider registered them and its version; a name collision
  names both sides instead of silently replacing.
- `run_scenarios.ps1` forwards `setup`, `teardown`, `artifacts` and
  `budget_seconds`. It had forwarded only `steps`, so those phases were dropped
  without failing — a harness quietly ignoring half a test file.

### Fixed
- `observe` read collision off the root component only. A Blueprint actor's root
  is `DefaultSceneRoot`, which has none, so every Blueprint actor reported
  nothing. It now falls back to the first colliding primitive and names it.

## 0.28.0

### Added
- Parameter **type** validation — a pragmatic subset of JSON Schema (`type`,
  `required`, `enum`, `minimum`/`maximum`, `minLength`/`maxLength`, `items`,
  nested `properties`). Only unknown parameter *names* were rejected before, so
  `{"count": "hello"}` reached a tool that had asked for an integer.
- Optional authentication. With `UPLINK_AUTH_TOKEN` set, every route requires
  `Authorization: Bearer <token>`, compared in constant time. Unset, nothing
  changes. The Node bridge reads the same variable.
- Risk annotations: `destructiveHint` alongside `readOnlyHint` (both standard
  MCP), plus `arbitraryExecutionHint`, `requiresPieHint` and `longRunningHint`,
  from one auditable table rather than scattered across the registration sites.
- `observe` — one player-centred read of the running game: pawn state, what is
  under the crosshair, and the nearest actors with collision, overlaps and the
  events they can fire. Facts only; there is deliberately no `interactable`
  field.
- Scenario phases: `setup` (a failed fixture skips the steps rather than failing
  them for the wrong reason), `teardown` (**always** runs, including after an
  aborted run), `artifacts`, and `budget_seconds`.
- Six scenarios — invalid object references, invalid function arguments,
  PIE/editor world isolation, batch failure semantics, recompiling across a
  dependency, large spawn. Fifteen in total.

### Changed
- REST and MCP share one dispatcher. Each had carried its own copy of resolve →
  validate → clamp → submit → await, and they had drifted on how long a caller
  may wait.
- A tool whose input schema will not parse is no longer registered at all. It
  used to be served with no parameters, which made every option it had
  undiscoverable — the tool's own interface lying to the agent.
- Cancellation and timeouts reach the invocation through
  `IUplinkInvocation::Cancel`. Both had only set a status field, so the caller
  got an answer while the operation carried on writing to the editor.
- Tasks declare the resources they need and wait for them; read-only work takes
  shared locks and never blocks other reads.
- `link_into_project.ps1` junctions only the source and gives each project its
  own `Binaries/` and `Intermediate/`, so one clone serves a 5.7 project and a
  5.8 project at once. Previously whichever you built last won.

### Fixed
- **`call_function` did nothing on an editor-world actor and reported success.**
  `AActor::GetFunctionCallspace` answers `Absorbed` when the actor's world is not
  a game world, and `ProcessEvent` responds to that with a bare `return` — no log,
  no error — handing back the parameter frame untouched. Since return values live
  in that frame, an untouched frame reads as a real answer:
  `K2_GetActorLocation` on an actor at z=12345 replied `(0,0,0)`.
- `Origin` is parsed into scheme, host and port instead of prefix-matched.
  `http://localhost.evil.com` starts with `http://localhost` and was accepted.
- Two latent bugs in the task manager: a reference held into the task map across
  the first step, and iteration of that map while a step could add to it.

## 0.27.0

### Added
- Event dispatchers (`add_dispatcher`, and bind/unbind/call/clear nodes),
  interfaces (`implement_interface`, `remove_interface`), parent overrides
  (`override_function`, `call_parent`) — the half of a Blueprint that is not
  nodes.
- `bp_references` — what calls this function, reads this variable, binds this
  dispatcher. Reports whether the search was exhaustive rather than presenting a
  bounded scan as a complete answer.
- Full variable typing and details-panel metadata; `bp_query` pages large graphs.

### Fixed
- `override_function` duplicated the event node when asked twice: once the event
  is implemented the engine resolves its declaring class to the skeleton class,
  and the engine's own lookup then cannot match the node the previous call made.
  It compiled clean, because the duplicate lands beside a disabled ghost node.
- `bp_references` missed a variable used in its own graph — a blueprint's
  `SKEL_Foo_C` and `Foo_C` are siblings, so `IsChildOf` is false both ways.

## 0.20.0 – 0.26.0

Blueprint flow control (branch, sequence, cast, switch, select, make/break
struct, the standard macro library). `bp_find_broken` and `bp_repair` for a
project a C++ change has broken. `asset_create` for the assets `bp_create`
cannot make. Seven tools for reading a project nobody explained —
`project_entry`, `ui_live`, `input_map`, `actor_components`,
`streaming_status`, `frame_strip`, `dialog_state`. C++ default arguments applied
from `CPP_Default_*` metadata. Unknown parameters rejected with a suggestion.
A render-thread crash fixed in the Slate capture path. VR simulation removed
from the plugin. The README stopped claiming Uplink plays your game.

## 0.1.0 – 0.19.0

Initial development, 11–13 August 2026: plugin skeleton, build and linking
scripts, the tool registry and task system, the native MCP endpoint and the Node
bridge, reflection (`class_info`, `find_functions`, `get_property`,
`set_property`, `call_function`), the world and asset tools, PIE lifecycle and
player control, the event recorder and `wait_until`, `run_scenario`, and the
first scenarios.
