# Changelog

Notable changes, newest first. Pre-1.0, so the API may still move between minor
versions; anything that changed behaviour rather than adding to it is called out.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.36.0

Three found while using Uplink on a real project. The third is the one that
cost weeks.

### Fixed
- `set_property` refuses a property the engine has deprecated, and
  `get_property` reports one. A field marked `UE_DEPRECATED` with
  `meta=(DeprecatedProperty)` took the write, returned success, and handed the
  value straight back on the next read - so every check a caller can make
  agreed the edit had worked, while the engine had stopped consulting the
  field releases ago. That is the shape of bug that survives, because nothing
  ever contradicts it. The refusal carries the author's own
  `DeprecationMessage`, which usually names the replacement; `force:true`
  writes one anyway, and reads are annotated rather than refused, since
  reading is how you find out what a stale asset still holds. Tested by
  metadata rather than by name, so a live property with "Deprecated" in its
  name is untouched.

- `input_map` answers `applied` from every playing world instead of one.
  It resolved the Enhanced Input subsystem through `GEditor->PlayWorld`, which
  is assigned once per PIE instance as each is created (PlayLevel.cpp) - so in
  a multi-instance session it holds whichever came last, arbitrarily, and one
  world out of several. Asking it about a world with no local player of its
  own, as a server has none, left the subsystem null and reported every
  context unapplied while a real key was driving the game through one of them.
  Every playing world's local players are consulted now, `applied_in` names
  which worlds a context is applied in, `inspected_worlds` names the ones
  looked at, and `world` scopes the question the way it does in every other
  tool.

- `set_pin_default` no longer calls an empty value on an enum pin a failure. An
  enum pin cannot be emptied: `GetPinDefaultValuesFromString` turns "" into the
  first entry's name and stores that. Comparing the reply against "" then
  reported the write as declined - while the pin had in fact moved to entry 0.
  The pin changed and the caller was told it had not, which is the worse half
  of the two. The value is normalised to entry 0's name up front, so the answer
  matches what the engine did. Found by auditing the write path, not by a
  failure.

- `set_pin_default` takes an enum's authored name. A pin stores the
  enumerator's own name, which for a User Defined Enum is the
  `NewEnumerator<n>` it was born with and never the name the node draws - so
  the only spelling a person has was declined, and the pin kept what it had.
  The value is translated to the stored name before the schema sees it. A name
  that is already the stored one passes through, every non-enum pin is
  untouched, and a name that is neither is still refused.

## 0.35.0

### Added
- `enum_query` and `enum_modify`: read a User Defined Enum's entries and add,
  remove, rename or reorder them. `asset_create` could already make the enum
  asset - it picks `EnumFactory` and reports success - but
  `FEnumEditorUtils::CreateUserDefinedEnum` builds it by calling `SetEnums`
  with an EMPTY name array, and nothing could put an entry in it afterwards.
  So the asset existed, a variable could be typed to it, and a `switch_enum`
  on it came back with no case pins at all. An enum you can reference and
  cannot fill is worse than one you cannot make.

  `AddNewEnumeratorForUserDefinedEnum` takes no name and returns void, so a
  named entry is add-then-rename and the entry count is the only evidence the
  add happened - the same shape as `FStructureEditorUtils::AddVariable`. An
  entry keeps the `NewEnumerator<n>` it was born with as its stored name while
  renaming changes only the display name, so both are reported: `name` is what
  a person authored, `raw_name` is what a saved asset and a switch pin see.

  `move` bounds its target index here rather than leaving it to the engine.
  `MoveEnumeratorInUserDefinedEnum` tests the index against `NumEnums()`,
  which counts the hidden trailing `_MAX`, and then inserts into a list built
  by `CopyEnumeratorsWithoutMax` - so the one index its own check admits and
  its array cannot hold runs off the end.

  `remove` and `move` are reported as renumbering: both end with
  `Names[i].Value = i` across the whole list, so every entry after the edit
  changes value. Anything already holding one of those numbers - a placed
  actor's property, a save - keeps the number and therefore means a different
  entry. `add` only appends and is safe.

- `bp_query` reports `shown_as` on a pin whose editor label differs from the
  name `connect` addresses it by. This is how a Switch on a User Defined Enum
  reads: its case pins are named after the stored `NewEnumerator<n>` while the
  node draws the authored name, so a caller who added "Closed" and then looked
  at the graph found no pin called that, and no way to tell which one was
  theirs.

