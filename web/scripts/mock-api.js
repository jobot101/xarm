// fake firmware API: data and answers, shared by scripts/mock.js (node) and src/preview.js (browser)
export const st = {
  state: 'idle', detail: '', busy: false, holding: false, marked: false,
  arm: { pos: [500, 500, 500, 500, 500, 500], torque: true, anim: true, link: true, battery_mv: 7480, lim: [[100, 800], [50, 950], [80, 920], [80, 920], [80, 920], [0, 1000]] },
  cam: { ok: false, enabled: false, raw: '50 3C 14 12 | 00 00 00 00', slots: [
    { name: 'red', seen: true, x: 80, y: 60, w: 20, h: 18 },
    { name: 'blue', seen: false, x: 0, y: 0, w: 0, h: 0 } ] },
  calib: [{ px: 30, py: 30 }, { px: 210, py: 30 }, { px: 120, py: 200 }],
  voice: { ok: true, last: 'pick up the red', status: 'listening — 24 phrases', level: 12, prob: 0, gain: 24, thresh: 22,
    wake: true, awake: false, vad: 'VADNet', dual: false, face: 1,
    cloud: { on: true, ready: false, key: '', status: 'no key — paste your Groq key below', last: '', ms: 0, today: 0, max: 1800 },
    src: 'flash', sd: { card: true, card_status: '16.0 GB card, 9.4 GB free', on: false, status: 'off' }, bank: false, bank_status: 'downloading Muffin\'s voice… 40%', heard: [{ p: 'dance robot', prob: 41, why: 'ok', ago: 3 }, { p: 'go home', prob: 27, why: 'Muffin was talking', ago: 12 }, { p: 'wave hello', prob: 44, why: 'asleep - say Hey Muffin', ago: 20 }, { p: 'can you pick up the red one', prob: 100, why: '☁ picking up red', ago: 25 }] },
  moves: { recording: false, frames: 0, list: [{ i: 0, name: 'robot' }] },
  kids: { cur: -1, list: [{ i: 0, name: 'sam', emoji: '🦖', jewel: 1, visits: 3, best_bells: 5, best_tower: 4 }] },
  net: { ip: '127.0.0.1', mode: 'sta', ssid: 'shop', online: true },
  fw: { version: 'mock-1', partition: 'ota_0', latest: 'mock-1', update: 'up to date (mock-1)', auto: true, sounds: '' },
}
export const school = { on: true, tap: true, can_pick: true, now: -1, minutes: 20, pass_set: false, state: 'waiting', kid: '', clock: 'internet', lessons_ready: true, lesson_count: 53,
  kids: [{ i: 0, name: 'sam', lesson: 4, stars: 19, streak: 3, used_s: 312, limit_s: 1200 }, { i: 12, name: 'guest', lesson: 0, stars: 0, streak: 0, used_s: 0, limit_s: 1200 }] }
// what Muffin is asking in class (school.c ask_json); the preview panel sets it
export const ASKS = {
  tf: { kind: 'tf', q: 12, options: [{ label: 'True', say: 'true' }, { label: 'False', say: 'false' }] },
  yn: { kind: 'yn', options: [{ label: 'Yes please', say: 'yes please' }, { label: 'No thanks', say: 'no thanks' }] },
  turn: { kind: 'turn', options: [{ label: 'Turn left', say: 'turn left' }, { label: 'Turn right', say: 'turn right' }] },
  nudge: { kind: 'turn', options: [{ label: 'Up', say: 'a little up' }, { label: 'Down', say: 'a little down' }, { label: 'Left', say: 'a little left' }, { label: 'Right', say: 'a little right' }] },
  stop: { kind: 'stop', options: [{ label: 'Stop!', say: 'stop' }] },
  free: { kind: 'free', options: [{ label: 'Dance!', say: 'lets dance' }, { label: 'Wave hello', say: 'wave hello' }, { label: 'Ring the bell', say: 'ring the bell' }, { label: 'Take a bow', say: 'take a bow' }, { label: 'Open hand', say: 'open your hand' }, { label: 'Close hand', say: 'close your hand' }] },
  fetch: { kind: 'fetch', options: [{ label: 'Clamp!', say: 'grab it' }] },
  giveback: { kind: 'giveback', options: [{ label: 'Let go', say: 'let go' }] },
  start: { kind: 'start', options: [{ label: 'Start class', say: 'lets go' }] },
}
school.ask = JSON.parse(JSON.stringify(ASKS.start))   // taps only, waiting: the Start class button
export const cfg = { home: [500, 500, 500, 500, 500, 500], drop: [500, 500, 500, 500, 500, 800],
  lift: [0, 0, 0, 0, -120, 0], reach: [0, 0, 0, -90, 90, 0], pick2: { align_axis: 1, align_turn: 375, grab_check: true, grab_margin: 40, grab_tries: 3, size_split: 900 }, grip_open: 200, grip_close: 650, move_ms: 700, grip_ms: 350, slot_name: ['red', 'blue'], cam_on_head: true, scan: [0, -200, 200], stack_pct: 30 }


