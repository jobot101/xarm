import { useEffect, useState, useCallback } from 'react'
import { api } from '../api.js'

// Muffin's Robot School (firmware school.c): the daily voice-only class. Grown-up controls live here;
// kids never see this panel, and nothing done from the app counts against their time.
export default function School({ run }) {
  const [sc, setSc] = useState(null)
  const [min, setMin] = useState('')
  const [pass, setPass] = useState('')

  const load = useCallback(async () => { try { setSc(await api.school()) } catch {} }, [])
  useEffect(() => { load(); const t = setInterval(load, 3000); return () => clearInterval(t) }, [load])

  const set = async body => { await run(api.setSchool(body)); load() }
  if (!sc) return <section className="card"><h2>Robot School</h2><p className="hint">loading…</p></section>

  const mm = s => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
  const clock = { internet: 'internet time', rtc: 'board clock', none: 'no clock: each restart is a new day' }[sc.clock] || sc.clock

  return (
    <section className="card">
      <h2>Robot School <small>{sc.state}{sc.kid ? ` · ${sc.kid}` : ''}</small></h2>
      <label className="autoup">
        <span>Daily class by voice: lesson, quiz, free play, then Muffin sleeps until tomorrow</span>
        <input type="checkbox" checked={sc.on} onChange={e => set({ on: e.target.checked })} />
      </label>
      <label className="autoup">
        <span>Taps only: Muffin's ears are off in school, the kid answers with the buttons on the lessons page ("stop" and the passphrase still work by voice)</span>
        <input type="checkbox" checked={!!sc.tap} onChange={e => set({ tap: e.target.checked })} />
      </label>
      <div className="fields">
        <label className="field">Minutes per kid per day
          <input type="number" min="5" max="120" value={min} placeholder={String(sc.minutes)} onChange={e => setMin(e.target.value)} />
        </label>
        <label className="field">Grown-up passphrase {sc.pass_set ? '(set)' : '(none)'}
          <input value={pass} placeholder={sc.pass_set ? 'new phrase, or clear' : 'e.g. purple elephant'} onChange={e => setPass(e.target.value)} />
        </label>
      </div>
      <div className="btns">
        <button className="primary" disabled={!min && !pass}
          onClick={() => { const b = {}; if (min) b.minutes = +min; if (pass) b.pass = pass; set(b); setMin(''); setPass('') }}>Save</button>
        {sc.pass_set && <button onClick={() => set({ pass: '' })}>Clear passphrase</button>}
      </div>
      <p className="hint">Saying the passphrase to Muffin (a few plain words, 6+ letters) adds ten minutes, even after it's gone to sleep.</p>

      <h3>Kids <small>{sc.lesson_count} lessons in the course</small></h3>
      {sc.kids.map(k => (
        <div key={k.i} className="autoup" style={{ margin: '8px 0' }}>
          <span>
            <b>{k.name}</b> · lesson {Math.min(k.lesson + 1, sc.lesson_count)}{k.lesson >= sc.lesson_count ? ' (done)' : ''} · ★ {k.stars}
            {k.streak > 1 ? ` · ${k.streak} days in a row` : ''} · today {mm(k.used_s)} of {mm(k.limit_s)}
          </span>
          <span className="btns">
            <button className="pill" onClick={() => set({ more: k.i })}>+10 min</button>
            <button className="pill" onClick={() => set({ reset_day: k.i })}>Reset today</button>
            <button className="pill danger" onClick={() => confirm(`Start ${k.name} over from lesson one?`) && set({ restart: k.i })}>Start over</button>
          </span>
        </div>
      ))}
      <p className="hint">
        {sc.lessons_ready ? 'Lessons are on the memory card.' : 'Lessons not on the card yet: turn on Voice → Store voice on SD card (needs a memory card and home WiFi). Until then the class is timed free play.'}
        {' '}Day starts at midnight · {clock}.
      </p>
    </section>
  )
}