### Fixed
- `set_property` refuses the trailing `_MAX` marker on an enum property. UHT
  appends one to every UENUM; `IsValidEnumValue` says yes to it, and it wrote
  and reloaded happily, leaving a property reading `ESomething_MAX` - which
  looks like an answer and is not one. It is a count, not a state, and no
  editor dropdown offers it: the enum pin widget and the switch node both stop
  at `NumEnums() - 1`, and that same bound is what is applied here. Every real
  value, and every name, is unaffected.

- `enum_modify` accepts either name an entry has. It matched only the authored
  one, so a caller who read `NewEnumerator0` off a Switch node's case pin had
  to call `enum_query` to translate before acting on it. The authored name is
  still tried first, since it is what a person means; the stored name is tried
  second, and is also the spelling that does not move when an entry is renamed
  or the editor runs in another language.

- `set_property` refuses a JSON boolean on an enum property instead of
  converting it. `FJsonValueBoolean::TryGetNumber` answers with 1 or 0, so
  `true` passed the range check added in 0.33.0 and landed on whichever entry
  happened to be numbered 1: setting `SpawnCollisionHandlingMethod` to `true`
  reported success and left it reading `AlwaysSpawn`. Found by auditing that
  guard rather than by anything failing.

## 0.34.0

### Added
- `bp_modify` `add_node` `variable_get` / `variable_set` take `class`: the
  object the variable lives on. Both node kinds called
  `VariableReference.SetSelfMember`, so a variable node could only ever be
  about the blueprint it sat in. That left an ordinary Blueprint node
  unreachable - reading `PlayerState` off the `NewPlayer` controller handed to
  `OnPostLogin`, where the target is a parameter and not self. Passing `class`
  makes the reference external, which is what gives the node its Target pin.
  Omitted, the node is about this blueprint exactly as before.

  The reference goes in through `SetFromField` rather than
  `SetExternalMember`, for two reasons. It reads the DECLARING class off the
  property, so passing `PlayerController` for a variable declared on
  `Controller` still resolves rather than pointing at a class that does not
  own it. And it fills in the member guid, which is what keeps a reference to
  a *Blueprint* variable alive when that variable is renamed; the two-argument
  `SetExternalMember` leaves the guid invalid.

  Legality is checked before the node is built, through
  `IsPropertyReadableInBlueprint` / `IsPropertyWritableInBlueprint` - the same
  answers the editor uses, so private and protected are judged from this
  blueprint rather than in the abstract. This is checked up front because the
  compiler does not say it loudly: `UK2Node_Variable::ValidateNodeDuringCompilation`
  reports an unresolved variable as a WARNING, so a node referring to nothing
  leaves the blueprint compiling with zero errors. A set of a
  `BlueprintReadOnly` property is refused with that as the reason, and an
  unknown name is refused with the class's blueprint-visible variables listed.

## 0.33.0

Two more gaps from the same run of real projects, and one bug the fix for the
second one introduced and had to be closed.

### Added
- `bp_modify` `add_node` `get_subsystem` `{class}`: the *Get Subsystem* node,
  typed to a concrete subsystem class. There are four node classes behind that
  one menu entry, and the right one is chosen from the class's own ancestry -
  GameInstance, World, LocalPlayer and AudioEngine subsystems share the base
  node, Engine and Editor subsystems have their own. Picking wrong is not a
  visible error: the node builds, and the compiler later says "Node @@ must
  have a class specified" about a node that plainly has one. A class from no
  subsystem family at all is refused here, with the families listed.

  The class is set through `Initialize()` before the node is placed, because
  `AllocateDefaultPins` reads it to type the return pin and adds a loose
  `Class` input pin when it is unset. It is also the only way in: `CustomClass`
  itself is protected. The node is constructed through the base pointer with
  the chosen class, since the three derived classes are plain `UCLASS()` and
  their own constructors are `NO_API`.

### Fixed
- `set_property` no longer reports correct writes as failures. It confirmed a
  write by re-serialising the property and diffing that against the caller's
  JSON, which fails on spelling alone: an enum sent as `2` or as
  `EComponentMobility::Static` reads back as `"Movable"` and `"Static"`, and an
  object inside an array reads back in export form. Those writes all landed,
  and all were reported as writes that did not survive - the worst kind of
  wrong answer, because the next move is to undo work that was right.

  It now re-applies the request to a throwaway copy of what is actually there
  and compares with `FProperty::Identical`, so whatever spelling the writer
  accepts, the check accepts. Copying the landed value first is what keeps a
  partial struct write honest: `{"X":1}` on a vector is a request about X, and
  Y and Z must not be read as having been asked for. A write that was genuinely
  reverted still fails - re-applying the request to the reverted value produces
  the value that was asked for, and that differs from what is there.

