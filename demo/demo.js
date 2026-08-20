// Uplink demo - three acts, paced for screen recording.
//
//   Act 1  arrive      what is this project, and where does a player start?
//   Act 2  change      author a Timeline-driven platform from nothing
//   Act 3  prove       play it, and measure that the thing actually moves
//
// Every call here is a real MCP tool call against the running editor. Nothing
// is staged: the Blueprint does not exist when the script starts, and the
// numbers in Act 3 are read back out of the running game.
//
//   node demo.js            run it
//
// Reads UPLINK_URL and UPLINK_AUTH_TOKEN, the same two the Node bridge uses.
//   node demo.js --clean    remove what a previous run left behind
//   node demo.js --pace 2   seconds between beats (default 1.5)

const http = require("http");
const { URL } = require("url");

// The same two the Node bridge reads, so one setup serves both.
const ENDPOINT = new URL(process.env.UPLINK_URL || "http://127.0.0.1:3777");
const AUTH_TOKEN = process.env.UPLINK_AUTH_TOKEN || "";

const BP_PATH = "/Game/Demo/BP_DemoPlatform";
const BP = BP_PATH + ".BP_DemoPlatform";
const CUBE = "/Engine/BasicShapes/Cube.Cube";

const args = process.argv.slice(2);
const CLEAN_ONLY = args.includes("--clean");
const PACE = (() => {
  const i = args.indexOf("--pace");
  return i >= 0 && args[i + 1] ? Number(args[i + 1]) * 1000 : 1500;
})();

// ---------------------------------------------------------------------------

function call(tool, body = {}, timeoutMs = 120000) {
  return new Promise((resolve, reject) => {
    const payload = JSON.stringify(body);
    const req = http.request(
      {
        host: ENDPOINT.hostname,
        port: ENDPOINT.port || 80,
        path: "/tool/" + tool,
        method: "POST",
        headers: Object.assign(
          { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(payload) },
          AUTH_TOKEN ? { Authorization: "Bearer " + AUTH_TOKEN } : {}
        ),
        timeout: timeoutMs,
      },
      res => {
        let d = "";
        res.on("data", c => (d += c));
        res.on("end", () => {
          if (res.statusCode === 401) {
            reject(new Error(
              "the editor requires a bearer token: it was launched with UPLINK_AUTH_TOKEN set. " +
              "Set the same value in this shell's UPLINK_AUTH_TOKEN and run again."));
            return;
          }
          try { resolve(JSON.parse(d)); }
          catch { reject(new Error(`${tool}: unreadable reply (HTTP ${res.statusCode}): ${d.slice(0, 200)}`)); }
        });
      }
    );
    req.on("timeout", () => req.destroy(new Error(`${tool}: timed out`)));
    req.on("error", reject);
    req.write(payload);
    req.end();
  });
}

