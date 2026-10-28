import { useRef, useState } from 'react'

// Massive, low-sensitivity slider. Drag is relative: touching never jumps the joint,
// and a full-width swipe moves only `span` units (default 200 of 0..1000).
export default function BigSlider({ label, value, onMove, onDone, disabled, span = 200, lo = 0, hi = 1000 }) {
  const ref = useRef(null)
  const start = useRef(null)
  const [live, setLive] = useState(null)
  const v = live ?? value

  const down = e => {
    if (disabled) return
    ref.current.setPointerCapture(e.pointerId)
    start.current = { x: e.clientX, v: value, w: ref.current.clientWidth }
    setLive(value)
  }
  const move = e => {
    if (!start.current) return
    const s = start.current
    const nv = Math.round(Math.min(hi, Math.max(lo, s.v + (e.clientX - s.x) / s.w * span)))
    setLive(nv)
    onMove(nv)
  }
  const up = () => {
    if (!start.current) return
    start.current = null
    if (live !== null) onDone(live)
    setLive(null)
  }

  return (
    <div ref={ref} className={`bigslider ${disabled ? 'dis' : ''} ${live !== null ? 'on' : ''}`}
      onPointerDown={down} onPointerMove={move} onPointerUp={up} onPointerCancel={up}>
      <div className="bs-fill" style={{ width: `${v / 10}%` }} />
      <div className="bs-off" style={{ left: 0, width: `${lo / 10}%` }} />
      <div className="bs-off" style={{ right: 0, width: `${(1000 - hi) / 10}%` }} />
      <span className="bs-label">{label}</span>
      <span className="bs-val">{v}</span>
    </div>
  )
}