- A number that no enumerator answers to is refused before the write. The
  engine's importer calls `SetIntPropertyValue` with no bounds check, so
  `SpawnCollisionHandlingMethod = 99` used to succeed at the memory level and
  leave the property holding a value nothing in the enum names - reading back
  as an empty string, discovered somewhere else much later. The old diff caught
  it by accident, for the wrong reason, and the `Identical` check above would
  not have: re-applying 99 to a copy of 99 matches. Bitflag enums are left
  alone, since a combination is a legitimate value there.

- `get_property` reports objects inside arrays and sets the same way it reports
  a single object reference. A container went to the engine converter whole,
  which writes its elements as `/Script/Engine.Material'/Game/M_X.M_X'` while
  the same object read on its own came back as `/Game/M_X.M_X` - one tool
  reporting one reference in two spellings, only one of which is a path
  anything else here accepts back.

## 0.32.0

Three gaps found the same way: using Uplink on real projects and hitting the
point where the graph or asset the tool had just reported success on was not the
one the editor showed.

### Added
- `bp_modify` `add_node` `custom_event` accepts `inputs`, so an event can be
  created with its parameters. The field was already declared on the tool (for
  `add_function`) and already accepted by the schema, so passing it read as
  supported and came back `"compiled"` — but the `custom_event` branch never
  looked at it, and the event arrived with nothing on it but `OutputDelegate`
  and `then`. The parameters are OUTPUT pins: data flows out of the event into
  the graph, and `UK2Node_CustomEvent` refuses `EGPD_Input` outright. Each pin
  is put to the node's own `CanCreateUserDefinedPin` first, because
  `CreateUserDefinedPin` never consults it and returns whatever the virtual
  `CreatePinFromUserDefinition` gives — null on the base class, which would
  record the parameter with no pin on the node to show for it.

- `struct_query` and `struct_modify`: read a User Defined Struct's members and
  add, remove, rename, retype or default them. `asset_create` could already make
  the struct asset, but nothing could put a member in it, so every struct came
  out holding only the factory's `MemberVar_0` placeholder. Members are
  addressed by the name the struct editor shows — the stored field name is
  `Damage_2_<32 hex digits>`, which nothing can be expected to predict.

- `bp_modify` `add_node` `enhanced_input` `{action}`: the Enhanced Input event
  node for an Input Action asset. There was no way to author one at all, which
  left the modern input path unreachable from the graph tools. The action is
  resolved before the node is placed, because `AllocateDefaultPins` reads it to
  type the `ActionValue` pin and to create the action pin at all. A second call
  for the same action reuses the existing node rather than stacking a duplicate
  the compiler rejects, which is what the editor's own spawner does.

### Fixed
- `struct_modify` refuses two edits the engine handles badly rather than
  passing them through. `AddVariable` generates `MemberVar_<n>` from a counter
  on the asset and then `check()`s that the name is free, so a member sitting
  under one of those names takes the editor down once the counter reaches it —
  renaming into that pattern is refused instead. And `RemoveVariable` will not
  leave a struct with no members: it early-outs at a count of one and says so
  only at Log verbosity, so the refusal is now explained rather than reported
  as a bare failure.

## 0.31.0

### Added
- `wait_until` gains a `navmesh_ready {location}` condition: it waits until a
  complete route can actually be built from the player pawn to that location,
  which is the question `navigate_to` is about to ask. Runtime navmesh tiles
  build across frames, so pathing issued straight after `BeginPlay` fails with a
  message about the goal being off the navmesh, and the usual workaround is
  sleeping a guessed number of seconds that has to be retuned per level and per
  machine. It deliberately avoids the two cheaper proxies, both of which
  answered yes while pathing still failed: the remaining-build-task count reads
  zero *before* generation starts, and projecting a point onto the mesh succeeds
  when there is mesh merely near it.

