import { useEffect, useState, useCallback, useRef } from 'react'
import { api } from './api.js'
import RoseWindow from './RoseWindow.jsx'
import Play from './panels/Play.jsx'
import Status from './panels/Status.jsx'
import Commands from './panels/Commands.jsx'
import Joints from './panels/Joints.jsx'
import Teach from './panels/Teach.jsx'
import Settings from './panels/Settings.jsx'
import Firmware from './panels/Firmware.jsx'
import Network from './panels/Network.jsx'
import Helper from './panels/Helper.jsx'
import Voice from './panels/Voice.jsx'
import Vision from './panels/Vision.jsx'
import Limits from './panels/Limits.jsx'
import System from './panels/System.jsx'
import School from './panels/School.jsx'

const POLL_MS = 250
const HOLD_MS = 3000          // hold the rose window this long to open the Workshop (keeps little hands in Play)

export default function App() {
  const [s, setS] = useState(null)        // latest /api/state
  const [online, setOnline] = useState(false)
  const [toast, setToast] = useState('')
  const [mode, setMode] = useState(() => {
    if (location.hash === '#update') return 'workshop'   // flasher page's "Open Muffin" lands on Firmware
    try { return localStorage.getItem('mode') || 'play' } catch { return 'play' }
  })
  const [hold, setHold] = useState(0)
  const holdTimer = useRef(null)

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
    try { const r = await p; if (r?.msg && r.msg !== 'ok') setToast(r.msg); return r }
    catch (e) { setToast(String(e.message || e)) }
  }, [])

  useEffect(() => { if (!toast) return; const t = setTimeout(() => setToast(''), 3500); return () => clearTimeout(t) }, [toast])

  const switchMode = m => { setMode(m); try { localStorage.setItem('mode', m) } catch {} }

  // press-and-hold the rose window: a white ring fills, then Workshop opens
  const holdStart = () => {
    const t0 = Date.now()
    clearInterval(holdTimer.current)
    holdTimer.current = setInterval(() => {
      const p = (Date.now() - t0) / HOLD_MS
      setHold(Math.min(p, 1))
      if (p >= 1) { clearInterval(holdTimer.current); setHold(0); switchMode(mode === 'play' ? 'workshop' : 'play') }
    }, 50)
  }
  const holdEnd = () => { clearInterval(holdTimer.current); setHold(0) }

  return (
    <div className={`app mode-${mode}`}>
      <header className="hall">
        <button className="rose-btn" onPointerDown={holdStart} onPointerUp={holdEnd} onPointerLeave={holdEnd} onPointerCancel={holdEnd}
          onContextMenu={e => e.preventDefault()} aria-label="Ragamuffin Hall">
          <RoseWindow size={52} spin={!!s?.busy} progress={hold} />
        </button>
        <div className="hall-name">
          <b>Muffin</b>
          <small>Ragamuffin Hall · {mode === 'play' ? 'play' : 'workshop'}</small>
        </div>
        {mode === 'workshop' && <button className="pill" onClick={() => switchMode('play')}>Back to play</button>}
      </header>

      {mode === 'play' ? (
        <Play s={s} online={online} run={run} />
      ) : (
        <>
          <Status s={s} online={online} />
          <main className="grid">
            <Commands s={s} run={run} />
            <Voice s={s} run={run} />
            <School run={run} />
            {s?.cam?.ok && <Vision s={s} run={run} />}
            <Joints s={s} run={run} title="Jog" torqueBtn />
            <Helper s={s} run={run} />
            <Limits s={s} run={run} />
            <Teach s={s} run={run} />
            <Settings s={s} run={run} />
            <Firmware s={s} run={run} />
            <Network s={s} run={run} />
            <System />
          </main>
        </>
      )}

      <button className="stop" onClick={() => run(api.cmd('stop'))}>STOP</button>
      {toast && <div className="toast">{toast}</div>}
    </div>
  )
}
