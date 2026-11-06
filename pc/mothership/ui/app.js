// Shop MotherShip control panel. Talks to the board through server.py (/b/... -> http://<board>/...).
"use strict";
const $ = (s, el = document) => el.querySelector(s);
const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "class") el.className = v;
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c != null) el.append(c.nodeType ? c : document.createTextNode(c));
  return el;
};
const api = async (path, body) => {
  const r = await fetch("/b" + path, body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  const t = await r.text();
  try { return JSON.parse(t); } catch { return { ok: r.ok, msg: t.slice(0, 120) }; }
};

let S = {};                 // latest /api/state
let section = new URLSearchParams(location.search).get("s") || localStorage.getItem("ms-section") || "overview";
let lastOk = 0;

// ---------------------------------------------------------------- sections
const JOINTS = ["Gripper", "Wrist twist", "Wrist", "Elbow", "Shoulder", "Base"];
const QUICK = ["ring the bell", "hold this for me", "grab it", "let go", "put it down", "give it to me", "go home", "wave hello", "take a bow", "dance robot"];
const TEACH = [["Home", "home", "where it rests"], ["Drop", "drop", "where “put it down” sets things"], ["Hand-off", "give", "where “give it to me” hands you things"],
               ["Helper", "helper", "the “hold this for me” spot"], ["Strike point", "bell", "the chime: closed gripper just touching it"]];
const DRIVE = [["base", "Base", "◀ left", "right ▶"], ["lift", "Lift", "▼ down", "up ▲"], ["reach", "Reach", "◀ in", "out ▶"],
               ["tilt", "Wrist tilt", "▼", "▲"], ["twist", "Wrist twist", "↺", "↻"], ["grip", "Gripper", "open", "close"]];
const PRESETS = [["Single", "x"], ["Three", "xxx"], ["Double-double", "xx.xx"], ["Accent", "x.x..!"], ["Soft", "ooo"], ["Hour", "xxxxxxxxxxxx"]];

const SECTIONS = {
  overview: { name: "Overview", detail: () => S.state ? `${S.state} · ${S.detail || ""}` : "connecting", build: buildOverview },
  drive:    { name: "Drive", detail: () => S.arm ? `arm ${(S.arm.battery_mv / 1000).toFixed(1)} V · ${S.arm.torque ? "holding" : "limp"}` : "", build: buildDrive },
  joints:   { name: "Joints", detail: () => S.arm ? S.arm.pos.join(" · ") : "", build: buildJoints },
  teach:    { name: "Teach poses", detail: () => "home, drop, hand-off, helper, chime", build: buildTeach },
  tricks:   { name: "Tricks", detail: () => S.moves ? (S.moves.recording ? `recording · ${S.moves.frames} frames` : `${(S.moves.list || []).length} saved`) : "", build: buildTricks },
  chime:    { name: "Chime", detail: () => S.chime ? `${S.chime.pattern} · ${S.chime.strength}%` : "", build: buildChime },
  radar:    { name: "Radar scan", detail: () => radarWords(), build: buildRadar },
  sensors:  { name: "Mics & radar", detail: () => S.voice ? `${(S.voice.level * 0.6 - 60).toFixed(0)} dBFS · ${radarWords()}` : "", build: buildSensors },
  commands: { name: "All commands", detail: () => "everything it can do", build: buildCommands },
};

function radarWords() {
  const r = S.radar;
  if (!r || r.kind === "none") return "no radar";
  if (r.kind === "serial" && (!r.format || r.format === "unknown")) return "radar: reading…";
  if (!r.present) return "nobody";
  let w = "someone";
  if (r.distance_cm >= 0) w += ` ${(r.distance_cm / 100).toFixed(1)} m`;
  if (r.angle_deg != null) w += r.angle_deg ? ` ${Math.abs(r.angle_deg)}° ${r.angle_deg > 0 ? "right" : "left"}` : " ahead";
  return w;
}

function nav() {
  const n = $("#nav"); n.replaceChildren();
  for (const [k, s] of Object.entries(SECTIONS))
    n.append(h("button", { class: k === section ? "sel" : "", onclick: () => go(k) },
      h("span", { class: "name" }, s.name), h("span", { class: "detail", "data-k": k }, s.detail())));
}
function go(k) { section = k; localStorage.setItem("ms-section", k); nav(); const m = $("#main"); m.replaceChildren(); SECTIONS[k].build(m); }

const reply = (el, txt) => { el.textContent = txt; };
async function cmd(text, out) {
  text = text.trim(); if (!text) return;
  out && reply(out, `› ${text} …`);
  const r = await api("/api/cmd", { text });
  out && reply(out, `› ${text}: ${r.msg || (r.ok ? "ok" : "failed")}`);
}
const card = (title, ...kids) => h("section", { class: "card" }, h("span", { class: "step" }, title), ...kids);

// ---- overview
let camTimer = null;
// camera source: "obsbot" (a webcam on this PC: the OBSBOT, else the bench webcam; see webcam.py) or "board" (the
// ribbon camera, blurry but always there). The PC webcam falls back to the board while none is plugged in or free.
let camSrc = localStorage.getItem("ms-cam") || "obsbot", camFallback = false, camName = "";
function camLoop(img, note) {
  clearTimeout(camTimer);
  const next = ms => { camTimer = setTimeout(() => camLoop(img, note), ms); };
  if (!document.body.contains(img)) return;
  const useObsbot = camSrc === "obsbot" && !camFallback;
  const t = new Image();
  if (useObsbot && !camName) fetch("/webcam").then(r => r.json()).then(s => { camName = s.device || ""; }).catch(() => {});
  t.onload = () => { img.src = t.src; note.textContent = useObsbot ? `PC webcam${camName ? " (" + camName + ")" : ""}: live` : `board camera ${S.cam && S.cam.ribbon || ""}: live` + (camSrc === "obsbot" ? "  (PC webcam busy: falling back)" : ""); next(useObsbot ? 150 : 400); };
  t.onerror = async () => {
    if (useObsbot) {                              // find out why, then fall back to the board for a while
      let why = "PC webcam not available"; camName = "";
      try { const r = await fetch("/webcam"); const s = await r.json(); why = s.error || why; } catch {}
      note.textContent = why; camFallback = true; setTimeout(() => camFallback = false, 15000); next(300); return;
    }
    note.textContent = S.radar && !S.radar.present ? "camera resting: nobody at the bench" : "no camera picture"; next(2000);
  };
  t.src = (useObsbot ? "/webcam.jpg?t=" : "/b/cam.jpg?t=") + Date.now();
}
function camSwitch() {
  const b = h("div", { class: "row", style: "margin-top:6px" });
  for (const [k, label] of [["obsbot", "PC webcam"], ["board", "Board camera"]])
    b.append(h("button", { class: "small " + (camSrc === k ? "" : "ghost"), onclick: () => { camSrc = k; camFallback = false; localStorage.setItem("ms-cam", k); go("overview"); } }, label));
  return b;
}
function buildOverview(m) {
  const img = h("img", { class: "view", alt: "" }), note = h("div", { class: "viewnote" }, "waiting for a picture…");
  const out = h("div", { class: "reply" });
  const say = h("input", { type: "text", placeholder: "say to MotherShip, e.g. hold this for me", onkeydown: e => { if (e.key === "Enter") cmd(say.value, out); } });
  m.append(h("div", { class: "grid2" },
    card("What MotherShip sees", img, note, camSwitch(), h("div", { class: "chips", id: "chips" }),
      h("canvas", { class: "scope", style: "width:100%;height:220px;display:block;margin-top:4px" })),
    card("Tell MotherShip",
      h("div", { class: "say" }, say, h("button", { onclick: () => cmd(say.value, out) }, "Send")), out,
      h("div", { class: "btns" }, QUICK.map(q => h("button", { class: "ghost", onclick: () => cmd(q, out) }, q))))));
  camLoop(img, note); chips(); startScope();
}
function chips() {
  const c = $("#chips"); if (!c || !S.arm) return;
  const b = S.boot || {}, up = b.uptime_s || 0;
  c.replaceChildren(...[
    ["Arm", `${(S.arm.battery_mv / 1000).toFixed(1)} V`], ["Brain", S.brain && S.brain.pc ? "connected" : "not connected"],
    ["Radar", radarWords()], ["Mic", `${(S.voice.level * 0.6 - 60).toFixed(0)} dBFS`],
    ["Up", up >= 120 ? `${Math.floor(up / 60)} min` : `${up} s`], ["Last reset", b.reset || "?"],
  ].map(([k, v]) => h("div", { class: "chip" }, h("b", {}, v), k)));
}

// ---- drive: hold a button to move
let driving = null, driveBusy = false, driveLoop = false;
function buildDrive(m) {
  const speed = h("input", { type: "range", min: 3, max: 30, value: 10, oninput: () => $("#spv").textContent = speed.value }), out = h("div", { class: "reply" });
  const pad = h("div", { class: "pad" });
  for (const [axis, name, neg, pos] of DRIVE) {
    pad.append(h("span", { class: "lab" }, name));
    for (const [txt, sign] of [[neg, -1], [pos, 1]]) {
      const b = h("button", { class: "ghost" }, txt);
      const start = e => { e.preventDefault(); b.classList.add("held"); b.setPointerCapture && b.setPointerCapture(e.pointerId); driving = { axis, sign, speed, out }; if (!driveLoop) { driveLoop = true; driveTick(); } };
      const stop = () => { b.classList.remove("held"); if (driving) { driving = null; api("/api/drive", { base: 0, lift: 0, reach: 0, tilt: 0, twist: 0, grip: 0, ms: 120 }); } };
      b.addEventListener("pointerdown", start); b.addEventListener("pointerup", stop); b.addEventListener("pointercancel", stop); b.addEventListener("lostpointercapture", stop);
      pad.append(b);
    }
  }
  m.append(card("Drive", h("p", { class: "small-text muted" }, "Hold a button to move, let go to stop."), pad,
    h("div", { class: "slider" }, h("span", { class: "lab" }, "Speed"), speed, h("span", { class: "val", id: "spv" }, "10")),
    h("div", { class: "row" },
      h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/torque", { on: false })).msg) }, "Limp (pose by hand)"),
      h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/torque", { on: true })).msg) }, "Hold"),
      h("button", { class: "ghost", onclick: () => cmd("go home", out) }, "Go home")), out));
}
async function driveTick() {
  if (!driving) { driveLoop = false; return; }
  if (!driveBusy) {
    const { axis, sign, speed, out } = driving;
    driveBusy = true;
    api("/api/drive", { [axis]: sign, step: +speed.value, ms: 120 }).then(r => { if (!r.ok) reply(out, "drive: " + (r.msg || "refused")); }).finally(() => driveBusy = false);
  }
  setTimeout(driveTick, 100);
}

// ---- joints
let jDrag = -1, jSent = 0;
function buildJoints(m) {
  const rows = JOINTS.map((name, i) => {
    const r = h("input", { type: "range", min: 0, max: 1000, value: 500, "data-j": i });
    const v = h("span", { class: "val" }, "500");
    r.addEventListener("pointerdown", () => jDrag = i);
    for (const ev of ["pointerup", "pointercancel"]) r.addEventListener(ev, () => setTimeout(() => { if (jDrag === i) jDrag = -1; }, 400));
    r.addEventListener("input", () => { v.textContent = r.value; if (Date.now() - jSent > 100) { jSent = Date.now(); api("/api/jog", { id: i + 1, pos: +r.value }); } });
    r.addEventListener("change", () => { api("/api/jog", { id: i + 1, pos: +r.value }).finally(() => jDrag = -1); });
    return h("div", { class: "slider" }, h("span", { class: "lab" }, name), r, v);
  });
  m.append(card("Joints", h("p", { class: "small-text muted" }, "Drag a slider to move one joint. The arm follows smoothly."), ...rows));
  syncJoints();
}
function syncJoints() {
  if (!S.arm) return;
  document.querySelectorAll("input[data-j]").forEach(r => {
    const i = +r.dataset.j, lim = (S.arm.lim || [])[i];
    if (lim) { r.min = lim[0]; r.max = lim[1]; }
    if (jDrag !== i) { r.value = S.arm.pos[i]; r.nextSibling.textContent = S.arm.pos[i]; }
  });
}

// ---- teach
function buildTeach(m) {
  const out = h("div", { class: "reply" });
  m.append(card("Teach poses", h("p", { class: "small-text muted" }, "Pose the arm (Drive, Joints or Limp), then save where it is as:"),
    h("div", { class: "btns" }, TEACH.map(([lbl, w, why]) => h("button", { class: "ghost", onclick: async () => {
      if (!confirm(`Save the arm's pose right now as ${lbl} (${why})?`)) return;
      reply(out, (await api("/api/teach", { what: w })).msg || "saved"); } }, lbl))), out));
}

// ---- tricks
function buildTricks(m) {
  const name = h("input", { type: "text", placeholder: "name, e.g. salute" }), out = h("div", { class: "reply" });
  m.append(card("Tricks",
    h("p", { class: "small-text muted" }, "Record, move the arm (Limp and pose by hand, or Drive), then Save with a name. Then “MotherShip, do the <name>” plays it."),
    h("div", { class: "row" },
      h("button", { onclick: async () => reply(out, (await api("/api/moves", { action: "record" })).msg) }, "● Record"), name,
      h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/moves", { action: "save", name: name.value.trim() || "trick" })).msg) }, "Save"),
      h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/moves", { action: "cancel" })).msg) }, "Cancel")),
    out, h("div", { class: "list", id: "tricks" })),
    card("Hiwonder actions",
      h("p", { class: "small-text muted" }, "Ready-made sequences from Hiwonder, played from the PC. Half speed by default; kept inside the joint limits. They were made for an arm standing on a table, so watch the first run."),
      h("div", { class: "slider" }, h("span", { class: "lab" }, "Speed"), h("input", { type: "range", min: 10, max: 100, value: 50, id: "acspd", oninput: e => $("#acv").textContent = e.target.value + "%" }), h("span", { class: "val", id: "acv" }, "50%")),
      h("div", { class: "list", id: "actions" }, h("span", { class: "muted" }, "loading…")), h("div", { class: "reply", id: "acout" })));
  syncTricks(true);
  fetch("/actions").then(r => r.json()).then(d => {
    $("#actions").replaceChildren(...(d.actions.length ? d.actions.map(a => h("div", { class: "item" },
      h("span", { class: "nm" }, a.name, h("span", { class: "muted small-text" }, `  ${a.frames} frames · ${a.secs.toFixed(1)} s`)),
      h("button", { class: "small", onclick: async () => {
        if (!confirm(`Play “${a.name}” at ${$("#acspd").value}% speed?

Make sure the arm has room to swing.`)) return;
        const r = await (await fetch("/actions/play", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: a.name, speed: $("#acspd").value / 100 }) })).json();
        $("#acout").textContent = r.msg; } }, "Play"))) : [h("p", { class: "small-text muted" }, "No action files in johnny/mothership/actions.")]));
  }).catch(() => {});
}
let tricksSeen = "";
function syncTricks(force) {
  const box = $("#tricks"); if (!box || !S.moves) return;
  const list = S.moves.list || [], key = JSON.stringify(list);
  if (!force && key === tricksSeen) return;
  tricksSeen = key;
  box.replaceChildren(...(list.length ? list.map(t => h("div", { class: "item" }, h("span", { class: "nm" }, t.name),
    h("div", { class: "row" },
      h("button", { class: "small", onclick: () => api("/api/moves", { action: "play", i: t.i }) }, "Play"),
      h("button", { class: "small ghost", onclick: () => confirm(`Delete the trick “${t.name}”?`) && api("/api/moves", { action: "delete", i: t.i }) }, "Delete"))))
    : [h("p", { class: "small-text muted" }, "No tricks recorded yet.")]));
}