- `level_open` and `level_new`, which open a level and then confirm the editor
  is on it. Going through `LevelEditorSubsystem` directly has a trap in it:
  `NewLevel` writes the asset and returns true without making it the level
  being edited, so every actor placed afterwards lands in whatever level was
  already open, the new one saves empty, and not one call reports a problem.
  Both tools verify where the editor actually ended up and fail if it is
  somewhere else.

- `scripts/check_repo.ps1`, and a GitHub Actions workflow that runs it on every
  push. Sixteen static checks that need no engine and finish in seconds:
  every tool schema parses as JSON, tool names are unique, every scenario step
  names a real tool and passes only parameters that tool declares, the trait
  table has no orphans, TOOLS.md documents every tool and its count is true,
  and the version is the same number in all four places that carry it. These
  all rot the same way — the code keeps working while something written beside
  it stops being true — so a build gate never sees any of them. `scripts/ci.ps1`
  runs the checks, the dual-engine build and the scenario suite in one command,
  and reports a stage that did not run as skipped rather than passed.

  One of the sixteen greps for the names of unrelated projects the machine also
  works on. Prose written while looking at another codebase carries its
  vocabulary out with it, and nothing about the code gives that away - the
  `trace` tool shipped a private project's collision profile as its worked
  example, and a comment named that project's renamed trace channel. The list
  of names is read from `scripts/private_terms.local.txt`, which is gitignored,
  because committing the list would publish what the check exists to keep out.
  Absent, the check reports as skipped rather than passed.

- **`bp_modify add_node` can author a Timeline.** `kind:"timeline"` builds all
  three objects a Timeline actually is - the `UTimelineTemplate` on the asset, a
  float curve carrying the keyframes, and the `UK2Node_Timeline` bound to it by
  variable name - which the editor otherwise only ever builds together through
  its own graph action menu. `{name?, length?, loop?, autoplay?, track?, keys?}`,
  where `track` names the float output pin and `keys` are `[{time, value}]`.

  Timelines are how most Blueprint gameplay moves anything - doors, platforms,
  lerps, fades - so an authoring tool without them cannot write a moving door.
  Verified by authoring a platform end to end and watching it run: BeginPlay
  stores the start location, a looping two-second timeline drives a Lerp into
  SetActorLocation, and the placed actor oscillates between z=250 and z=450.

  Two engine details the implementation has to get right, both silent when
  wrong. Length, `loop` and `autoplay` only take effect on the template - the
  node's copies of all three are transient caches, overwritten on every pin
  rebuild. And `AllocateDefaultPins` walks the template's display-track order
  rather than its float-track array, so a track added without a display entry
  compiles into a working runtime binding with no output pin and no error.

- **`observe` can gather the whole situation in one call.** `include` adds
  `screenshot` (the frame as an image block), `ui` (the UMG on screen and
  whether a click could land on it) and `events` (watched delegates that have
  fired) to the reply it already gives about the player and what is around
  them. Asking for all three answers where the player is, what is around them,
  what is on screen and what just happened - taken at one moment. The four
  separate calls it replaces cannot do that: each is a round trip through the
  game thread and the game moves between them, so the four answers describe
  four different moments, and an agent stitching them is reasoning about a
  situation that never existed.

  Nothing here is a second implementation. The viewport capture behind
  `viewport_screenshot` and the screen enumeration and widget walk behind
  `ui_live` moved to a shared header and are called from both, so the two tools
  cannot disagree about what is on screen - checked live with three widgets, and
  both report the same three. Reading events takes nothing away from a
  `drain_events` loop, because that cursor lives with the caller.

### Fixed
- **`run_scenario` validates each step's parameters.** `ValidateParams` had one
  call site - the HTTP/MCP transport - and a scenario step went straight to the
  tool's factory, so every step in every scenario ran unvalidated. A misspelt
  parameter was dropped in silence and the step passed having run the tool with
  its defaults, on the one caller whose entire product is a pass/fail verdict.
  The same hole made `expect_failure` unsound: a step asserting a refusal the
  schema layer owns passed for the wrong reason, because no refusal happened.
  Validation runs after template expansion, since a step's parameters are not
  known until its `$steps[N]` references resolve.

  This was not hypothetical. `04-refuses-bad-input.json` opened with `bp_create`
  given `parent` instead of `parent_class`, and its own note read "a step that
  passes is the bug": called directly the parameter is refused by name, and as a
  scenario step it created a Blueprint with the default parent and passed.

