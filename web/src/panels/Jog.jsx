import { useRef, useState } from 'react'
import { api, JOINTS } from '../api.js'

const SEND_EVERY_MS = 100

export default function Jog({ s, run }) {
  const pos = s?.arm?.pos || [500, 500, 500, 500, 500, 500]
  const limp = s?.arm?.torque === false
  const [drag, setDrag] = useState({})      // joint index -> value while the finger is down
  const last = useRef({})

  const move = (i, v, force) => {
    setDrag(d => ({ ...d, [i]: v }))
    const now = Date.now()
    if (force || !last.current[i] || now - last.current[i] > SEND_EVERY_MS) {
      last.current[i] = now
      api.jog(i + 1, v, 120).catch(() => {})
    }
  }
  const release = (i, v) => { move(i, v, true); setDrag(d => { const n = { ...d }; delete n[i]; return n }) }
  const nudge = (i, dv) => run(api.jog(i + 1, Math.min(1000, Math.max(0, pos[i] + dv)), 150))

  return (
    <section className="card">
      <h2>Jog
        <button className={`pill ${limp ? 'warn' : ''}`} onClick={() => run(api.torque(limp))}>
          {limp ? 'Limp — tap to hold' : 'Holding — tap to go limp'}
        </button>
      </h2>
      {JOINTS.map((name, i) => {
        const v = drag[i] ?? pos[i]
        return (
          <div className="joint" key={i}>
            <label>{i + 1} · {name}</label>
            <button onClick={() => nudge(i, -10)} disabled={limp}>−</button>
            <input type="range" min="0" max="1000" value={v} disabled={limp}
              onChange={e => move(i, +e.target.value)}
              onPointerUp={e => release(i, +e.target.value)}
              onKeyUp={e => release(i, +e.target.value)} />
            <button onClick={() => nudge(i, 10)} disabled={limp}>+</button>
            <output>{v}</output>
          </div>
        )
      })}
      <p className="hint">Limp mode lets you pose the arm by hand; the sliders follow it.</p>
    </section>
  )
}
