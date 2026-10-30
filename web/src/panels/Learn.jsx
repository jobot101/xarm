import { useEffect, useState, useCallback } from 'react'
import { api } from '../api.js'
import course from '../lessons.json'

// The main page: Muffin's Robot School, to read. No robot controls here (class happens by voice);
// lessons already learned open to re-read their story, the next one shows its teaser, later ones are a title.
export default function Learn({ online }) {
  const [sc, setSc] = useState(null)
  const [open, setOpen] = useState(null)

  const load = useCallback(async () => { try { setSc(await api.school()) } catch {} }, [])
  useEffect(() => { load(); const t = setInterval(load, 2000); return () => clearInterval(t) }, [load])

  const kid = sc?.kids?.[0]
  const total = course.parts.reduce((a, p) => a + p.lessons.length, 0)
  const done = Math.min(kid?.lesson ?? 0, total)             // lessons finished; the next one is done + 1
  const rank = course.ranks[Math.min(Math.floor(done / course.rank_every), course.ranks.length - 1)]
  const left = kid ? Math.max(0, kid.limit_s - kid.used_s) : null
  const asleep = sc?.state === 'asleep'

  return (
    <main className="learn">
      <section className="card learn-hero">
        <h2>{kid ? `${kid.name[0].toUpperCase() + kid.name.slice(1)}'s Robot School` : "Muffin's Robot School"}</h2>
        <div className="learn-rank">{rank}</div>
        <div className="learn-bar"><i style={{ width: `${(done / total) * 100}%` }} /></div>
        <div className="learn-stats">
          <span><b>{done}</b> of {total} lessons</span>
          <span><b>★ {kid?.stars ?? 0}</b> stars</span>
          {kid?.streak > 1 && <span><b>{kid.streak}</b> days in a row</span>}
          {left != null && !asleep && <span><b>{Math.ceil(left / 60)}</b> min left today</span>}
        </div>
        <p className="learn-say">
          {!online ? 'Muffin is offline.'
            : asleep ? 'Muffin is asleep. School opens again tomorrow.'
            : sc?.state === 'in class' ? 'Class is on! Listen to Muffin.'
            : <>Say <b>“Hey Muffin, let's go!”</b> to start class.</>}
        </p>
      </section>

      {course.parts.map((part, pi) => {
        const got = part.lessons.filter(l => l.n <= done).length
        return (
          <section key={pi} className="card learn-part">
            <h2>Part {pi + 1}: {part.title} <small>{got} of {part.lessons.length}</small></h2>
            <ol>
              {part.lessons.map(l => {
                const isDone = l.n <= done, isNext = l.n === done + 1, isOpen = open === l.n
                if (isDone) return (
                  <li key={l.n} className="done">
                    <button className="learn-row" onClick={() => setOpen(isOpen ? null : l.n)} aria-expanded={isOpen}>
                      <span className="mark">✓</span><span>{l.n}. {l.title}</span><span className="chev">{isOpen ? '−' : '+'}</span>
                    </button>
                    {isOpen && <div className="story">{l.story.map((t, i) => <p key={i}>{t}</p>)}</div>}
                  </li>)
                if (isNext) return (
                  <li key={l.n} className="next">
                    <div className="learn-row"><span className="mark">→</span><span>{l.n}. {l.title}</span><span className="tag">next</span></div>
                    <p className="teaser">{l.teaser}</p>
                  </li>)
                return (
                  <li key={l.n} className="later">
                    <div className="learn-row"><span className="mark">·</span><span>{l.n}. {l.title}</span></div>
                  </li>)
              })}
            </ol>
          </section>
        )
      })}
    </main>
  )
}
