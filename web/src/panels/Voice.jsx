import { useEffect, useState } from 'react'
import { api } from '../api.js'

// voice check: is the mic hearing anything, did the model load, and live gain/sensitivity tuning
export default function Voice({ s, run }) {
  const v = s?.voice || {}
  const [gain, setGain] = useState(null)
  const [thresh, setThresh] = useState(null)
  useEffect(() => { if (gain === null && v.gain !== undefined) { setGain(v.gain); setThresh(v.thresh) } }, [v.gain])

  const [list, setList] = useState(null)
  const showList = async () => { if (list) return setList(null); try { setList(await api.phrases()) } catch { setList([]) } }
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
        <label className="vslide">Mic gain <b>{gain} dB</b>
          <input type="range" min="0" max="42" step="3" value={gain}
            onChange={e => setGain(+e.target.value)} onPointerUp={() => save(gain, thresh)} onKeyUp={() => save(gain, thresh)} />
        </label>
        <label className="vslide">Threshold <b>{thresh}%</b> <small>lower = hears more, more false triggers</small>
          <input type="range" min="5" max="60" step="1" value={thresh}
            onChange={e => setThresh(+e.target.value)} onPointerUp={() => save(gain, thresh)} onKeyUp={() => save(gain, thresh)} />
        </label>
      </>}
      <div className="btns"><button onClick={showList}>{list ? 'Hide' : 'Things you can say'}</button></div>
      {list && <ul className="phrases">{list.map(p => <li key={p}>{p}</li>)}</ul>}
      <p className="hint">Good start: gain 30–33 dB, threshold 15–20%. Try “dance robot”, “wave hello”, “go home”, “stop”. Speak plainly, about 1 m away.</p>
    </section>
  )
}