- **A scenario refused before any step ran is no longer scored as a pass.** The
  runner read `@($result.data.steps).Count`, and `@($null).Count` is 1 in
  PowerShell, so a scenario that never executed reported one step that had run
  and behaved. `04-refuses-bad-input` ended on a step naming a tool that does
  not exist, which is refused at parse time and aborts the file - so its other
  assertions had never run at all, under a green line reading "all 1 steps
  correctly refused". That case now lives in `18-unknown-tool.json`, which
  declares `_expect_scenario_refused` and asserts on the call; the runner no
  longer scores any file backwards by its filename.

- `wait_until` refuses a condition that can never become true instead of
  waiting out its timeout. `actor_exists`/`actor_gone` never checked that
  `actor` was given, and the lookup falls back to a substring match - every
  string contains the empty string, so an omitted actor matched whichever actor
  came first and answered "condition met" instantly, naming nothing. A
  `property_equals` naming neither `actor` nor `object_path` had its resolve
  error discarded and waited the full timeout. An object that has not spawned
  *yet* is still worth waiting for, so that case is untouched; what is refused
  is a condition with no target at all.

- `widget_add` refuses a widget the root panel would not take. The
  explicit-parent branch already checked `AddChild` and said exactly why; the
  branch three lines below it discarded the same return value, so adding a
  second child to a Button or Border root reported success and left the widget
  in the tree parented to nothing, surfacing later as a compiler complaint with
  no apparent cause.

- `pie_start` refuses a `rotation` passed without a `location` rather than
  ignoring it. The engine gates the whole placement on the location -
  `FRequestPlaySessionParams::HasPlayWorldPlacement()` is `StartLocation.IsSet()`
  - so a rotation alone was never read and the pawn spawned at the level's
  PlayerStart facing its own way, with the call reporting success.

- `foliage_scatter` scatters into the editor world, like `lighting_setup` and
  `landscape_create` beside it. It followed the caller's world instead, and
  never declared a `world` parameter to say so - so with a session running it
  placed its instances in the PIE copy, counted them, reported them, and lost
  them all at `pie_stop`.

- `bp_modify op=set_variable` refuses `type` and `default` instead of dropping
  them. Only `add_variable` reads those - it passes them to
  `AddMemberVariable` - so a call meant to retune an existing variable reported
  success and changed nothing.

- `anim_modify` records its edits in the transaction buffer. It called
  `MarkPackageDirty` but never `Modify()`, so its transaction was empty and
  `edit_history undo` - the documented way back from a destructive tool - could
  not restore a removed notify. It is now marked Destructive, along with
  `level_open` and `level_new`.

- `level_open` and `level_new` refuse when other packages have unsaved changes,
  naming them, and take `discard_unsaved` to proceed anyway. The engine's load
  path does not prompt, which is deliberate for tools that run unattended, but
  `spawn_actor`, `spawn_batch` and `move_actor` only mark the level dirty - so
  an agent that placed forty actors and then opened another map to check
  something lost all forty and was told the map opened.

- **`drain_events` no longer loses events it capped.** It kept the NEWEST `max`
  and then set `next_seq` to the newest sequence in the recorder, so a burst
  larger than the cap had its oldest events dropped and the cursor stepped
  straight over them - in the polling loop the tool documents, with nothing
  saying so. It now returns the oldest of what was waiting, sets `truncated`,
  and leaves the cursor just after the last event handed over, so the next call
  collects the remainder.

- `add_node kind:"event"` anchors the node to the class that declares the
  function. `UClass::FindFunctionByName` searches implemented interfaces, so an
  interface event passed validation, and the node was then stamped with
  `ParentClass` - which the reference resolves along the super chain, where an
  implemented interface never appears. The call reported success and produced a
  node that could never resolve.

- **`run_tests` can see the whole test suite.** `GetValidTestNames` returns only
  tests whose filter flag is in the framework's `RequestedTestFilter`, and the
  framework constructs that as `SmokeFilter` alone (`AutomationTest.cpp`, both
  5.7 and 5.8). Every test declares exactly one filter type, so nothing outside
  the smoke set was ever visible: on this project the tool saw 429 tests out of
  6432 and answered "no tests match" for filters naming perfectly real ones -
  a wrong verdict over a wrong count, with the reason nowhere in the reply. The
  filter is widened for the run and restored afterwards.

