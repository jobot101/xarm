import { useEffect, useState } from 'react'
import { api } from '../api.js'

// voice check: is the mic hearing anything, did the model load, and live gain/sensitivity tuning
export default function Voice({ s, run }) {
  const v = s?.voice || {}
  const [gain, setGain] = useState(null)
  const [thresh, setThresh] = useState(null)
  useEffect(() => { if (gain === null && v.gain !== undefined) { setGain(v.gain); setThresh(v.thresh) } }, [v.gain])

  const lv = v.level ?? 0
  const save = (g, t) => run(api.voice({ gain: g, thresh: t }))

  return (
    <section className="card">
      <h2>Voice</h2>
      <p><b>{v.ok ? '● ' : '○ '}</b>{v.status || 'no voice info from the board'}</p>
      <div className="meter" title="mic level">
        <div style={{ width: `${lv}%`, height: 14, borderRadius: 7, transition: 'width 120ms',
          background: lv > 85 ? '#c0392b' : lv > 35 ? '#2e8b57' : '#b8860b' }} />
      </div>
      <p className="hint">Mic level {lv}. Quiet room ≈ 10–30, talking at arm's length should jump past 40.</p>
      <p>Last heard: <b>{v.last || '—'}</b>{v.prob ? ` (${v.prob}%)` : ''}</p>
      {gain !== null && <>
        <label>Mic gain {gain} dB
          <input type="range" min="0" max="42" step="3" value={gain}
            onChange={e => setGain(+e.target.value)} onPointerUp={() => save(gain, thresh)} onKeyUp={() => save(gain, thresh)} />
        </label>
        <label>Sensitivity — threshold {thresh}% (lower hears more, but more false triggers)
          <input type="range" min="5" max="60" step="1" value={thresh}
            onChange={e => setThresh(+e.target.value)} onPointerUp={() => save(gain, thresh)} onKeyUp={() => save(gain, thresh)} />
        </label>
      </>}
      <p className="hint">Try: “stop”, “go home”, “wave hello”, “pick up the red”. Speak plainly, about 1 m away.</p>
    </section>
  )
}
