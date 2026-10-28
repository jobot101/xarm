import { useState } from 'react'
import { api } from '../api.js'

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

// over-the-air update: POST the raw .bin to /api/ota (XHR for upload progress), then wait for the reboot
export default function Firmware({ s, run }) {
  const [file, setFile] = useState(null)
  const [pct, setPct] = useState(null)      // upload progress, null when not uploading
  const [msg, setMsg] = useState('')
  const [busy, setBusy] = useState(false)
  const cur = s?.fw?.version

  const fail = m => { setPct(null); setBusy(false); setMsg(m) }

  const flash = () => {
    setBusy(true); setPct(0); setMsg('uploading…')
    const x = new XMLHttpRequest()
    x.open('POST', '/api/ota')
    x.upload.onprogress = e => { if (e.lengthComputable) setPct(Math.round(100 * e.loaded / e.total)) }
    x.onerror = () => fail('upload failed — board unreachable')
    x.onload = async () => {
      let r = {}
      try { r = JSON.parse(x.responseText) } catch { /* not json */ }
      if (x.status !== 200 || !r.ok) return fail(r.msg || `upload failed (${x.status})`)
      setPct(null); setMsg('rebooting…')
      try {
        const v = await waitForNew(cur)
        setMsg(`now running ${v}`)
        setBusy(false)
      } catch (e) { fail(e.message) }
    }
    x.send(file)
  }

  return (
    <section className="card">
      <h2>Firmware <small>running {cur || '—'}{s?.fw?.partition ? ` · slot ${s.fw.partition}` : ''}</small></h2>
      <div className="autoup">
        <label><input type="checkbox" checked={!!s?.fw?.auto} onChange={e => run(api.update({ auto: e.target.checked }))} /> Auto-update from GitHub</label>
        <button onClick={() => run(api.update({ check: true }))}>Check now</button>
      </div>
      <p className="hint">{s?.fw?.update || '—'}{s?.fw?.latest && s.fw.latest !== cur ? ` · latest ${s.fw.latest}` : ''}</p>
      <h3>Manual upload</h3>
      <input type="file" disabled={busy} onChange={e => setFile(e.target.files[0] || null)} />
      {pct !== null && <progress value={pct} max="100" />}
      <div className="btns">
        <button className="primary" disabled={!file || busy} onClick={flash}>Upload &amp; update</button>
      </div>
      <p className="hint">{msg || 'Pick xarm-voice.bin from the latest release. A build that fails to start rolls back on reboot.'}</p>
    </section>
  )
}