// ---- chime
function buildChime(m) {
  const c = S.chime || { strength: 15, hit_ms: 50, gap_ms: 470, pattern: "xxx" };
  const mk = (label, key, lo, hi, unit) => {
    const r = h("input", { type: "range", min: lo, max: hi, value: c[key], "data-c": key }), v = h("span", { class: "val" }, c[key] + unit);
    r.addEventListener("input", () => v.textContent = r.value + unit);
    return h("div", { class: "slider" }, h("span", { class: "lab" }, label), r, v);
  };
  // Strength = how hard it hits = how fast the strike comes in (hit_ms 400 gentle .. 50 full); Travel = distance
  const mkForce = hit => {
    const pct = Math.round((400 - hit) / 350 * 100), r = h("input", { type: "range", min: 0, max: 100, value: pct, id: "force" }), v = h("span", { class: "val" }, pct + "%");
    r.addEventListener("input", () => v.textContent = r.value + "%");
    return h("div", { class: "slider" }, h("span", { class: "lab" }, "Strength"), r, v);
  };
  const pat = h("input", { type: "text", value: c.pattern, id: "pat" }), out = h("div", { class: "reply" });
  const plot = h("canvas", { class: "plot", width: 900, height: 130 });
  const body = () => ({ strength: +$("[data-c=strength]").value, hit_ms: Math.round(400 - +$("#force").value / 100 * 350), gap_ms: +$("[data-c=gap_ms]").value, pattern: pat.value.trim() || "x" });
  const ring = async () => {
    const b = body(); reply(out, `ringing ${b.pattern} …`);
    const r = await api("/api/chime", b);
    if (!r.ok) return reply(out, "chime: " + (r.msg || "refused"));
    await new Promise(res => setTimeout(res, 600));
    const until = Date.now() + 40000;
    while (Date.now() < until && S.busy) await new Promise(res => setTimeout(res, 300));
    await new Promise(res => setTimeout(res, 1200));
    const lv = await (await fetch(`/b/api/levels?since_ms=${Math.floor(r.t0_ms || 0)}`)).json();
    reply(out, drawLevels(plot, lv, `${b.pattern}, strength ${$("#force").value}%, travel ${b.strength}%`));
  };
  m.append(card("Chime",
    h("p", { class: "small-text muted" }, "The arm plays the chime from its taught strike point with short, hard taps. Strength is how hard it hits (how fast the strike comes in); Travel is how far it backs off first, capped at 25% so it never swings the full distance."),
    mkForce(c.hit_ms), mk("Travel", "strength", 5, 25, "%"), mk("Beat", "gap_ms", 150, 1500, " ms"),
    h("div", { class: "row" }, h("span", { class: "lab muted", style: "width:120px" }, "Pattern"), pat,
      h("span", { class: "small-text muted" }, h("code", {}, "x"), " strike  ", h("code", {}, "!"), " accent  ", h("code", {}, "o"), " soft  ", h("code", {}, "."), " rest")),
    h("div", { class: "row" }, PRESETS.map(([n, p]) => h("button", { class: "small ghost", onclick: () => pat.value = p }, n))),
    h("div", { class: "row" },
      h("button", { onclick: ring }, "Ring"),
      h("button", { class: "ghost", onclick: async () => { const r = await api("/api/chime", { ...body(), save: true, play: false }); reply(out, r.ok ? `saved: “ring the bell” plays ${body().pattern}` : "not saved"); } }, "Save as default"),
      h("button", { class: "ghost", onclick: async () => { if (!confirm("Save the arm's pose right now as the chime's strike point?\n\nPose it first: closed gripper just touching the chime.")) return; reply(out, (await api("/api/teach", { what: "bell" })).msg); } }, "Teach strike point"),
      h("button", { class: "ghost", onclick: async () => { const r = await api("/api/bell", { ms: 12 }); reply(out, r.ok ? drawLevels(plot, r, "solenoid tap 12 ms") : "no solenoid in this firmware"); } }, "Solenoid test")),
    plot, out),
    buildSolenoid());
}