- **An auth token containing a comma can authenticate.** The engine splits every
  header value on commas while parsing, so `Bearer a,b` arrived as two values:
  the first carried the prefix and a truncated secret, the second carried no
  prefix and was skipped, and no token with a comma in it could ever match
  however correct it was. The values are rejoined before parsing, which is the
  exact inverse of the split. Verified against a live editor: the full token is
  accepted, and the truncated and wrong forms are still refused.

- Every `material_query` parameter row says whether the instance `overridden` it
  or inherits it. The description promised "overridden parameters" and the reply
  was the parent's entire declared set, undifferentiated - an instance setting
  one of two dozen looked identical to one setting all of them.

- A held input is released when its step is abandoned. `input_action`'s timed
  hold and `input_key`'s tap press on the way in and release on a later tick, so
  a step that timed out or was cancelled left the action injected or the key
  down for the rest of the session, and every step afterwards ran against a pawn
  that would not stop moving. Both now release in `Cancel`.

  The runner was defeating that anyway: `run_scenario` dropped a timed-out child
  without calling `Cancel` at all, and had no `Cancel` of its own to pass a
  cancellation down with - so the two tools that did implement cleanup were
  disarmed by their own runner.

- `task_cancel` says which of the four things happened. It called the older
  `Cancel` wrapper, which collapses `RequestCancel`'s outcomes into a bool, so
  a task that reported it could not be interrupted safely - still running, and
  ending on its own deadline - came back as "task not found or not running".
  The one case where the caller most needs the truth was told the opposite.

- `possess` reports the pawn the controller actually holds. `Possess` returns
  void and does nothing without authority, so a refused possession - a client
  controller in a multi-client session - was reported as a successful one,
  because the reply echoed the pawn that had been asked for.

- `capture_widget` and `ui_tree` honour the `wN` a path carries. The window
  index was parsed and thrown away under a comment claiming it was already
  resolved, so a path taken from an asset editor was walked from whichever
  window was largest, and the screenshot came back of a different widget
  entirely, reported as the one asked for. An explicit `window` still wins, and
  a path naming a window that is no longer open is refused rather than walked
  somewhere else.

- `set_property` verifies the value survived and refuses when it did not. A
  Blueprint variable that is not instance editable is put back by the actor's
  construction scripts on the way out of `PostEditChangeProperty`, and the
  write, the change event and the dirty flag all succeed on the way past — so
  the call reported success and the number was the old one. The read-back goes
  through a fresh resolve rather than the address written to, because rerunning
  the construction scripts rebuilds components and that address may no longer
  belong to anything. The refusal names the cause and the fix.

- `save` no longer reports failure for an unnamed `/Temp/` level. It is a
  scratch level the editor is holding, it has no file to write to, and it is
  almost never what the caller was saving — one turned an otherwise complete
  save red. Those packages are listed separately in `unnamed`.

- The `world` parameter is declared the same way on all 23 tools that take one.
  Fifteen of them declared `enum: [editor, pie]`, which the schema validator
  enforces, so they refused the very ids `worlds` exists to hand out: passing
  `"pie:1"` to name one client in a multi-client session was rejected before
  the resolver ever saw it, on `get_property`, `call_function`, `click_widget`,
  `console_command` and `actor_components` among others, while the eight tools
  without the enum accepted it. Nothing is lost by removing it — an unknown
  world was already refused at resolve time with a better message, naming the
  worlds that do exist.

- `get_property`, `set_property`, `call_function` and `observe` report which
  worlds exist when the one named does not. The resolver already produced that
  line; the object lookup that ran next overwrote it with "no world available
  to search for the actor", so the diagnosis was computed and then discarded.
  Only visible once the `world` enum stopped rejecting those ids earlier.

- `build_all.ps1` no longer reports a pass for a build that did not happen. An
  engine that was not installed was skipped with a warning and never recorded,
  so a machine with neither engine printed "BUILD GATE PASSED: all engines
  compiled" having compiled nothing. A missing engine is now a failure.

- `navigate_to` stops guessing when a pawn stalls on the navmesh. Standing on
  mesh and motionless was reported as "the path to the goal could not be
  built", which was an assumption - and on a real project it was the wrong one.
  A complete route existed the whole time; the game was holding the player still
  through a scripted intro, and the message sent the investigation into nav
  volumes and tile generation for hours while the cause sat in the game's own
  log. It now asks the navigation system for the route before blaming the route,
  and says which of the two it is: no route, or a route the pawn did not move
  along. They need completely different things done about them.

