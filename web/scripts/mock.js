// fake firmware API so the UI can be developed with no hardware:
//   terminal 1: node scripts/mock.js      terminal 2: ARM=http://localhost:8787 npm run dev
import { createServer } from 'node:http'

const st = {
  state: 'idle', detail: '', busy: false, holding: false, marked: false,
  arm: { pos: [500, 500, 500, 500, 500, 500], torque: true, link: true, battery_mv: 7480 },
  cam: { ok: true, raw: '50 00 3C 00 14 00 12 00 | 00 00 00 00 00 00 00 00', slots: [
    { name: 'red', seen: true, x: 80, y: 60, w: 20, h: 18 },
    { name: 'blue', seen: false, x: 0, y: 0, w: 0, h: 0 } ] },
  calib: [{ px: 20, py: 20 }, { px: 140, py: 20 }, { px: 80, py: 100 }],
  voice: { ok: true, last: 'pick up the red' },
  moves: { recording: false, frames: 0, list: [{ i: 0, name: 'robot' }] },
  net: { ip: '127.0.0.1', mode: 'sta', ssid: 'shop', online: true },
  fw: { version: 'mock-1', partition: 'ota_0', latest: 'mock-1', update: 'up to date (mock-1)', auto: true },
}
const cfg = { home: [500, 500, 500, 500, 500, 500], drop: [500, 500, 500, 500, 500, 800],
  lift: [0, 0, 0, 0, -120, 0], grip_open: 200, grip_close: 650, move_ms: 700, grip_ms: 350, slot_name: ['red', 'blue'], cam_on_head: true, scan: [0, -200, 200] }

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
        const t = body.text, map = { 'dance': 'dancing', 'wave hello': 'waving', 'take a bow': 'bowing', 'ring the bell': 'ringing', 'sort everything': 'sorting', 'give it to me': 'giving' }
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
      case 'POST /api/fx': return send({ ok: true, msg: 'ok' })
      case 'GET /api/config': return send(cfg)
      case 'POST /api/config': Object.assign(cfg, body); st.cam.slots.forEach((s, i) => (s.name = cfg.slot_name[i])); return send({ ok: true, msg: 'saved' })
    }
    res.statusCode = 404; res.end()
  })
}).listen(8787, () => console.log('mock firmware on http://localhost:8787'))
