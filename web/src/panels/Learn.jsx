import { useEffect, useState, useCallback } from 'react'
import { api } from '../api.js'
import course from '../lessons.json'

// The main page: Muffin's Robot School, to read. No robot controls here (class happens by voice);
// lessons already learned open to re-read their story, the next one shows its teaser, later ones are a title.
// When Muffin asks something in class (true/false, yes/no, "your turn"), a pop-up offers the answers as
// buttons: a tap goes in as if the kid said it (api.say), so it counts the same as speaking. With taps only
// (School setting, the default) the ears are off and these are the only way in: class starts from a button on
// the top card, and free play is a pop-up of tricks.
export default function Learn({ online }) {
  const [sc, setSc] = useState(null)
  const [open, setOpen] = useState(null)

  const [sent, setSent] = useState({ key: '', t: 0 })   // just tapped: hide that pop-up a few seconds while Muffin acts

  const load = useCallback(async () => { try { setSc(await api.school()) } catch {} }, [])
  const fast = sc?.state === 'in class'
  useEffect(() => { load(); const t = setInterval(load, fast ? 700 : 2000); return () => clearInterval(t) }, [load, fast])

  const ask = online ? sc?.ask : null
  const askKey = ask ? JSON.stringify(ask) : ''
  const answer = async o => { setSent({ key: askKey, t: Date.now() }); try { await api.say(o.say) } catch {} ; load() }
  const start = ask && (ask.kind === 'start' || ask.kind === 'resume')     // taps only: class starts from the top card
  const popup = ask && !start && ask.options?.length > 0 && (askKey !== sent.key || Date.now() - sent.t > 4000)

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
            : start ? (ask.kind === 'resume' ? 'Ready to keep going?' : 'Ready when you are.')
            : sc?.tap ? 'Muffin is getting ready.'
            : <>Say <b>“Hey Muffin, let's go!”</b> to start class.</>}
        </p>
        {start && <button className="learn-start" onClick={() => answer(ask.options[0])}>{ask.options[0].label}</button>}
      </section>

      {popup && (
        <div className="ask-back" role="dialog" aria-modal="true">
          <div className={`ask ask-${ask.kind}`}>
            <div className="ask-who">Muffin asks</div>
            <p className="ask-q">{
              ask.kind === 'tf' ? (course.quiz[ask.q] ?? 'True or false?')
              : ask.kind === 'yn' ? 'Another lesson?'
              : ask.kind === 'kid' ? "Who's here today?"
              : ask.kind === 'stop' ? 'Tell Muffin to stop!'
              : ask.kind === 'free' ? 'Free play! Pick one:'
              : ask.kind === 'fetch' ? 'Put something small between Muffin\'s fingers, then:'
              : ask.kind === 'giveback' ? 'Hold it, then:'
              : 'Your turn! Tell Muffin:'}</p>
            <div className="ask-opts">
              {ask.options.map(o => <button key={o.say} className={`ask-btn ${o.say}`} onClick={() => answer(o)}>{o.label}</button>)}
            </div>
            {!sc?.tap && <p className="ask-hint">Tap one, or just say it.</p>}
          </div>
        </div>
      )}

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
