import { useState } from 'react'
import { api } from '../api.js'

// what the vision module reports: color blobs (center x,y + size) in its own pixel space
const COLORS = ['#ff5a4e', '#4ea4ff']

const KNOWN = { '0x18': 'speaker codec', '0x20': 'IO expander', '0x40': 'mic codec', '0x51': 'clock', '0x52': 'CAMERA' }

export default function Vision({ s }) {
  const [scan, setScan] = useState(null)
  const doScan = async () => { setScan({ busy: true }); try { setScan(await api.i2c()) } catch (e) { setScan({ err: String(e.message || e) }) } }
  const slots = s?.cam?.slots || []
  const calib = s?.calib || []
  // Hiwonder color sketch runs a 240x240 frame (uint8 coords)
  const W = Math.max(240, ...slots.map(b => b.x + b.w / 2), ...calib.map(c => c.px))
  const H = Math.max(240, ...slots.map(b => b.y + b.h / 2), ...calib.map(c => c.py))

  return (
    <section className="card">
      <h2>Camera <small>{s?.cam?.ok ? `${calib.length} calib pts` : 'not responding'}</small></h2>
      <svg className="cam" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="xMidYMid meet">
        <rect width={W} height={H} className="cam-bg" />
        {calib.map((c, i) => (
          <g key={i} className="calpt">
            <circle cx={c.px} cy={c.py} r="2.5" />
            <text x={c.px + 4} y={c.py - 3}>{i + 1}</text>
          </g>
        ))}
        {slots.map((b, i) => b.seen && (
          <g key={i} stroke={COLORS[i]} fill="none">
            <rect x={b.x - b.w / 2} y={b.y - b.h / 2} width={b.w} height={b.h} strokeWidth="1.5" />
            <text x={b.x - b.w / 2} y={b.y - b.h / 2 - 3} fill={COLORS[i]} stroke="none">{b.name}</text>
          </g>
        ))}
      </svg>
      <div className="legend">
        {slots.map((b, i) => (
          <span key={i} style={{ color: COLORS[i] }}>
            ■ {b.name}: {b.seen ? `${b.x}, ${b.y} · ${b.w}×${b.h}` : 'not seen'}
          </span>
        ))}
      </div>
      <div className="btns" style={{ marginTop: 10 }}><button onClick={doScan} disabled={scan?.busy}>Scan I2C bus</button></div>
      {scan && !scan.busy && (scan.err ? <p className="hint">{scan.err}</p> : <>
        <p><b>{scan.cam ? 'Camera found at 0x52 ✓' : 'Camera NOT on the bus ✗'}</b></p>
        <p className="hint">{(scan.found || 'nothing answered').split(' ').map(a => KNOWN[a] ? `${a} ${KNOWN[a]}` : a).join(' · ')}</p>
        {!scan.cam && <p className="hint">Check: camera 5V + GND, its SDA→board SDA (IO11), SCL→board SCL (IO10), and that the camera was flashed with the color firmware.</p>}
      </>)}
      <details><summary>raw I2C</summary><code>{s?.cam?.raw}</code></details>
    </section>
  )
}
