import { useState } from 'react'
import { api } from '../api.js'

// home WiFi: the board needs internet to pull new builds from GitHub by itself
export default function Network({ s, run }) {
  const [ssid, setSsid] = useState('')
  const [pass, setPass] = useState('')
  const n = s?.net || {}
  const where = n.mode === 'sta' ? `on “${n.ssid}” · ${n.ip} · http://muffin · backup “Muffin” 192.168.4.1` : n.mode === 'ap' ? 'own hotspot “Muffin” · 192.168.4.1' : '—'

  return (
    <section className="card">
      <h2>WiFi <small>{where}</small></h2>
      <div className="fields">
        <label className="field">Home network<input value={ssid} placeholder={n.ssid || 'network name'} onChange={e => setSsid(e.target.value)} /></label>
        <label className="field">Password<input type="password" value={pass} onChange={e => setPass(e.target.value)} /></label>
      </div>
      <div className="btns">
        <button className="primary" disabled={!ssid}
          onClick={() => confirm(`Join “${ssid}” and reboot? Then reconnect your phone to that WiFi and open http://muffin (iPhone: muffin.local works too). The “Muffin” network stays on as a backup: 192.168.4.1`) && run(api.wifi(ssid, pass))}>
          Save &amp; reboot
        </button>
      </div>
      {n.ssid && !n.online && n.why && <p className="hint"><b>Not on “{n.ssid}”:</b> {n.why}. It keeps trying every 45 s.</p>}
      <p className="hint">If it can't join, its own “Muffin” hotspot (http://192.168.4.1) stays up, and it keeps retrying home WiFi.</p>
    </section>
  )
}
