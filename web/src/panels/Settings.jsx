import { useEffect, useState } from 'react'
import { api, JOINTS } from '../api.js'

export default function Settings({ run }) {
  const [c, setC] = useState(null)
  useEffect(() => { api.getConfig().then(setC).catch(() => {}) }, [])
  if (!c) return <section className="card"><h2>Settings</h2><p className="hint">loading…</p></section>

  // home/drop are taught on the Teach panel — never send stale copies back
  const save = () => { const { home, drop, ...rest } = c; return api.setConfig(rest) }
  const set = (k, v) => setC({ ...c, [k]: v })
  const setIdx = (k, i, v) => set(k, c[k].map((x, j) => (j === i ? v : x)))
  const numIn = (k, label) => (
    <label className="field">{label}<input type="number" value={c[k]} onChange={e => set(k, +e.target.value)} /></label>
  )

  return (
    <section className="card">
      <h2>Settings</h2>
      <div className="fields">
        {c.slot_name.map((n, i) => (
          <label className="field" key={i}>Color {i + 1} spoken name
            <input value={n} maxLength={15} onChange={e => setIdx('slot_name', i, e.target.value.toLowerCase())} />
          </label>
        ))}
        {numIn('grip_open', 'Gripper open')}
        {numIn('grip_close', 'Gripper closed')}
        {numIn('move_ms', 'Move time ms')}
        {numIn('grip_ms', 'Grip time ms')}
      </div>
      <h3>Lift offset <small>added to grasp pose for the approach</small></h3>
      <div className="fields six">
        {JOINTS.map((j, i) => (
          <label className="field" key={i}>{j}
            <input type="number" value={c.lift[i]} onChange={e => setIdx('lift', i, +e.target.value)} />
          </label>
        ))}
      </div>
      <div className="btns">
        <button className="primary" onClick={() => run(save())}>Save settings</button>
        <button onClick={() => run(save().then(() => api.teach('test_lift')))}>Save + test lift from here</button>
      </div>
    </section>
  )
}
