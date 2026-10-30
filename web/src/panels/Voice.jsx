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
  const setWake = on => run(api.voice({ wake: on ? 1 : 0 }))
  const c = v.cloud
  const [key, setKey] = useState('')
  const saveKey = () => { run(api.voice({ stt_key: key.trim() })); setKey('') }
  const setCloud = on => run(api.voice({ cloud: on ? 1 : 0 }))
  const setDual = on => {
    if (confirm(`${on ? 'Use both mics' : 'Go back to one mic'}? Muffin restarts to switch (about 15 s).`)) run(api.voice({ dual: on ? 1 : 0 }))
  }

  return (
    <section className="card">
      <h2>Voice</h2>
      <p><b>{v.ok ? '● ' : '○ '}</b>{v.status || 'no voice info from the board'}</p>
      {v.vad !== undefined && <p className="hint">
        Voice detector: <b>{v.vad}</b>{v.vad === 'WebRTC' && ' — the newer VADNet detector needs a Full install (keep settings) from the phone flasher'}
        {' · '}{v.dual ? 'both mics' : 'one mic'}
        {v.wake && <>{' · '}<b className={v.awake ? 'ok-line' : ''}>{v.awake ? 'listening now' : 'waiting for “Hey Muffin”'}</b></>}
      </p>}
      {v.wake !== undefined && <div className="toggles">
        <label className="toggle"><input type="checkbox" checked={!!v.wake} onChange={e => setWake(e.target.checked)} />
          <span><b>Say “Hey Muffin” first</b><small>“Stop” always works. After “Hey Muffin” (or a tap in the app) Muffin listens
            ~8 s, keeps listening while you talk to it, and the ring glows blue while it's listening.</small></span></label>
        <label className="toggle"><input type="checkbox" checked={!!v.dual} onChange={e => setDual(e.target.checked)} />
          <span><b>Use both mics</b><small>Steers toward whoever's talking. Try it if Muffin misses you in a noisy room. Restarts Muffin.</small></span></label>
        {v.dual && <label className="toggle"><input type="checkbox" checked={v.face !== 0} onChange={e => run(api.voice({ face: e.target.checked ? 1 : 0 }))} />
          <span><b>Turn toward voices</b><small>On “Hey Muffin” it turns its head toward whoever said it.
            Turns the wrong way? <a href="#" onClick={e => { e.preventDefault(); run(api.voice({ face: v.face === -1 ? 1 : -1 })) }}>Flip it</a>{v.face === -1 ? ' (flipped)' : ''}.</small></span></label>}
      </div>}
      {c && <div className="cloud">
        <h3>Cloud ears <small>Groq Whisper · free · only after “Hey Muffin”</small></h3>
        <p><b className={c.ready ? 'ok-line' : ''}>{c.ready ? '● ' : '○ '}</b>{c.status}
          {c.ready && c.today > 0 && <small> · {c.today} of {c.max} today</small>}</p>
        {c.last && <p className="hint">Last: “{c.last}”{c.ms ? ` — ${(c.ms / 1000).toFixed(1)} s` : ''}</p>}
        {c.key && <label className="toggle"><input type="checkbox" checked={!!c.on} onChange={e => setCloud(e.target.checked)} />
          <span><b>Use cloud ears</b><small>Understands whole sentences (“can you pick up the red one?”). “Stop” and
            “Hey Muffin” stay on the board, and with no internet it falls back to the on-board phrases by itself.</small></span></label>}
        <div className="keyrow">
          <input type="password" autoComplete="off" spellCheck="false" placeholder={c.key ? `set: ${c.key}` : 'Groq key (gsk_…)'}
            value={key} onChange={e => setKey(e.target.value)} />
          <button disabled={!key.trim()} onClick={saveKey}>Save key</button>
        </div>
        {c.key && <button className="link" onClick={() => confirm('Remove the Groq key from Muffin?') && run(api.voice({ stt_key: '' }))}>Remove key</button>}
        <p className="hint">Get a free key at console.groq.com → API Keys. It's stored on Muffin only. Voice clips go to Groq's
          servers, only after “Hey Muffin”.{!v.wake && ' With “Hey Muffin” switched off, every sentence near Muffin is sent — that uses the free daily allowance fast.'}</p>
      </div>}
      {v.bank_status !== undefined && <p className={v.bank ? 'hint' : 'warn-line'}>
        Muffin's voice: <b>{v.bank ? 'ready' : 'not loaded'}</b> — {v.bank_status}
        {!v.bank && ' (needs home WiFi with internet; it downloads by itself)'}
        {v.src === 'sd' && ' · playing full quality from the SD card'}</p>}
      {v.sd && <div className="sdvoice">
        <label className="check">
          <input type="checkbox" checked={!!v.sd.on} disabled={!v.sd.card && !v.sd.on}
            onChange={e => run(api.voice({ sd: e.target.checked ? 1 : 0 }))} />
          Store Muffin's full-quality voice on the SD card
        </label>
        <p className="hint">Card: {v.sd.card_status}{v.sd.on ? ` · ${v.sd.status}` : ''}</p>
        <p className="hint">Muffin only ever writes inside a <code>muffin</code> folder on the card and never formats it.
          About 25 MB. The flash voice keeps playing until the download is checked and ready.</p>
      </div>}
      <div className="meter" title="mic level">
        <div style={{ width: `${lv}%`, height: 14, borderRadius: 7, transition: 'width 120ms',
          background: lv > 85 ? '#c0392b' : lv > 35 ? '#2e8b57' : '#b8860b' }} />
      </div>
      <p className="hint">Mic level {lv}. Quiet room ≈ 10–30, talking at arm's length should jump past 40.</p>
      <p>Last heard: <b>{v.last || '—'}</b>{v.prob ? ` (${v.prob}%)` : ''}</p>
      {v.heard?.length > 0 && <>
        <h3>What Muffin heard <small>newest first · % = how sure the on-board ears were · ☁ = cloud</small></h3>
        <ul className="heard">
          {v.heard.map((h, i) => (
            <li key={i} className={/ignored|no voice|talking|asleep|didn't|no words|cloud didn't|too fast/.test(h.why) ? 'skip' : 'ok'}>
              <b>{h.p}</b> {!/^☁/.test(h.why) && h.prob > 0 && <span className="pc">{h.prob}%</span>} <small>{h.why} · {h.ago}s ago</small>
            </li>))}
        </ul>
        <p className="hint">Said something and it's not here at all? Raise the gain or lower the threshold. Here but
          “no voice heard” or “Muffin was talking”? It caught you but wasn't sure — speak a bit louder or wait for it
          to finish talking. “asleep”? It heard you fine but nobody said “Hey Muffin” first. Here as the wrong phrase?
          Raise the threshold a little.</p>
      </>}
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
      <p className="hint">Good start: gain 24–30 dB, threshold 18–25%. Try “hey muffin”, then “dance robot”, “wave hello”, “go home”, “stop”. Speak plainly, about 1 m away, after Muffin finishes talking.</p>
    </section>
  )
}
