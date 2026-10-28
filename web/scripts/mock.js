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
  net: { ip: '127.0.0.1', mode: 'sta' },
  fw: { version: 'mock-1', partition: 'ota_0' },
}
const cfg = { home: [500, 500, 500, 500, 500, 500], drop: [500, 500, 500, 500, 500, 800],
  lift: [0, 0, 0, 0, -120, 0], grip_open: 200, grip_close: 650, move_ms: 700, grip_ms: 350, slot_name: ['red', 'blue'] }

createServer((req, res) => {
  let b = ''
  req.on('data', d => (b += d))
  req.on('end', () => {
    const body = b && req.url !== '/api/ota' ? JSON.parse(b) : {}   // OTA body is the raw .bin, not json
    const send = (o) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(o)) }
    st.cam.slots[0].x = 80 + Math.round(5 * Math.sin(Date.now() / 700))
    switch (`${req.method} ${req.url}`) {
      case 'GET /api/state': return send(st)
      case 'POST /api/cmd': st.voice.last = ''; st.detail = body.text; return send({ ok: true, msg: `ok: ${body.text}` })
      case 'POST /api/jog': if (body.id) st.arm.pos[body.id - 1] = body.pos; return send({ ok: true, msg: 'ok' })
      case 'POST /api/torque': st.arm.torque = body.on; return send({ ok: true, msg: body.on ? 'holding' : 'limp' })
      case 'POST /api/teach':
        if (body.what === 'calib_mark') st.marked = true
        if (body.what === 'calib_add') { st.calib.push({ px: st.cam.slots[0].x, py: 60 }); st.marked = false }
        if (body.what === 'calib_clear') st.calib = []
        return send({ ok: true, msg: body.what })
      case 'POST /api/ota': setTimeout(() => (st.fw.version = `mock-${Date.now() % 10000}`), 1500); return send({ ok: true, msg: 'updated — rebooting' })
      case 'GET /api/config': return send(cfg)
      case 'POST /api/config': Object.assign(cfg, body); st.cam.slots.forEach((s, i) => (s.name = cfg.slot_name[i])); return send({ ok: true, msg: 'saved' })
    }
    res.statusCode = 404; res.end()
  })
}).listen(8787, () => console.log('mock firmware on http://localhost:8787'))
