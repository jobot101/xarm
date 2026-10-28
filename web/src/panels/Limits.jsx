import { useState } from 'react'
import { api, JOINTS } from '../api.js'

// soft limits per servo — the firmware clamps every move (sliders, drive, voice, tricks) to these
export default function Limits({ s, run }) {
  const pos = s?.arm?.pos || []
  const lim = s?.arm?.lim || []
  const [edit, setEdit] = useState({})     // i -> [min,max] being typed

  const cur = i => edit[i] || lim[i] || [0, 1000]
  const set = (i, k, v) => setEdit(e => ({ ...e, [i]: Object.assign([...cur(i)], { [k]: v }) }))
  const save = async (i, lo, hi) => {
    await run(api.limits(i, lo, hi))
    setEdit(e => { const n = { ...e }; delete n[i]; return n })
  }

  return (
    <section className="card">
      <h2>Servo limits</h2>
      {[5, 4, 3, 2, 1, 0].map(i => {
        const [lo, hi] = cur(i)
        return (
          <div className="limrow" key={i}>
            <b>{JOINTS[i]}</b> <small>now {pos[i] ?? '—'}</small>
            <div className="limctl">
              <input type="number" min="0" max="1000" value={lo} onChange={e => set(i, 0, +e.target.value)} />
              <button onClick={() => save(i, pos[i], hi)}>Min = now</button>
              <input type="number" min="0" max="1000" value={hi} onChange={e => set(i, 1, +e.target.value)} />
              <button onClick={() => save(i, lo, pos[i])}>Max = now</button>
              {edit[i] && <button className="primary" onClick={() => save(i, lo, hi)}>Save</button>}
            </div>
          </div>
        )
      })}
      <p className="hint">Jog a joint to the furthest safe spot, tap Min/Max = now. Every move — sliders, drive, voice, tricks — stays inside these.</p>
    </section>
  )
}
