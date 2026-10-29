// fake firmware API so the UI can be developed with no hardware:
//   terminal 1: node scripts/mock.js      terminal 2: ARM=http://localhost:8787 npm run dev
import { createServer } from 'node:http'

const st = {
  state: 'idle', detail: '', busy: false, holding: false, marked: false,
  arm: { pos: [500, 500, 500, 500, 500, 500], torque: true, link: true, battery_mv: 7480, lim: [[100, 800], [50, 950], [80, 920], [80, 920], [80, 920], [0, 1000]] },
  cam: { ok: true, raw: '50 3C 14 12 | 00 00 00 00', slots: [
    { name: 'red', seen: true, x: 80, y: 60, w: 20, h: 18 },
    { name: 'blue', seen: false, x: 0, y: 0, w: 0, h: 0 } ] },
  calib: [{ px: 30, py: 30 }, { px: 210, py: 30 }, { px: 120, py: 200 }],
  voice: { ok: true, last: 'pick up the red', status: 'listening — 24 phrases', level: 12, prob: 0, gain: 30, thresh: 20 },
  moves: { recording: false, frames: 0, list: [{ i: 0, name: 'robot' }] },
  kids: { cur: -1, list: [{ i: 0, name: 'sam', emoji: '🦖', jewel: 1, visits: 3, best_bells: 5, best_tower: 4 }] },
  net: { ip: '127.0.0.1', mode: 'sta', ssid: 'shop', online: true },
  fw: { version: 'mock-1', partition: 'ota_0', latest: 'mock-1', update: 'up to date (mock-1)', auto: true },
}
const cfg = { home: [500, 500, 500, 500, 500, 500], drop: [500, 500, 500, 500, 500, 800],
  lift: [0, 0, 0, 0, -120, 0], reach: [0, 0, 0, -90, 90, 0], pick2: { align_axis: 1, align_turn: 375, grab_check: true, grab_margin: 40, grab_tries: 3, size_split: 900 }, grip_open: 200, grip_close: 650, move_ms: 700, grip_ms: 350, slot_name: ['red', 'blue'], cam_on_head: true, scan: [0, -200, 200], stack_pct: 30 }

createServer((req, res) => {
  let b = ''
  req.on('data', d => (b += d))
  req.on('end', () => {
    const body = b && req.url !== '/api/ota' ? JSON.parse(b) : {}   // OTA body is the raw .bin, not json
    const send = (o) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(o)) }
    st.cam.slots[0].x = 80 + Math.round(5 * Math.sin(Date.now() / 700))
    switch (`${req.method} ${req.url}`) {
      case 'GET /api/state': return send(st)
      case 'POST /api/cmd': {
        const t = body.text, map = { 'build a tower': 'stacking', 'hold this': 'helping', 'dance': 'dancing', 'wave hello': 'waving', 'take a bow': 'bowing', 'ring the bell': 'ringing', 'sort everything': 'sorting', 'give it to me': 'giving' }
        if (t === 'stop') { st.state = 'stopped'; st.busy = false; return send({ ok: true, msg: 'stopped' }) }
        st.state = map[t] || (t.startsWith('pick') ? 'searching' : 'moving'); st.busy = true; st.detail = t
        setTimeout(() => { st.busy = false; st.state = t.startsWith('pick') ? 'holding' : 'idle'; st.holding = t.startsWith('pick') }, 2500)
        return send({ ok: true, msg: 'ok' })
      }
      case 'POST /api/jog': if (body.id) st.arm.pos[body.id - 1] = body.pos; return send({ ok: true, msg: 'ok' })
      case 'POST /api/torque': st.arm.torque = body.on; return send({ ok: true, msg: body.on ? 'holding' : 'limp' })
      case 'POST /api/teach':
        if (body.what === 'calib_mark') st.marked = true
        if (body.what === 'calib_add') { st.calib.push({ px: st.cam.slots[0].x, py: 60 }); st.marked = false }
        if (body.what === 'calib_clear') st.calib = []
        return send({ ok: true, msg: body.what })
      case 'POST /api/ota': setTimeout(() => (st.fw.version = `mock-${Date.now() % 10000}`), 1500); return send({ ok: true, msg: 'updated — rebooting' })
      case 'POST /api/wifi': st.net.ssid = body.ssid; return send({ ok: true, msg: 'saved — rebooting onto your WiFi' })
      case 'POST /api/update':
        if (typeof body.auto === 'boolean') { st.fw.auto = body.auto; return send({ ok: true, msg: body.auto ? 'auto-update on' : 'auto-update off' }) }
        st.fw.update = 'checking…'; setTimeout(() => (st.fw.update = `up to date (${st.fw.version})`), 1000); return send({ ok: true, msg: 'checking…' })
      case 'POST /api/moves': {
        const m = st.moves
        if (body.action === 'record') { m.recording = true; m.frames = 0; const t = setInterval(() => { if (!m.recording) return clearInterval(t); m.frames = Math.min(150, m.frames + 1) }, 200) }
        if (body.action === 'cancel') m.recording = false
        if (body.action === 'save') { m.recording = false; m.list.push({ i: m.list.length, name: body.name }) }
        if (body.action === 'delete') m.list = m.list.filter(x => x.i !== body.i)
        if (body.action === 'play') { st.state = 'playing'; st.busy = true; setTimeout(() => { st.state = 'idle'; st.busy = false }, 2000) }
        return send({ ok: true, msg: body.action === 'save' ? `saved "${body.name}"` : 'ok' })
      }
      case 'POST /api/drive': if (body.base) st.arm.pos[5] = Math.max(0, Math.min(1000, st.arm.pos[5] + body.base * body.step)); return send({ ok: true, msg: 'ok' })
      case 'GET /api/phrases': return send(['stop', 'go home', 'dance robot', 'wave hello', 'thank you', 'move left', 'turn right'])
      case 'GET /api/i2c': return send({ found: '0x18 0x20 0x40 0x51 0x52', cam: true })
      case 'POST /api/limits': st.arm.lim[body.i] = [body.min, body.max]; return send({ ok: true, msg: 'limits saved' })
      case 'POST /api/voice': if (body.gain >= 0) st.voice.gain = body.gain; if (body.thresh >= 5) st.voice.thresh = body.thresh; return send({ ok: true, msg: 'voice tuned' })
      case 'POST /api/fx': return send({ ok: true, msg: 'ok' })
      case 'POST /api/kids': {
        const k = st.kids
        if (body.action === 'add') { const i = k.list.length; k.list.push({ i, name: body.name.toLowerCase(), emoji: body.emoji, jewel: body.jewel, visits: 1, best_bells: 0, best_tower: 0 }); k.cur = i; return send({ ok: true, msg: `hi ${body.name}!` }) }
        if (body.action === 'select') { k.cur = body.i; k.list.find(x => x.i === body.i).visits++; return send({ ok: true, msg: 'hi!' }) }
        if (body.action === 'remove') k.list = k.list.filter(x => x.i !== body.i)
        if (body.action === 'score') { const me = k.list.find(x => x.i === k.cur); if (me && body.score > me.best_bells) me.best_bells = body.score }
        return send({ ok: true, msg: 'ok' })
      }
      case 'GET /api/config': return send(cfg)
      case 'POST /api/config': Object.assign(cfg, body); st.cam.slots.forEach((s, i) => (s.name = cfg.slot_name[i])); return send({ ok: true, msg: 'saved' })
    }
    res.statusCode = 404; res.end()
  })
}).listen(8787, () => console.log('mock firmware on http://localhost:8787'))
