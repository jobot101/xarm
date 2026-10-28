import { useEffect, useState, useCallback } from 'react'
import { api } from './api.js'
import Status from './panels/Status.jsx'
import Commands from './panels/Commands.jsx'
import Vision from './panels/Vision.jsx'
import Jog from './panels/Jog.jsx'
import Teach from './panels/Teach.jsx'
import Settings from './panels/Settings.jsx'
import Firmware from './panels/Firmware.jsx'

const POLL_MS = 250

export default function App() {
  const [s, setS] = useState(null)        // latest /api/state
  const [online, setOnline] = useState(false)
  const [toast, setToast] = useState('')

  useEffect(() => {
    let live = true
    const tick = async () => {
      try { const st = await api.state(); if (live) { setS(st); setOnline(true) } }
      catch { if (live) setOnline(false) }
      if (live) setTimeout(tick, POLL_MS)
    }
    tick()
    return () => { live = false }
  }, [])

  // every action goes through here so replies show up in one place
  const run = useCallback(async (p) => {
    try { const r = await p; if (r?.msg) setToast(r.msg) }
    catch (e) { setToast(String(e.message || e)) }
  }, [])

  useEffect(() => { if (!toast) return; const t = setTimeout(() => setToast(''), 3500); return () => clearTimeout(t) }, [toast])

  return (
    <div className="app">
      <Status s={s} online={online} />
      <main className="grid">
        <Commands s={s} run={run} />
        <Vision s={s} />
        <Jog s={s} run={run} />
        <Teach s={s} run={run} />
        <Settings run={run} />
        <Firmware s={s} />
      </main>
      <button className="stop" onClick={() => run(api.cmd('stop'))}>STOP</button>
      {toast && <div className="toast">{toast}</div>}
    </div>
  )
}