// ---- the solenoid (fixed at the chime): find the pulse where the plunger just reaches it, and save that
function buildSolenoid() {
  const out = h("div", { class: "reply" }), plot = h("canvas", { class: "plot", width: 900, height: 110 });
  const ms = h("input", { type: "range", min: 3, max: 30, value: 12, id: "pulse" }), pv = h("span", { class: "val" }, "12 ms");
  const set = v => { ms.value = Math.max(3, Math.min(30, v)); pv.textContent = ms.value + " ms"; };
  ms.addEventListener("input", () => set(+ms.value));
  const tap = async n => {
    const r = await api("/api/bell", { ms: +ms.value, n, gap: 450 });
    reply(out, r.ok ? drawLevels(plot, r, `tap ${ms.value} ms${n > 1 ? " ×" + n : ""}`) : "no solenoid in this firmware");
  };
  const find = async () => {
    if (!confirm("Tap the chime with short pulses from 3 ms upward, one at a time, and stop at the first one the mics hear clearly?\n\nKeep the shop quiet for about a minute.")) return;
    for (let p = 3; p <= 30; p++) {
      set(p); reply(out, `trying ${p} ms…`);
      const r = await api("/api/bell", { ms: p });
      if (!r.ok) return reply(out, "no solenoid in this firmware");
      drawLevels(plot, r, `${p} ms`);
      const s = ringStats(r.mic1, r.pre);
      if (s && s.rise >= 8) {                       // a clear hit over the room
        const pick = Math.min(30, p + 2);
        set(pick);
        return reply(out, `Strike point found: the plunger first reaches the chime at ${p} ms (${s.rise.toFixed(0)} dB over the room). Suggest ${pick} ms for a sure tap. Press Save pulse to keep it.`);
      }
      await new Promise(res => setTimeout(res, 300));
    }
    reply(out, "No clear hit up to 30 ms: check the solenoid's supply and wiring, or move it closer to the chime.");
  };
  return card("Solenoid (fixed at the chime)",
    h("p", { class: "small-text muted" }, "A short pulse throws the plunger only part way; a longer one reaches the chime. The strike point is the shortest pulse that clearly taps it. Pulses are capped at 30 ms."),
    h("div", { class: "slider" }, h("span", { class: "lab" }, "Pulse"), ms, pv),
    h("div", { class: "row" },
      h("button", { class: "small ghost", onclick: () => set(+ms.value - 1) }, "−1 ms"),
      h("button", { class: "small ghost", onclick: () => set(+ms.value + 1) }, "+1 ms"),
      h("button", { onclick: () => tap(1) }, "Tap"),
      h("button", { class: "ghost", onclick: () => tap(3) }, "Tap ×3"),
      h("button", { class: "ghost", onclick: find }, "Find strike point"),
      h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/bell", { ms: +ms.value, save: true, listen: false })).ok ? `saved: “ring the bell” taps at ${ms.value} ms` : "not saved") }, "Save pulse")),
    plot, out);
}
function ringStats(db, pre) {
  if (!db || !db.length || pre < 1 || db.length <= pre) return null;
  const room = [...db.slice(0, pre)].sort((a, b) => a - b)[Math.floor(pre / 2)], tail = db.slice(pre);
  let pk = 0; tail.forEach((v, i) => { if (v > tail[pk]) pk = i; });
  const stop = Math.max(room + 6, tail[pk] - 40);
  let end = tail.findIndex((v, i) => i > pk && v < stop); if (end < 0) end = tail.length;
  return { room, peak: tail[pk], rise: tail[pk] - room, ring: (end - pk) * 16 };
}
function drawLevels(cv, r, title) {
  const g = cv.getContext("2d"), W = cv.width, H = cv.height, lo = -80, hi = 0;
  const y = db => H - (Math.max(lo, Math.min(hi, db)) - lo) / (hi - lo) * H;
  g.clearRect(0, 0, W, H); g.font = "12px DM Sans, Segoe UI"; g.fillStyle = "#b0a28e";
  for (const d of [-60, -40, -20]) { g.strokeStyle = "rgba(184,146,58,.16)"; g.beginPath(); g.moveTo(0, y(d)); g.lineTo(W, y(d)); g.stroke(); g.fillText(d, W - 26, y(d) - 3); }
  const m1 = r.mic1 || [], m2 = r.mic2 || [], pre = r.pre || 0;
  if (pre && m1.length) { g.strokeStyle = "#b8923a"; g.setLineDash([4, 4]); g.beginPath(); g.moveTo(pre / m1.length * W, 0); g.lineTo(pre / m1.length * W, H); g.stroke(); g.setLineDash([]); }
  for (const [a, col] of [[m1, "#ff9b8a"], [m2, "#8fb3e0"]]) {
    if (a.length < 2 || Math.max(...a) <= -98) continue;
    g.strokeStyle = col; g.lineWidth = 2; g.beginPath(); a.forEach((v, i) => { const x = i / (a.length - 1) * W; i ? g.lineTo(x, y(v)) : g.moveTo(x, y(v)); }); g.stroke();
  }
  const s1 = ringStats(m1, pre), s2 = m2.length && Math.max(...m2) > -98 ? ringStats(m2, pre) : null;
  if (!s1) return `${title}: the mic didn't hear it`;
  return `${title}: mic 1 rang ${s1.rise.toFixed(0)} dB over the room for ${s1.ring} ms` + (s2 ? `; mic 2 ${s2.rise.toFixed(0)} dB (${(s2.peak - s1.peak).toFixed(1)} dB vs mic 1)` : "");
}

// ---- mics & radar
function buildSensors(m) {
  const out = h("div", { class: "reply" }), v = S.voice || {};
  const gain = h("input", { type: "range", min: 0, max: 37, value: v.gain || 24 }), gv = h("span", { class: "val" }, (v.gain || 24) + " dB");
  gain.addEventListener("input", () => gv.textContent = gain.value + " dB");
  const dual = h("input", { type: "checkbox", class: "toggle", checked: !!v.dual });
  dual.addEventListener("change", async () => {
    if (!confirm(`${dual.checked ? "Use both mics" : "Use one mic"}? MotherShip restarts (about 15 s).`)) { dual.checked = !dual.checked; return; }
    reply(out, (await api("/api/voice", { dual: dual.checked ? 1 : 0 })).msg);
  });
  m.append(h("div", { class: "grid2" },
    card("Mics",
      h("div", { class: "meter" }, h("i", { id: "meter" })), h("div", { class: "viewnote", id: "meterlbl" }, ""),
      h("div", { class: "slider" }, h("span", { class: "lab" }, "Gain"), gain, gv),
      h("div", { class: "row" },
        h("button", { class: "ghost", onclick: async () => reply(out, (await api("/api/voice", { gain: +gain.value })).msg || "set") }, "Set gain"),
        h("button", { class: "ghost", onclick: () => levelVoice(gain, gv, out) }, "Level to my voice (talk 5 s)")),
      h("label", { class: "check" }, dual, "Use both mics (restarts MotherShip)"), out),
    card("Radar", h("div", { id: "radarbox" }))));
  syncSensors();
}
async function levelVoice(gain, gv, out) {
  reply(out, "Talk normally from where you'll stand, for 5 seconds…");
  let peak = 0; const t0 = Date.now();
  while (Date.now() - t0 < 5000) { peak = Math.max(peak, (S.voice || {}).level || 0); await new Promise(r => setTimeout(r, 100)); }
  if (peak < 5) return reply(out, "Didn't hear anything. Check the mic or talk louder.");
  const db = peak * 0.6 - 60, want = Math.max(0, Math.min(37, Math.round(+gain.value + (-9 - db))));
  gain.value = want; gv.textContent = want + " dB";
  reply(out, `Your voice peaked at ${db.toFixed(0)} dBFS. Suggest ${want} dB (aim −9 dBFS). Press Set gain.`);
}
function syncSensors() {
  const mt = $("#meter"); if (mt && S.voice) { mt.style.width = S.voice.level + "%"; $("#meterlbl").textContent = `${(S.voice.level * 0.6 - 60).toFixed(0)} dBFS · ${S.voice.status || ""}`; }
  const rb = $("#radarbox"); if (!rb) return;
  const r = S.radar || {};
  rb.replaceChildren(h("h2", {}, h("span", { class: "dot " + (r.present ? "on" : r.kind === "serial" && !r.format ? "warn" : "") }), r.present ? "Someone's here" : "Nobody"),
    h("div", { class: "chips" }, [["Where", radarWords()], ["Module", (r.format || r.kind || "none") + (r.baud ? ` · ${r.baud} baud` : "")],
      ["For", `${r.since_s || 0} s`], ["Targets", r.targets != null ? r.targets : "–"]].map(([k, v]) => h("div", { class: "chip" }, h("b", {}, v), k))),
    h("p", { class: "small-text muted" }, "While someone's here it listens and follows with its base. After a minute of nobody, the camera rests and the ring dims; after three, a held part is put down."));
}

// ---- radar scope
const FOV = 60;                                   // LD2450: +/-60 degrees
let scopeData = null, scopeAt = 0, scopeLoop = null, trails = [];
function radarPts(r) {
  if (!r || !r.present) return [];
  if (Array.isArray(r.pts) && r.pts.length) return r.pts.map(p => ({ x: p[0], y: p[1], v: p[2] || 0, kind: p[4] || "" }));
  if (r.x_cm != null && r.y_cm != null) return [{ x: r.x_cm, y: r.y_cm, v: 0 }];      // older firmware: nearest only
  return [];
}
async function scopeFetch() {
  if (Date.now() - scopeAt < 180) return;
  scopeAt = Date.now();
  try { const r = await fetch("/b/api/radar", { cache: "no-store" }); if (r.ok) { const d = await r.json(); if (d && d.kind) scopeData = d; return; } } catch {}
  scopeData = S.radar || null;                    // no /api/radar on this firmware yet
}
function startScope() {
  cancelAnimationFrame(scopeLoop);
  const tick = t => {
    const cvs = document.querySelectorAll("canvas.scope");
    if (!cvs.length) return;
    scopeFetch();
    const pts = radarPts(scopeData || S.radar);
    pts.forEach(p => {                              // a point joins the trail whose newest point is closest (within 60 cm)
      let tr = null, best = 60;
      for (const t of trails) { const l = t[t.length - 1]; if (l) { const d = Math.hypot(l.x - p.x, l.y - p.y); if (d < best) { best = d; tr = t; } } }
      if (!tr) { if (trails.length >= 3) trails.shift(); tr = []; trails.push(tr); }
      const last = tr[tr.length - 1];
      if (!last || Math.hypot(last.x - p.x, last.y - p.y) > 2) { tr.push({ ...p, t: Date.now() }); if (tr.length > 40) tr.shift(); } });
    trails.forEach(tr => { while (tr.length && Date.now() - tr[0].t > 6000) tr.shift(); });
    cvs.forEach(c => drawScope(c, pts, t));
    scopeLoop = requestAnimationFrame(tick);
  };
  scopeLoop = requestAnimationFrame(tick);
}
function drawScope(c, pts, t) {
  const dpr = window.devicePixelRatio || 1, W = c.clientWidth, H = c.clientHeight;
  if (c.width !== W * dpr) { c.width = W * dpr; c.height = H * dpr; }
  const g = c.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
  const far = Math.max(200, ...pts.map(p => Math.hypot(p.x, p.y) * 1.3)), range = Math.min(600, Math.ceil(far / 100) * 100);
  const ox = W / 2, oy = H - 22, R = Math.min(H - 40, W / 2 / Math.sin(FOV * Math.PI / 180) - 10);
  const toXY = (x, y) => [ox + x / range * R, oy - y / range * R];
  const a0 = -Math.PI / 2 - FOV * Math.PI / 180, a1 = -Math.PI / 2 + FOV * Math.PI / 180;
  // field of view
  g.fillStyle = "rgba(184,146,58,.05)"; g.beginPath(); g.moveTo(ox, oy); g.arc(ox, oy, R, a0, a1); g.closePath(); g.fill();
  g.strokeStyle = "rgba(184,146,58,.22)"; g.lineWidth = 1; g.font = "11px DM Sans, Segoe UI"; g.fillStyle = "#b0a28e";
  const step = range <= 300 ? 50 : 100;
  for (let d = step; d <= range; d += step) {
    g.beginPath(); g.arc(ox, oy, d / range * R, a0, a1); g.stroke();
    if (d % 100 === 0) g.fillText(`${d / 100} m`, ox + 4, oy - d / range * R + 12);
  }
  for (const deg of [-60, -30, 0, 30, 60]) {
    const a = -Math.PI / 2 + deg * Math.PI / 180;
    g.beginPath(); g.moveTo(ox, oy); g.lineTo(ox + Math.cos(a) * R, oy + Math.sin(a) * R); g.stroke();
    if (deg) g.fillText(`${Math.abs(deg)}°`, ox + Math.cos(a) * (R + 2) - 8, oy + Math.sin(a) * (R + 2) - 2);
  }
  // sweep (for looks: the LD2450 sees the whole fan at once)
  const s = Math.sin(t / 900), sa = -Math.PI / 2 + s * FOV * Math.PI / 180, dir = Math.cos(t / 900) >= 0 ? -1 : 1;
  const grad = g.createRadialGradient(ox, oy, 0, ox, oy, R); grad.addColorStop(0, "rgba(168,201,138,.0)"); grad.addColorStop(1, "rgba(168,201,138,.22)");
  g.fillStyle = grad; g.beginPath(); g.moveTo(ox, oy); g.arc(ox, oy, R, Math.min(sa, sa + dir * 0.35), Math.max(sa, sa + dir * 0.35)); g.closePath(); g.fill();
  g.strokeStyle = "rgba(168,201,138,.6)"; g.beginPath(); g.moveTo(ox, oy); g.lineTo(ox + Math.cos(sa) * R, oy + Math.sin(sa) * R); g.stroke();
  // ignore zones (learned from an empty room) and the clutter inside them
  const z = (scopeData || S.radar || {}).zones || [];
  z.forEach(([x0, x1, y0, y1]) => {
    const [ax, ay] = toXY(x0, y1), [bx, by] = toXY(x1, y0);
    g.fillStyle = "rgba(110,99,82,.22)"; g.fillRect(ax, ay, bx - ax, by - ay);
    g.strokeStyle = "rgba(176,162,142,.5)"; g.setLineDash([2, 3]); g.strokeRect(ax, ay, bx - ax, by - ay); g.setLineDash([]);
    g.fillStyle = "#b0a28e"; g.font = "11px DM Sans, Segoe UI"; g.fillText("ignored", ax + 3, ay + 12);
  });
  ((scopeData || S.radar || {}).ignored || []).forEach(([x, y]) => {
    const [px, py] = toXY(x, y); g.fillStyle = "rgba(176,162,142,.7)"; g.beginPath(); g.arc(px, py, 4, 0, 7); g.fill();
  });
  // swinging in place (a cable, a fan): the radar sees it, MotherShip doesn't count it as a person
  ((scopeData || S.radar || {}).swinging || []).forEach(([x, y, , , x0, x1, y0, y1]) => {
    const [ax, ay] = toXY(x0, y1), [bx, by] = toXY(x1, y0), [px, py] = toXY(x, y);
    g.strokeStyle = "rgba(212,170,85,.6)"; g.setLineDash([4, 3]); g.strokeRect(ax - 4, ay - 4, bx - ax + 8, by - ay + 8); g.setLineDash([]);
    g.fillStyle = "#d4aa55"; g.beginPath(); g.arc(px, py, 5, 0, 7); g.fill();
    g.font = "12px DM Sans, Segoe UI"; g.fillText("swinging · not counted", bx + 8, ay + 8);
  });
  // trails and people
  trails.forEach(tr => tr.forEach((p, k) => {
    const [x, y] = toXY(p.x, p.y), age = (Date.now() - p.t) / 6000;
    g.fillStyle = `rgba(168,201,138,${(1 - age) * 0.35})`; g.beginPath(); g.arc(x, y, 3, 0, 7); g.fill();
  }));
  pts.forEach((p, i) => {
    const [x, y] = toXY(p.x, p.y), d = Math.hypot(p.x, p.y), ang = Math.round(Math.atan2(p.x, p.y) * 57.3);
    const glow = g.createRadialGradient(x, y, 0, x, y, 18); glow.addColorStop(0, "rgba(168,201,138,.9)"); glow.addColorStop(1, "rgba(168,201,138,0)");
    g.fillStyle = glow; g.beginPath(); g.arc(x, y, 18, 0, 7); g.fill();
    g.fillStyle = "#e8f5dc"; g.beginPath(); g.arc(x, y, 5, 0, 7); g.fill();
    g.fillStyle = "#f4efe6"; g.font = "13px DM Sans, Segoe UI";
    g.fillText(`${i + 1} · ${(d / 100).toFixed(1)} m ${ang ? Math.abs(ang) + "° " + (ang > 0 ? "R" : "L") : "ahead"}${p.kind === "person" ? " · walked in" : ""}${Math.abs(p.v) > 5 ? " · moving" : ""}`, x + 10, y - 8);
  });
  // MotherShip at the bottom, and the 45 cm "no reach" zone
  g.strokeStyle = "rgba(255,155,138,.5)"; g.setLineDash([3, 4]); g.beginPath(); g.arc(ox, oy, 45 / range * R, a0, a1); g.stroke(); g.setLineDash([]);
  g.fillStyle = "#b8923a"; g.beginPath(); g.arc(ox, oy, 7, 0, 7); g.fill();
  g.fillStyle = "#d4aa55"; g.font = "12px DM Sans, Segoe UI"; g.fillText("MotherShip", ox + 12, oy + 4);
  if (!pts.length) { g.fillStyle = "#b0a28e"; g.font = "15px Sorts Mill Goudy, Georgia"; g.fillText("nobody in view", ox - 50, oy - R * 0.55); }
}
function buildRadar(m) {
  m.append(card("Radar scan", h("canvas", { class: "scope", style: "width:100%;height:440px;display:block" }),
    h("div", { class: "chips", id: "radarchips" }),
    h("div", { class: "row" },
      h("button", { onclick: learnRoom }, "Learn empty room (30 s)"),
      h("button", { class: "ghost", onclick: ignoreSwinging }, "Always ignore what's swinging"),
      h("button", { class: "ghost", onclick: async () => { if (!confirm("Forget the ignore zones? Everything the radar sees will count as a person again.")) return;
        const r = await api("/api/radar", { zones: [] }); $("#zoneout").textContent = r.msg || "cleared"; } }, "Clear zones")),
    h("div", { class: "reply", id: "zoneout" }),
    h("p", { class: "small-text muted" }, "What the LD2450 sees: up to three people, with where they are and a short trail. \"Walked in\" means it came from further away. Gold dots are something swinging back and forth in place (a cable, a fan): MotherShip spots the steady rhythm after several swings and doesn't count it as a person. \"Always ignore what's swinging\" turns those into ignore zones, so they're left out from the start next time. Grey boxes are ignore zones (learned from an empty room: the arm's own motion, a fan); grey dots inside them don't count. The dashed red arc is 45 cm: closer than that MotherShip stops reaching toward you.")));
  startScope(); syncRadarChips();
}
async function learnRoom() {
  const out = $("#zoneout");
  if (!confirm("Leave the room (or at least the radar's view) for 30 seconds after you press OK.\n\nEverything the radar sees while you're gone becomes an ignore zone.")) return;
  const seen = [], t0 = Date.now();
  out.classList.add("busy-tool");
  try { while (Date.now() - t0 < 30000) {
    out.textContent = `listening to the empty room… ${Math.ceil((30000 - (Date.now() - t0)) / 1000)} s`;
    try {
      const r = await (await fetch("/b/api/radar", { cache: "no-store" })).json();
      for (const p of [...(r.pts || []), ...(r.ignored || []), ...(r.swinging || [])]) seen.push([p[0], p[1]]);
      if (!r.pts && r.present && r.x_cm != null) seen.push([r.x_cm, r.y_cm]);
    } catch {
      const r = S.radar || {}; if (r.present && r.x_cm != null) seen.push([r.x_cm, r.y_cm]);
    }
    await new Promise(res => setTimeout(res, 300));
  }
  } finally { out.classList.remove("busy-tool"); }
  if (!seen.length) { out.textContent = "The room was empty: nothing to ignore."; return; }
  // cluster: points within 40 cm of a cluster's box join it; keep clusters seen in at least 10% of the samples
  const cl = [];
  for (const [x, y] of seen) {
    let c = cl.find(c => x > c.x0 - 40 && x < c.x1 + 40 && y > c.y0 - 40 && y < c.y1 + 40);
    if (!c) cl.push(c = { x0: x, x1: x, y0: y, y1: y, n: 0 });
    c.x0 = Math.min(c.x0, x); c.x1 = Math.max(c.x1, x); c.y0 = Math.min(c.y0, y); c.y1 = Math.max(c.y1, y); c.n++;
  }
  const keep = cl.filter(c => c.n >= Math.max(3, seen.length * 0.1)).sort((a, b) => b.n - a.n).slice(0, 4)
                 .map(c => [Math.round(c.x0 - 10), Math.round(c.x1 + 10), Math.max(0, Math.round(c.y0 - 10)), Math.round(c.y1 + 10)]);
  const r = await api("/api/radar", { zones: keep });
  out.textContent = r.ok ? `${r.msg}: ${keep.map(z => `x ${z[0]}…${z[1]} cm, y ${z[2]}…${z[3]} cm`).join("; ")}`
                         : `couldn't save the zones: ${r.msg || "this firmware has no ignore zones yet"}`;
}

async function ignoreSwinging() {
  const out = $("#zoneout"), r = scopeData || S.radar || {};
  const sw = (r.swinging || []).map(([, , , , x0, x1, y0, y1]) => [Math.round(x0 - 10), Math.round(x1 + 10), Math.max(0, Math.round(y0 - 10)), Math.round(y1 + 10)]);
  if (!sw.length) { out.textContent = "Nothing's swinging right now."; return; }
  const zones = [...(r.zones || []), ...sw].slice(0, 4);
  const res = await api("/api/radar", { zones });
  out.textContent = res.ok ? `${res.msg}${zones.length < (r.zones || []).length + sw.length ? " (only 4 fit: the newest were left out)" : ""}`
                           : `couldn't save the zones: ${res.msg || "failed"}`;
}

function syncRadarChips() {
  const c = $("#radarchips"); if (!c) return;
  const r = scopeData || S.radar || {}, pts = radarPts(r);
  c.replaceChildren(...[["People", pts.length], ["Nearest", radarWords()], ["Module", (r.format || r.kind || "none") + (r.baud ? ` · ${r.baud} baud` : "")],
    ["Seen for", `${r.since_s || 0} s`]].map(([k, v]) => h("div", { class: "chip" }, h("b", {}, v), k)));
}

// ---- all commands
async function buildCommands(m) {
  const out = h("div", { class: "reply" }), box = h("div", { class: "btns" }, h("span", { class: "muted" }, "loading…"));
  m.append(card("All commands", h("p", { class: "small-text muted" }, "Everything MotherShip can do. Tap to run."), out, box));
  let brain = [];
  try { brain = await (await fetch("/commands")).json(); } catch {}
  const board = await api("/api/phrases"), tricks = ((S.moves || {}).list || []).map(t => `do the ${t.name}`);
  const all = [...new Set([...brain, ...tricks, ...(Array.isArray(board) ? board : [])])];
  box.replaceChildren(...all.map(p => h("button", { class: "ghost", onclick: () => cmd(p, out) }, p)));
}

// ---------------------------------------------------------------- status loop
function pill(id, cls, txt) { const p = $(id); p.className = "pill " + cls; p.textContent = txt; }
async function poll() {
  try {
    const r = await fetch("/b/api/state", { cache: "no-store" });
    if (!r.ok) throw new Error(r.status);
    const first = !lastOk;
    S = await r.json(); lastOk = Date.now();
    if (first && !document.querySelector(".busy-tool")) go(section);   // redraw with real values, unless a tool is mid-run
    const busy = S.busy || (S.state && S.state !== "idle");
    pill("#p-state", busy ? "working" : "done", S.state || "?");
    pill("#p-brain", S.brain && S.brain.pc ? "done" : "failed", S.brain && S.brain.pc ? "brain connected" : "no brain");
    pill("#p-radar", S.radar && S.radar.present ? "done" : "incomplete", radarWords());
    const b = S.boot || {}, bad = /brownout|crash|watchdog|glitch/.test(b.reset || "") && (b.uptime_s || 0) < 600;
    pill("#p-fw", bad ? "failed" : "incomplete", `fw ${S.fw ? S.fw.version : "?"}` + (bad ? ` · reset: ${b.reset}` : ""));
    document.querySelectorAll(".nav .detail").forEach(d => d.textContent = SECTIONS[d.dataset.k].detail());
    chips(); syncJoints(); syncTricks(); syncSensors(); syncRadarChips();
  } catch (e) {
    if (Date.now() - lastOk > 4000) pill("#p-state", "failed", "can't reach the board");
  }
  setTimeout(poll, 1000);
}
$("#stop").addEventListener("click", () => api("/api/cmd", { text: "stop" }));
document.addEventListener("keydown", e => { if (e.key === "Escape") api("/api/cmd", { text: "stop" }); });   // Esc = STOP
nav(); go(section); poll();