// one request -> reply object, or undefined (404)
export function handle(method, url, body = {}) {
  st.cam.slots[0].x = 80 + Math.round(5 * Math.sin(Date.now() / 700))
  switch (`${method} ${url}`) {
    case 'GET /api/state': return (st)
    case 'POST /api/cmd': {
      if (body.voice && school.ask) {
        const o = school.ask.options.find(x => x.say === body.text)
        if (school.ask.kind === 'start') { school.state = 'in class'; school.can_pick = false; school.now = school.kids[0].lesson; delete school.ask }
        else if (school.ask.kind !== 'free') delete school.ask      // free play stays up: pick another trick
        return { ok: true, msg: o ? `answer: ${o.label}` : 'ok' }
      }
      const t = body.text, map = { 'build a tower': 'stacking', 'hold this': 'helping', 'dance': 'dancing', 'wave hello': 'waving', 'take a bow': 'bowing', 'ring the bell': 'ringing', 'sort everything': 'sorting', 'give it to me': 'giving' }
      if (t === 'stop') { st.state = 'stopped'; st.busy = false; return ({ ok: true, msg: 'stopped' }) }
      st.state = map[t] || (t.startsWith('pick') ? 'searching' : 'moving'); st.busy = true; st.detail = t
      setTimeout(() => { st.busy = false; st.state = t.startsWith('pick') ? 'holding' : 'idle'; st.holding = t.startsWith('pick') }, 2500)
      return ({ ok: true, msg: 'ok' })
    }
    case 'POST /api/jog': if (body.id) st.arm.pos[body.id - 1] = body.pos; return ({ ok: true, msg: 'ok' })
    case 'POST /api/torque': st.arm.torque = body.on; return ({ ok: true, msg: body.on ? 'holding' : 'limp' })
    case 'POST /api/teach':
      if (body.what === 'calib_mark') st.marked = true
      if (body.what === 'calib_add') { st.calib.push({ px: st.cam.slots[0].x, py: 60 }); st.marked = false }
      if (body.what === 'calib_clear') st.calib = []
      return ({ ok: true, msg: body.what })
    case 'POST /api/ota': setTimeout(() => (st.fw.version = `mock-${Date.now() % 10000}`), 1500); return ({ ok: true, msg: 'updated — rebooting' })
    case 'POST /api/wifi': st.net.ssid = body.ssid; return ({ ok: true, msg: 'saved — rebooting onto your WiFi' })
    case 'POST /api/update':
      if (typeof body.auto === 'boolean') { st.fw.auto = body.auto; return ({ ok: true, msg: body.auto ? 'auto-update on' : 'auto-update off' }) }
      st.fw.update = 'checking…'; setTimeout(() => (st.fw.update = `up to date (${st.fw.version})`), 1000); return ({ ok: true, msg: 'checking…' })
    case 'POST /api/moves': {
      const m = st.moves
      if (body.action === 'record') { m.recording = true; m.frames = 0; const t = setInterval(() => { if (!m.recording) return clearInterval(t); m.frames = Math.min(150, m.frames + 1) }, 200) }
      if (body.action === 'cancel') m.recording = false
      if (body.action === 'save') { m.recording = false; m.list.push({ i: m.list.length, name: body.name }) }
      if (body.action === 'delete') m.list = m.list.filter(x => x.i !== body.i)
      if (body.action === 'play') { st.state = 'playing'; st.busy = true; setTimeout(() => { st.state = 'idle'; st.busy = false }, 2000) }
      return ({ ok: true, msg: body.action === 'save' ? `saved "${body.name}"` : 'ok' })
    }
    case 'POST /api/drive': if (body.base) st.arm.pos[5] = Math.max(0, Math.min(1000, st.arm.pos[5] + body.base * body.step)); return ({ ok: true, msg: 'ok' })
    case 'GET /api/sys': return ({ core0: 92, core1: 48, heap_kb: 41, heap_min_kb: 12, heap_block_kb: 22, psram_kb: 5100, uptime_s: 900, state_ms: 38, tasks: [{ name: 'AFE_fetch', cpu: 61, core: 0, prio: 5, stack_free: 900 }, { name: 'voice', cpu: 40, core: 1, prio: 5, stack_free: 2100 }, { name: 'wifi', cpu: 9, core: 0, prio: 23, stack_free: 1800 }, { name: 'httpd', cpu: 6, core: -1, prio: 5, stack_free: 3000 }] })
    case 'GET /api/phrases': return (['stop', 'go home', 'dance robot', 'wave hello', 'thank you', 'move left', 'turn right'])
    case 'GET /api/i2c': return ({ found: '0x18 0x20 0x40 0x51 0x52', cam: true })
    case 'POST /api/limits': st.arm.lim[body.i] = [body.min, body.max]; return ({ ok: true, msg: 'limits saved' })
    case 'POST /api/voice': if (body.gain >= 0) st.voice.gain = body.gain; if (body.thresh >= 5) st.voice.thresh = body.thresh; if (body.wake === 0 || body.wake === 1) st.voice.wake = !!body.wake; if (body.dual === 0 || body.dual === 1) st.voice.dual = !!body.dual; if ([-1, 0, 1].includes(body.face)) st.voice.face = body.face; if (typeof body.stt_key === 'string') Object.assign(st.voice.cloud, body.stt_key ? { key: body.stt_key.slice(0, 4) + '…' + body.stt_key.slice(-4), ready: true, status: 'ready' } : { key: '', ready: false, status: 'no key — paste your Groq key below' }); if (body.cloud === 0 || body.cloud === 1) st.voice.cloud.on = !!body.cloud; if (body.sd === 0 || body.sd === 1) Object.assign(st.voice.sd, { on: !!body.sd, status: body.sd ? 'downloading full-quality voice… 12%' : 'off' }); return ({ ok: true, msg: body.dual !== undefined ? 'restarting' : 'voice tuned' })
    case 'POST /api/fx': if (typeof body.anim === 'boolean') st.arm.anim = body.anim; return ({ ok: true, msg: 'ok' })
    case 'POST /api/kids': {
      const k = st.kids
      if (body.action === 'add') { const i = k.list.length; k.list.push({ i, name: body.name.toLowerCase(), emoji: body.emoji, jewel: body.jewel, visits: 1, best_bells: 0, best_tower: 0 }); k.cur = i; return ({ ok: true, msg: `hi ${body.name}!` }) }
      if (body.action === 'select') { k.cur = body.i; k.list.find(x => x.i === body.i).visits++; return ({ ok: true, msg: 'hi!' }) }
      if (body.action === 'remove') k.list = k.list.filter(x => x.i !== body.i)
      if (body.action === 'score') { const me = k.list.find(x => x.i === k.cur); if (me && body.score > me.best_bells) me.best_bells = body.score }
      return ({ ok: true, msg: 'ok' })
    }
    case 'GET /api/school': return (school)
    case 'POST /api/school': {
      if (typeof body.on === 'boolean') school.on = body.on
      if (typeof body.tap === 'boolean') school.tap = body.tap
      if (typeof body.lesson === 'number') {
        if (!school.can_pick || body.lesson > school.kids[0].lesson) return { ok: false, msg: "that lesson isn't open yet" }
        school.state = 'in class'; school.can_pick = false; school.now = body.lesson; delete school.ask
        return { ok: true, msg: `lesson ${body.lesson + 1}` }
      }
      if (body.minutes) school.minutes = body.minutes
      if (typeof body.pass === 'string') school.pass_set = !!body.pass
      const k = school.kids.find(x => x.i === (body.more ?? body.reset_day ?? body.restart))
      if (k && body.more !== undefined) k.limit_s += 600
      if (k && body.reset_day !== undefined) k.used_s = 0
      if (k && body.restart !== undefined) Object.assign(k, { lesson: 0, stars: 0, streak: 0 })
      return ({ ok: true, msg: 'ok' })
    }
    case 'GET /api/config': return (cfg)
    case 'POST /api/config': Object.assign(cfg, body); st.cam.slots.forEach((s, i) => (s.name = cfg.slot_name[i])); return ({ ok: true, msg: 'saved' })
  }
}
