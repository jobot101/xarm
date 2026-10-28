import { useRef, useState } from 'react'
import { api, JOINTS } from '../api.js'
import BigSlider from './BigSlider.jsx'

const SEND_EVERY_MS = 100
const SPANS = [['Fine', 80], ['Normal', 200], ['Coarse', 500]]   // units per full-width swipe

// one big slider per servo, top of the arm first
export default function Joints({ s, run, title = 'Joints', torqueBtn = false, disabled = false }) {
  const pos = s?.arm?.pos || [500, 500, 500, 500, 500, 500]
  const limp = s?.arm?.torque === false
  const [span, setSpan] = useState(1)
  const last = useRef({})

  const send = (i, v, force) => {
    const now = Date.now()
    if (force || !last.current[i] || now - last.current[i] > SEND_EVERY_MS) {
      last.current[i] = now
      api.jog(i + 1, v, 150).catch(() => {})
    }
  }
  const order = [5, 4, 3, 2, 1, 0]   // base, shoulder, elbow, wrist, wrist rot, gripper

  return (
    <section className="card joints">
      <h2>{title}
        <span className="seg">
          {SPANS.map(([n], k) => <button key={n} className={k === span ? 'on' : ''} onClick={() => setSpan(k)}>{n}</button>)}
        </span>
        {torqueBtn && <button className={`pill ${limp ? 'warn' : ''}`} onClick={() => run(api.torque(limp))}>
          {limp ? 'Limp — tap to hold' : 'Holding — tap to go limp'}
        </button>}
      </h2>
      {order.map(i => (
        <BigSlider key={i} label={JOINTS[i]} value={pos[i]} disabled={disabled || limp || !s}
          span={SPANS[span][1]} onMove={v => send(i, v)} onDone={v => send(i, v, true)} />
      ))}
      <p className="hint">Drag anywhere on a bar — it moves from where the joint is, never jumps.</p>
    </section>
  )
}
