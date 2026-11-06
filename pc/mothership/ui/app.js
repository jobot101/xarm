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
function camLoop(img, note) {
  clearTimeout(camTimer);
  const next = () => { camTimer = setTimeout(() => camLoop(img, note), 250); };
  if (!document.body.contains(img)) return;
  const t = new Image();
  t.onload = () => { img.src = t.src; note.textContent = `camera ${S.cam && S.cam.ribbon || ""}: live`; next(); };
  t.onerror = () => { note.textContent = S.radar && !S.radar.present ? "camera resting: nobody at the bench" : "no camera picture"; camTimer = setTimeout(() => camLoop(img, note), 2000); };
  t.src = "/b/cam.jpg?t=" + Date.now();
}
function buildOverview(m) {
  const img = h("img", { class: "view", alt: "" }), note = h("div", { class: "viewnote" }, "waiting for a picture…");
  const out = h("div", { class: "reply" });
  const say = h("input", { type: "text", placeholder: "say to MotherShip, e.g. hold this for me", onkeydown: e => { if (e.key === "Enter") cmd(say.value, out); } });
  m.append(h("div", { class: "grid2" },
    card("What MotherShip sees", img, note, h("div", { class: "chips", id: "chips" })),
    card("Tell MotherShip",
      h("div", { class: "say" }, say, h("button", { onclick: () => cmd(say.value, out) }, "Send")), out,
      h("div", { class: "btns" }, QUICK.map(q => h("button", { class: "ghost", onclick: () => cmd(q, out) }, q))))));
  camLoop(img, note); chips();
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
let driving = null, driveBusy = false;
function buildDrive(m) {
  const speed = h("input", { type: "range", min: 3, max: 30, value: 10, oninput: () => $("#spv").textContent = speed.value }), out = h("div", { class: "reply" });
  const pad = h("div", { class: "pad" });
  for (const [axis, name, neg, pos] of DRIVE) {
    pad.append(h("span", { class: "lab" }, name));
    for (const [txt, sign] of [[neg, -1], [pos, 1]]) {
      const b = h("button", { class: "ghost" }, txt);
      const start = e => { e.preventDefault(); b.classList.add("held"); b.setPointerCapture && b.setPointerCapture(e.pointerId); driving = { axis, sign, speed, out }; driveTick(); };
      const stop = () => { b.classList.remove("held"); driving = null; };
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
  if (!driving) return;
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
    out, h("div", { class: "list", id: "tricks" })));
  syncTricks(true);
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
  const c = S.chime || { strength: 50, hit_ms: 110, gap_ms: 470, pattern: "xxx" };
  const mk = (label, key, lo, hi, unit) => {
    const r = h("input", { type: "range", min: lo, max: hi, value: c[key], "data-c": key }), v = h("span", { class: "val" }, c[key] + unit);
    r.addEventListener("input", () => v.textContent = r.value + unit);
    return h("div", { class: "slider" }, h("span", { class: "lab" }, label), r, v);
  };
  const pat = h("input", { type: "text", value: c.pattern, id: "pat" }), out = h("div", { class: "reply" });
  const plot = h("canvas", { class: "plot", width: 900, height: 130 });
  const body = () => ({ strength: +$("[data-c=strength]").value, hit_ms: +$("[data-c=hit_ms]").value, gap_ms: +$("[data-c=gap_ms]").value, pattern: pat.value.trim() || "x" });
  const ring = async () => {
    const b = body(); reply(out, `ringing ${b.pattern} …`);
    const r = await api("/api/chime", b);
    if (!r.ok) return reply(out, "chime: " + (r.msg || "refused"));
    await new Promise(res => setTimeout(res, 600));
    const until = Date.now() + 40000;
    while (Date.now() < until && S.state && S.state !== "idle") await new Promise(res => setTimeout(res, 300));
    await new Promise(res => setTimeout(res, 1200));
    const lv = await (await fetch(`/b/api/levels?since_ms=${Math.floor(r.t0_ms)}`)).json();
    reply(out, drawLevels(plot, lv, `${b.pattern} at ${b.strength}%`));
  };
  m.append(card("Chime",
    h("p", { class: "small-text muted" }, "The arm plays the chime from its taught strike point. Strength is how far it backs off before each strike."),
    mk("Strength", "strength", 10, 100, "%"), mk("Strike speed", "hit_ms", 50, 400, " ms"), mk("Beat", "gap_ms", 150, 1500, " ms"),
    h("div", { class: "row" }, h("span", { class: "lab muted", style: "width:120px" }, "Pattern"), pat,
      h("span", { class: "small-text muted" }, h("code", {}, "x"), " strike  ", h("code", {}, "!"), " accent  ", h("code", {}, "o"), " soft  ", h("code", {}, "."), " rest")),
    h("div", { class: "row" }, PRESETS.map(([n, p]) => h("button", { class: "small ghost", onclick: () => pat.value = p }, n))),
    h("div", { class: "row" },
      h("button", { onclick: ring }, "Ring"),
      h("button", { class: "ghost", onclick: async () => { const r = await api("/api/chime", { ...body(), save: true, play: false }); reply(out, r.ok ? `saved: “ring the bell” plays ${body().pattern}` : "not saved"); } }, "Save as default"),
      h("button", { class: "ghost", onclick: async () => { if (!confirm("Save the arm's pose right now as the chime's strike point?\n\nPose it first: closed gripper just touching the chime.")) return; reply(out, (await api("/api/teach", { what: "bell" })).msg); } }, "Teach strike point"),
      h("button", { class: "ghost", onclick: async () => { const r = await api("/api/bell", { ms: 60 }); reply(out, r.ok ? drawLevels(plot, r, "solenoid 60 ms") : "no solenoid in this firmware"); } }, "Solenoid test")),
    plot, out));
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
    if (first) go(section);                    // the section was drawn before the board answered: redraw with real values
    const busy = S.busy || (S.state && S.state !== "idle");
    pill("#p-state", busy ? "working" : "done", S.state || "?");
    pill("#p-brain", S.brain && S.brain.pc ? "done" : "failed", S.brain && S.brain.pc ? "brain connected" : "no brain");
    pill("#p-radar", S.radar && S.radar.present ? "done" : "incomplete", radarWords());
    const b = S.boot || {}, bad = /brownout|crash|watchdog|glitch/.test(b.reset || "") && (b.uptime_s || 0) < 600;
    pill("#p-fw", bad ? "failed" : "incomplete", `fw ${S.fw ? S.fw.version : "?"}` + (bad ? ` · reset: ${b.reset}` : ""));
    document.querySelectorAll(".nav .detail").forEach(d => d.textContent = SECTIONS[d.dataset.k].detail());
    chips(); syncJoints(); syncTricks(); syncSensors();
  } catch (e) {
    if (Date.now() - lastOk > 4000) pill("#p-state", "failed", "can't reach the board");
  }
  setTimeout(poll, 1000);
}
$("#stop").addEventListener("click", () => api("/api/cmd", { text: "stop" }));
document.addEventListener("keydown", e => { if (e.key === "Escape") api("/api/cmd", { text: "stop" }); });   // Esc = STOP
nav(); go(section); poll();
