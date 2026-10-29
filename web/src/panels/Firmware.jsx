import { useEffect, useRef, useState } from 'react'
import { api } from '../api.js'

// where CI publishes every build (same files the board's auto-update reads). GitHub Pages sends
// Access-Control-Allow-Origin: *, so this page (served by the board over http) can fetch them.
const SRC = 'https://jobot101.github.io/xarm/fw/'
const NO_NET = "This phone can't reach GitHub right now. On Muffin's WiFi: keep mobile data on " +
  "(iPhone: tap “Use Without Internet” if it asks; Android: “Keep connection”). Or put Muffin on home WiFi in Network."

// the board reboots ~1 s after it replies; poll until it answers with a different build
async function waitForNew(old) {
  for (let t = 0; t < 90; t++) {
    await new Promise(r => setTimeout(r, 1000))
    try {
      const st = await api.state()
      if (st.fw?.version !== old) return st.fw.version
    } catch { /* still rebooting */ }
  }
  throw new Error("didn't come back on a new version within 90 s")
}

// download with progress (fetch streams); throws NO_NET on network failure
async function download(path, onPct) {
  let r
  try { r = await fetch(SRC + path, { cache: 'no-store' }) } catch { throw new Error(NO_NET) }
  if (!r.ok) throw new Error(`${path}: GitHub said ${r.status}`)
  const total = +r.headers.get('content-length') || 0
  if (!r.body || !total) return r.blob()
  const reader = r.body.getReader(), parts = []
  let got = 0
  for (;;) {
    const { value, done } = await reader.read()
    if (done) break
    parts.push(value); got += value.length
    onPct(Math.round(100 * got / total))
  }
  return new Blob(parts)
}

// XHR, because fetch has no upload progress
function upload(url, body, onPct) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest()
    x.open('POST', url)
    x.upload.onprogress = e => { if (e.lengthComputable) onPct(Math.round(100 * e.loaded / e.total)) }
    x.onerror = () => reject(new Error('upload failed — board unreachable'))
    x.onload = () => {
      let r = {}
      try { r = JSON.parse(x.responseText) } catch { /* not json */ }
      if (x.status !== 200 || !r.ok) reject(new Error(r.msg || `upload failed (${x.status})`))
      else resolve(r)
    }
    x.send(body)
  })
}

// Two ways in, both work from iPhone and Android:
//  - Install latest: this phone downloads the newest build from GitHub and hands it to the board.
//    Works even when the board has no internet (phone on Muffin's own WiFi, using its mobile data).
//  - Manual upload: pick a xarm-voice.bin saved earlier.
export default function Firmware({ s, run }) {
  const [file, setFile] = useState(null)
  const [pct, setPct] = useState(null)      // progress of the current step, null when idle
  const [msg, setMsg] = useState('')
  const [busy, setBusy] = useState(false)
  const ref = useRef(null)
  const cur = s?.fw?.version

  // flasher page links here as http://muffin.local/#update — scroll once the panels above have filled in
  const scrolled = useRef(false)
  useEffect(() => {
    if (!cur || scrolled.current || location.hash !== '#update') return
    scrolled.current = true
    setTimeout(() => ref.current?.scrollIntoView({ block: 'start' }), 300)
  }, [cur])

  const step = (m) => (p) => { setPct(p); setMsg(`${m} ${p}%`) }
  const fail = e => { setPct(null); setBusy(false); setMsg(String(e.message || e)) }

  const finishApp = async () => {
    setPct(null); setMsg('rebooting… listen for “Ready to rumble!”')
    const v = await waitForNew(cur)
    setMsg(`now running ${v}`)
  }

  const installLatest = async () => {
    setBusy(true); setPct(null); setMsg('checking GitHub…')
    try {
      let man
      try { man = await (await fetch(`${SRC}manifest.json?t=${Date.now()}`, { cache: 'no-store' })).json() }
      catch { throw new Error(NO_NET) }
      const needVoice = !!man.sounds && man.sounds !== s?.fw?.sounds
      const needApp = !!man.version && man.version !== cur
      if (!needVoice && !needApp) { setMsg(`already up to date (${cur})`); setBusy(false); return }

      // voice first, like the board's own updater: no reboot, and it belongs to the build on offer
      if (needVoice) {
        const bank = await download(`sounds.bin?v=${man.sounds}`, step('downloading Muffin’s voice…'))
        await upload(`/api/sounds?v=${encodeURIComponent(man.sounds)}`, bank, step('sending voice to Muffin…'))
      }
      if (needApp) {
        const bin = await download(`xarm-voice.bin?v=${man.version}`, step(`downloading ${man.version}…`))
        await upload('/api/ota', bin, step('sending to Muffin…'))
        await finishApp()
      } else {
        setPct(null); setMsg('voice updated')
      }
      setBusy(false)
    } catch (e) { fail(e) }
  }

  const flashFile = async () => {
    setBusy(true)
    try {
      await upload('/api/ota', file, step('uploading…'))
      await finishApp()
      setBusy(false)
    } catch (e) { fail(e) }
  }

  const behind = s?.fw?.latest && s.fw.latest !== cur

  return (
    <section className="card" id="firmware" ref={ref}>
      <h2>Firmware <small>running {cur || '—'}{s?.fw?.partition ? ` · slot ${s.fw.partition}` : ''}</small></h2>
      <div className="btns">
        <button className="primary" disabled={busy || !cur} onClick={installLatest}>Install latest</button>
      </div>
      {pct !== null && <progress value={pct} max="100" />}
      <p className="hint">{msg || 'Downloads the newest build through this phone and installs it — app, web page and Muffin’s voice. Works on Muffin’s own WiFi too.'}</p>

      <h3>Muffin checks by itself</h3>
      <div className="autoup">
        <label><input type="checkbox" checked={!!s?.fw?.auto} onChange={e => run(api.update({ auto: e.target.checked }))} /> Auto-update from GitHub</label>
        <button onClick={() => run(api.update({ check: true }))}>Check now</button>
      </div>
      <p className="hint">{s?.fw?.update || '—'}{behind ? ` · latest ${s.fw.latest}` : ''} (needs home WiFi)</p>

      <h3>Manual upload</h3>
      <input type="file" accept=".bin,application/octet-stream" disabled={busy} onChange={e => setFile(e.target.files[0] || null)} />
      <div className="btns">
        <button disabled={!file || busy} onClick={flashFile}>Upload &amp; update</button>
      </div>
      <p className="hint">Pick xarm-voice.bin saved from the flasher page. A build that fails to start rolls back on reboot.</p>
    </section>
  )
}