- The `evidence_on_failure` bundle re-reads every property the scenario itself
  asserted on or waited for, taken in the failed world before teardown removes
  it. The bundle already said what the frame looked like, what was near the
  player and what the log complained about; what it did not carry was the
  number the assertion was actually about. A run that stops at “expected
  bIsDead to be true, got false” and then tears down the session has destroyed
  the only world in which the enemy’s health could be read — and whether it
  took damage and refused to die, or never took any, is the next question. An
  agent diagnosing a benchmark reported rebuilding the whole fight by hand for
  that number, three rounds running. Targets come from the scenario: whatever
  it read or waited on is, by construction, what it considers worth knowing.
- `get_property` echoes the property path in its result. A reply of
  `{object, type: float, value: 1}` does not say *which* float, which matters
  as soon as a caller reads a batch of them — the evidence bundle above returns
  one per assertion.

- Property paths index array elements: `Items[0].ItemData.HealPercent` reads
  through an array, into the struct in that element, across the object
  reference it holds, and down to a float. `get_property`, `set_property` and
  `wait_until` share one resolver, so all three gained it at once. The value
  worth asserting on in a game is usually inside one element of a list — an
  inventory slot, a spawn entry, a waypoint — and without this a caller could
  only fetch the whole array and pick through the JSON afterwards, which an
  assertion cannot do at all. Misuse is refused with the fact that identifies
  it: an index past the end says how many elements there really are, because
  “not found” on an empty inventory reads as a mistyped path rather than as the
  empty inventory it is describing; indexing a non-array names the type it
  actually is.

- `wait_until`’s `property_equals` condition takes the same dotted property
  paths `get_property` and `set_property` do. It looked the name up directly on
  the class, so the one tool whose entire job is asserting was the only one that
  could not reach a nested value — a GAS attribute at
  `AttributeSet.Health.CurrentValue` was simply unassertable, and the failure
  said “property not found”, which reads as a wrong property name rather than a
  missing capability. All three now share `ResolvePropertyPath`.

### Added
- `spawn_volume` — a volume actor with real brush geometry: trigger volumes,
  nav mesh bounds, blocking volumes, post-process volumes, anything deriving
  from `ABrush`. `spawn_actor` reaches those classes perfectly well and
  produces a volume that bounds nothing: `SpawnActor` never runs the
  brush-builder step the editor’s placement flow does, so `BrushBuilder`,
  `Brush` and `BrushComponent.Brush` all come back empty. The actor is there,
  correctly named and positioned, with no shape — a nav bounds volume that
  generates no navmesh, a trigger that overlaps nobody, and nothing anywhere
  reporting a fault. Found while building a test level whose `navigate_to`
  refused to path because the nav volume enclosed nothing. The tool reports the
  polygon count and returns an error rather than success when the builder
  produced no geometry, because claiming success on an empty volume is the
  failure it exists to remove.

### Added
- `level_diff` — every property on a placed actor that differs from what its
  class says it should be: the details panel’s yellow revert-arrow, as data. A
  level is mostly defaults, and the overrides are the short list of decisions
  somebody actually made in it — which is where “this worked yesterday” nearly
  always turns out to live. Components are compared against their own archetype
  (the Blueprint’s SCS template) rather than the class default, so a value the
  Blueprint sets is not reported as a level override. Placement transforms are
  off by default and editor-only components are skipped entirely: on a
  six-actor test level that took the report from 33 rows to 8, and the one
  injected fault appeared as the 9th.
- `wait_until` gains a `ui_visible {contains}` condition, matching a live
  widget’s name or the text it displays. A menu is constructed a frame or two
  after the screen holding it is added, and a key sent into that gap reaches
  the viewport instead of the menu — and still answers handled. Waiting on a
  widget rather than on a stopwatch is the fix; measured on a shipping title,
  where the stopwatch version silently did nothing twice.
- `cancellableHint` in tool annotations. `CanCancel()` was a runtime virtual
  with no static counterpart, so a client could not tell which tools a stop
  button would actually stop. Tools registered through `RegisterQuick` declare
  `false`: they run to completion inside one call, so there is no moment at
  which a cancel could arrive and change the outcome.

### Changed
- `run_scenario` assertion failures name what they expected and what they got —
  `expected value to be true, got false` rather than `expectation not met:
  value`. A scenario with four `get_property` assertions in a row reported the
  same six words four different ways, leaving the step index as the only thing
  telling them apart.

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
