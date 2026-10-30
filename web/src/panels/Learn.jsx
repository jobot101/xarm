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
  const pick = async n => { try { await api.setSchool({ lesson: n - 1 }) } catch {} ; setOpen(null); load() }   // Start lesson
  const canPick = online && sc?.can_pick
  const answer = async o => { setSent({ key: askKey, t: Date.now() }); try { await api.say(o.say) } catch {} ; load() }
  const start = ask && (ask.kind === 'start' || ask.kind === 'resume')     // taps only: class starts from the top card
  const popup = ask && !start && ask.options?.length > 0 && (askKey !== sent.key || Date.now() - sent.t > 4000)

  const kid = sc?.kids?.[0]
  const total = course.parts.reduce((a, p) => a + p.lessons.length, 0)
  const done = Math.min(kid?.lesson ?? 0, total)             // lessons finished; the next one is done + 1
  const rank = course.ranks[Math.min(Math.floor(done / course.rank_every), course.ranks.length - 1)]
  const left = kid ? Math.max(0, kid.limit_s - kid.used_s) : null
  const asleep = sc?.state === 'asleep'
  const lessons = course.parts.flatMap(p => p.lessons)
  const nextL = lessons[done] || null                        // null = finished the course
  const teaser = l => { const t = l.teaser.replace(/^Next time:\s*/i, ''); return t[0].toUpperCase() + t.slice(1) }
  const nowL = sc?.now >= 0 ? lessons[sc.now] : null
  const inClass = sc?.state === 'in class'

  // the one obvious next step, for whatever moment this is
  const step = !online ? { cls: 'off', eyebrow: 'Offline', title: "Can't reach Muffin", note: 'Check that Muffin is switched on and on the WiFi.' }
    : asleep ? { cls: 'sleep', eyebrow: 'Tomorrow', title: nextL ? `Lesson ${nextL.n}: ${nextL.title}` : 'Review day',
                 note: 'Muffin is asleep. School opens again tomorrow.' }
    : inClass ? (ask?.kind === 'free' ? { cls: 'now', eyebrow: 'Now', title: 'Free play', note: 'Pick a trick for Muffin.' }
               : { cls: 'now', eyebrow: 'Now', title: nowL ? `Lesson ${nowL.n}: ${nowL.title}` : 'Warm-up',
                   note: 'Listen to Muffin. Buttons pop up when it\'s your turn.' })
    : start ? { cls: 'go', eyebrow: ask.kind === 'resume' ? 'Paused' : 'Next up',
                title: nextL ? `Lesson ${nextL.n}: ${nextL.title}` : 'Review time',
                note: nextL ? teaser(nextL) : 'You finished every lesson! Muffin will quiz you on the best ones.',
                btn: ask.kind === 'resume' ? '▶ Keep going' : '▶ Start', go: () => answer(ask.options[0]),
                sub: ask.kind !== 'resume' && done > 0 && nextL ? 'A quick warm-up first, then the new lesson.' : null }
    : sc?.tap === false ? { cls: 'go', eyebrow: 'Next up', title: nextL ? `Lesson ${nextL.n}: ${nextL.title}` : 'Review time',
                            note: <>Say <b>“Hey Muffin, let's go!”</b> to start.</> }
    : { cls: 'wait', eyebrow: 'One moment', title: 'Muffin is getting ready', note: 'The Start button shows up in a second.' }

  return (
    <main className="learn">
      <section className={`card learn-next ${step.cls}`} aria-live="polite">
        <div className="learn-eyebrow">{step.cls === 'now' && <i className="pulse" />}{step.eyebrow}</div>
        <h2>{step.title}</h2>
        {step.note && <p>{step.note}</p>}
        {step.btn && <button className="learn-start" onClick={step.go}>{step.btn}</button>}
        {step.sub && <small>{step.sub}</small>}
      </section>

      <section className="card learn-hero">
        <h2>{kid && kid.name !== 'guest' ? `${kid.name[0].toUpperCase() + kid.name.slice(1)}'s Robot School` : "Muffin's Robot School"}</h2>
        <div className="learn-rank">{rank}</div>
        <div className="learn-bar"><i style={{ width: `${(done / total) * 100}%` }} /></div>
        <div className="learn-stats">
          <span><b>{done}</b> of {total} lessons</span>
          <span><b>★ {kid?.stars ?? 0}</b> stars</span>
          {kid?.streak > 1 && <span><b>{kid.streak}</b> days in a row</span>}
          {left != null && !asleep && <span><b>{Math.ceil(left / 60)}</b> min left today</span>}
        </div>
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
                      <span className="mark">✓</span><span>{l.n}. {l.title}</span>
                      {nowL?.n === l.n && <span className="tag">now</span>}<span className="chev">{isOpen ? '−' : '+'}</span>
                    </button>
                    {isOpen && <div className="story">{l.story.map((t, i) => <p key={i}>{t}</p>)}</div>}
                    {isOpen && canPick && <button className="learn-go again" onClick={() => pick(l.n)}>▶ Do it again</button>}
                  </li>)
                if (isNext) return (
                  <li key={l.n} className="next">
                    <div className="learn-row"><span className="mark">→</span><span>{l.n}. {l.title}</span>
                      <span className="tag">{nowL?.n === l.n ? 'now' : 'next'}</span></div>
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
