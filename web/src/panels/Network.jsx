import { useState } from 'react'
import { api } from '../api.js'

// home WiFi: the board needs internet to pull new builds from GitHub by itself
export default function Network({ s, run }) {
  const [ssid, setSsid] = useState('')
  const [pass, setPass] = useState('')
  const n = s?.net || {}
  const where = n.mode === 'sta' ? `on “${n.ssid}” · ${n.ip}` : n.mode === 'ap' ? 'own hotspot “xarm-voice” · 192.168.4.1' : '—'

  return (
    <section className="card">
      <h2>WiFi <small>{where}</small></h2>
      <div className="fields">
        <label className="field">Home network<input value={ssid} placeholder={n.ssid || 'network name'} onChange={e => setSsid(e.target.value)} /></label>
        <label className="field">Password<input type="password" value={pass} onChange={e => setPass(e.target.value)} /></label>
      </div>
      <div className="btns">
        <button className="primary" disabled={!ssid}
          onClick={() => confirm(`Join “${ssid}” and reboot? Then reconnect your phone to that WiFi and open http://xarm.local`) && run(api.wifi(ssid, pass))}>
          Save &amp; reboot
        </button>
      </div>
      <p className="hint">If it can't join, it falls back to its own “xarm-voice” hotspot after 15 s.</p>
    </section>
  )
}