// A step that must work. Anything else stops the run, because a demo that
// carries on past a failed beat records a lie.
async function must(label, tool, body, timeoutMs) {
  const r = await call(tool, body, timeoutMs);
  if (!r.success) throw new Error(`${label}\n    ${tool} refused: ${r.message}`);
  return r.data || {};
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
const beat = () => sleep(PACE);

const say = s => console.log(s);
const act = (n, title) => say(`\n${"=".repeat(64)}\n  ACT ${n}  ${title}\n${"=".repeat(64)}`);
const step = s => say(`\n> ${s}`);
const note = s => say(`    ${s}`);

// ---------------------------------------------------------------------------

async function clean() {
  await call("pie_stop", { wait_ms: 40000 }).catch(() => {});
  await call("delete_actors", { names: ["DemoPlatform"], world: "editor" }).catch(() => {});
  await call("call_function", {
    object_path: "/Script/EditorScriptingUtilities.Default__EditorAssetLibrary",
    function: "DeleteDirectory",
    args: { DirectoryPath: "/Game/Demo" },
  }).catch(() => {});
  await call("save", {}).catch(() => {});
}

async function actOne() {
  act(1, "ARRIVE  —  an agent that has never seen this project");

  step("project_entry  — where does a player actually start?");
  const entry = await must("project_entry", "project_entry", {});
  note(`default map      ${entry.gameDefaultMap}`);
  note(`editor opened    ${entry.currentMap}`);
  note(`game mode        ${String(entry.globalDefaultGameMode).split("/").pop()}`);
  note(`playable maps    ${entry.mapsTotal}`);
  (entry.maps || []).forEach(m => note(`    ${m}`));
  await beat();
}

async function actTwo() {
  act(2, "CHANGE  —  author a moving platform from nothing");

  step("bp_create  — a new Actor Blueprint");
  await must("create the blueprint", "bp_create", { path: BP_PATH, parent_class: "/Script/Engine.Actor" });
  note("BP_DemoPlatform");
  await beat();

  step("bp_add_component  — something to look at");
  await must("add the mesh", "bp_add_component", {
    blueprint: BP, class: "StaticMeshComponent", name: "Platform",
    static_mesh: CUBE, scale: { x: 3, y: 3, z: 0.3 }, compile: true,
  });
  note("StaticMeshComponent 'Platform', a flat cube");
  await beat();

  step("bp_modify  — the variables it needs");
  await must("StartLocation", "bp_modify", { blueprint: BP, op: "add_variable", name: "StartLocation", type: "vector" });
  await must("Height", "bp_modify", {
    blueprint: BP, op: "add_variable", name: "Height", type: "float",
    default: "300.0", instance_editable: true, compile: true,
  });
  note("StartLocation (vector), Height (float, editable per instance)");
  await beat();

  step("bp_modify add_node kind:timeline  — a real Blueprint Timeline");
  const tl = await must("add the timeline", "bp_modify", {
    blueprint: BP, op: "add_node", kind: "timeline",
    name: "Move", length: 2.0, loop: true, autoplay: true, track: "Alpha",
    keys: [{ time: 0, value: 0 }, { time: 1, value: 1 }, { time: 2, value: 0 }],
    x: 300, y: 0, compile: true,
  });
  const pins = ((tl.node || {}).pins || []).map(p => p.name);
  note(`template + curve + node, pins: ${pins.join(", ")}`);
  note("'Alpha' is the curve's output — the pin the movement reads from");
  await beat();

  step("bp_modify  — the rest of the graph, one batched call");
  await must("add the nodes", "bp_modify", {
    blueprint: BP, compile: true,
    ops: [
      { op: "add_node", kind: "event", name: "ReceiveBeginPlay", x: -600, y: 0 },
      { op: "add_node", kind: "call_function", class: "/Script/Engine.Actor", function: "K2_GetActorLocation", x: -380, y: 150 },
      { op: "add_node", kind: "variable_set", name: "StartLocation", x: -180, y: 0 },
      { op: "add_node", kind: "variable_get", name: "StartLocation", x: 420, y: 220 },
      { op: "add_node", kind: "variable_get", name: "StartLocation", x: 420, y: 340 },
      { op: "add_node", kind: "variable_get", name: "Height", x: 420, y: 460 },
      { op: "add_node", kind: "call_function", class: "/Script/Engine.KismetMathLibrary", function: "MakeVector", x: 620, y: 440 },
      { op: "add_node", kind: "call_function", class: "/Script/Engine.KismetMathLibrary", function: "Add_VectorVector", x: 820, y: 360 },
      { op: "add_node", kind: "call_function", class: "/Script/Engine.KismetMathLibrary", function: "VLerp", x: 1020, y: 240 },
      { op: "add_node", kind: "call_function", class: "/Script/Engine.Actor", function: "K2_SetActorLocation", x: 1260, y: 60 },
    ],
  });
  note("BeginPlay, GetActorLocation, Set/Get variables, MakeVector, +, Lerp, SetActorLocation");
  await beat();

  step("bp_modify  — wire it up");
  // Guids are read back rather than assumed, and the two identical
  // 'Get StartLocation' nodes are told apart by where they were placed.
  const q = await must("read the graph", "bp_query", { blueprint: BP, graph: "EventGraph", max_nodes: 60 });
  const nodes = (q.graphs || []).flatMap(g => g.nodes || []);
  const one = t => {
    const m = nodes.filter(n => n.title === t);
    if (m.length !== 1) throw new Error(`expected one "${t}" node, found ${m.length}`);
    return m[0].guid;
  };
  const starts = nodes.filter(n => n.title === "Get StartLocation").sort((a, b) => a.y - b.y);
  if (starts.length !== 2) throw new Error(`expected two Get StartLocation nodes, found ${starts.length}`);

  const wires = [
    ["Event BeginPlay", "then", "Set StartLocation", "execute"],
    ["Get Actor Location", "ReturnValue", "Set StartLocation", "StartLocation"],
    ["Move", "Update", "Set Actor Location", "execute"],
    [starts[0].guid, "StartLocation", "Lerp (Vector)", "A"],
    [starts[1].guid, "StartLocation", "vector + vector", "A"],
    ["Get Height", "Height", "MakeVector", "Z"],
    ["MakeVector", "ReturnValue", "vector + vector", "B"],
    ["vector + vector", "ReturnValue", "Lerp (Vector)", "B"],
    ["Move", "Alpha", "Lerp (Vector)", "Alpha"],
    ["Lerp (Vector)", "ReturnValue", "Set Actor Location", "NewLocation"],
  ];
  const isGuid = v => /^[0-9A-F-]{30,}$/i.test(v);
  await must("wire the graph", "bp_modify", {
    blueprint: BP, compile: true, save: true,
    ops: wires.map(([f, fp, t, tp]) => ({
      op: "connect",
      from_node: isGuid(f) ? f : one(f), from_pin: fp,
      to_node: isGuid(t) ? t : one(t), to_pin: tp,
    })),
  });
  note(`${wires.length} connections, compiled clean, saved`);
  note("Timeline Update → Lerp(start, start + Z·Height) → SetActorLocation");
  await beat();
}

async function actThree() {
  act(3, "PROVE  —  does the Blueprint it just wrote actually work?");

  step("spawn_actor  — place one in the level");
  const spawned = await must("place the platform", "spawn_actor", {
    class_path: BP_PATH + ".BP_DemoPlatform_C",
    location: { x: 400, y: 0, z: 150 }, label: "DemoPlatform", world: "editor",
  });
  note(`${spawned.name} at z=150`);
  await beat();

  step("pie_start  — play it");
  await must("start play", "pie_start", { wait_ms: 60000 }, 120000);
  note("BeginPlay has run");
  await beat();

  step("get_property  — measure the platform over one loop");
  const zs = [];
  for (let i = 0; i < 10; i++) {
    const r = await call("get_property", {
      actor: "DemoPlatform", property: "RootComponent.RelativeLocation", world: "pie",
    });
    const v = (r.data || {}).value;
    if (v) { zs.push(Math.round(v.z)); process.stdout.write(`    z=${Math.round(v.z)}`); }
    await sleep(250);
  }
  say("");

  const low = Math.min(...zs), high = Math.max(...zs);
  note(`travelled ${high - low}cm, between z=${low} and z=${high}`);
  if (high - low < 50) {
    throw new Error("the platform did not move - the timeline is not driving the location");
  }
  note("the Blueprint an agent wrote sixty seconds ago is moving the world");
  await beat();

  step("observe  — the frame and the facts, one call, one moment");
  const o = await must("observe", "observe", { radius: 1200, max: 5, include: ["screenshot"] }, 60000);
  note(`player      ${o.player.pawn}  ${o.player.movement_mode}`);
  note(`nearby      ${(o.nearby || []).map(a => a.class).join(", ")}`);
  if (o.screen) note(`frame       ${o.screen.width}x${o.screen.height} from ${o.screen.source}`);
  await beat();

  await call("pie_stop", { wait_ms: 40000 }, 60000);
}

// ---------------------------------------------------------------------------

(async () => {
  // Distinguish the two reasons this fails. Reporting "not answering" for a
  // refused token sends you looking at the wrong thing entirely.
  let status;
  try {
    status = await call("status", {}, 15000);
  } catch (e) {
    console.error("\n" + e.message + "\n");
    process.exit(1);
  }
  if (!status || !status.success) {
    console.error(`\nNo editor answering on ${ENDPOINT.origin} - open it with the plugin enabled.\n`);
    process.exit(1);
  }

  await clean();
  if (CLEAN_ONLY) { say("cleaned."); return; }

  say(`\nUplink demo   —   ${status.data.project} on UE ${status.data.engine}`);

  await actOne();
  await actTwo();
  await actThree();

  say(`\n${"=".repeat(64)}`);
  say("  understand → author → run → observe — no leaving the conversation");
  say(`${"=".repeat(64)}\n`);
  say("run with --clean to remove what this left behind.\n");
})().catch(e => {
  console.error(`\nDEMO STOPPED: ${e.message}\n`);
  process.exit(1);
});
