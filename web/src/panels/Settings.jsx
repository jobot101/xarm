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
        {numIn('stack_pct', 'Tower: block height (% of lift)')}
      </div>
      <h3>Camera</h3>
      <label className="check"><input type="checkbox" checked={!!c.cam_on_head} onChange={e => set('cam_on_head', e.target.checked)} /> Camera rides on the arm's head (uses the Look pose)</label>
      {c.cam_on_head && (
        <div className="fields six">
          {['Center', 'Look left', 'Look right'].map((l, i) => (
            <label className="field" key={i}>{l} (base Δ)
              <input type="number" value={c.scan?.[i] ?? 0} onChange={e => setIdx('scan', i, +e.target.value)} />
            </label>
          ))}
        </div>
      )}
      <h3>Reach direction <small>what “Out” adds per step on the drive pad</small></h3>
      <div className="fields six">
        {JOINTS.map((j, i) => (
          <label className="field" key={i}>{j}
            <input type="number" value={c.reach?.[i] ?? 0} onChange={e => setIdx('reach', i, +e.target.value)} />
          </label>
        ))}
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
