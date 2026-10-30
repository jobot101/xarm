import { useEffect, useState } from 'react'
import { api } from '../api.js'

// where the chip's time goes: per-core load, the busiest tasks, memory, and how fast the board answers.
// Only polls while open, so it doesn't add load of its own the rest of the time.
export default function System() {
  const [open, setOpen] = useState(false)
  const [d, setD] = useState(null)
  const [rtt, setRtt] = useState(0)
  const [err, setErr] = useState('')
  useEffect(() => {
    if (!open) return
    let live = true
    const tick = async () => {
      const t = performance.now()
      try { const x = await api.sys(); if (live) { setD(x); setRtt(Math.round(performance.now() - t)); setErr('') } }
      catch (e) { if (live) setErr(String(e.message || e)) }
      if (live) setTimeout(tick, 2000)
    }
    tick()
    return () => { live = false }
  }, [open])

  const bar = v => <span className="sysbar"><i style={{ width: `${v}%`, background: v > 85 ? 'var(--bad)' : v > 60 ? 'var(--warn)' : 'var(--ok)' }} /></span>
  const tasks = (d?.tasks || []).filter(t => !/^IDLE/.test(t.name)).sort((a, b) => b.cpu - a.cpu).slice(0, 12)

  return (
    <section className="card">
      <h2>System</h2>
      {!open ? <div className="btns"><button onClick={() => setOpen(true)}>Measure</button></div> : <>
        {err && <p className="warn-line">{err}</p>}
        {d && <>
          <p className="syscore">Core 0 {bar(d.core0)} <b>{d.core0}%</b></p>
          <p className="syscore">Core 1 {bar(d.core1)} <b>{d.core1}%</b></p>
          <p className="hint">Board answers in <b>{rtt} ms</b> · status page takes {d.state_ms} ms to build ·
            RAM {d.heap_kb} KB free (lowest {d.heap_min_kb}, biggest block {d.heap_block_kb}) · PSRAM {Math.round(d.psram_kb / 1024 * 10) / 10} MB ·
            up {Math.floor(d.uptime_s / 60)} min</p>
          <table className="systab"><thead><tr><th>task</th><th>CPU</th><th>core</th><th>stack left</th></tr></thead>
            <tbody>{tasks.map(t => <tr key={t.name}><td>{t.name}</td><td>{t.cpu}%</td><td>{t.core < 0 ? 'any' : t.core}</td>
              <td className={t.stack_free < 512 ? 'warn-line' : ''}>{t.stack_free}</td></tr>)}</tbody></table>
        </>}
        {!d && !err && <p className="hint">measuring…</p>}
        <div className="btns"><button onClick={() => { setOpen(false); setD(null) }}>Stop measuring</button></div>
        <p className="hint">Screenshot this for Claude. Numbers settle after the second reading (2 s).</p>
      </>}
    </section>
  )
}
