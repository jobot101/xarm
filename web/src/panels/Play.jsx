import { useEffect, useRef, useState } from 'react'
import { api } from '../api.js'

// what the arm is doing, said the way a kid would
const SAY = {
  idle:       ['🙂', 'Ready! What should I do?'],
  searching:  ['👀', 'Looking for it…'],
  moving:     ['🦾', 'On my way…'],
  grasping:   ['✊', 'Grabbing!'],
  holding:    ['🤲', 'Got it!'],
  dropping:   ['⬇️', 'Putting it down…'],
  giving:     ['🎁', 'Here you go!'],
  sorting:    ['🧺', 'Sorting everything…'],
  waving:     ['👋', 'Hello there!'],
  bowing:     ['🙇', 'Thank you, thank you!'],
  dancing:    ['💃', 'Dance party!'],
  ringing:    ['🔔', 'Ding dong!'],
  playing:    ['⭐', 'Doing my move!'],
  not_found:  ['🤔', "Hmm… I can't see it."],
  need_calib: ['🧑‍🔧', 'A grown-up needs to teach me first.'],
  stopped:    ['✋', 'Stopped!'],
  updating:   ['✨', 'Learning new tricks…'],
  stacking:   ['🧱', 'Building a tower!'],
  helping:    ['🤝', "I'll hold it for you!"],
}
const JEWEL = ['ruby', 'sapphire', 'emerald', 'gold']

export default function Play({ s, online, run }) {
  const st = online ? s?.state || 'idle' : 'offline'
  const kid = (s?.kids?.list || []).find(k => k.i === s?.kids?.cur)
  let [icon, line] = online ? SAY[st] || ['🤖', st] : ['😴', "I'm asleep — is my power on?"]
  if (online && kid && st === 'idle') { icon = kid.emoji; line = `Hi ${cap(kid.name)}! What should I do?` }
  if (online && st === 'idle' && /tower of/.test(s?.detail || '')) line = `${cap(s.detail)} 🎉`
  const busy = !!s?.busy
  const names = s?.cam?.slots?.map(x => x.name) || ['red', 'blue']
  const say = t => run(api.cmd(t))

  const TILES = [
    { k: 'grab0', icon: '✊', label: `Grab ${names[0]}`, cls: 'ruby',     act: () => say(`pick up the ${names[0]}`) },
    { k: 'grab1', icon: '✊', label: `Grab ${names[1]}`, cls: 'sapphire', act: () => say(`pick up the ${names[1]}`) },
    { k: 'give',  icon: '🎁', label: 'Give it to me',   cls: s?.holding ? 'glow' : '', act: () => say('give it to me') },
    { k: 'drop',  icon: '⬇️', label: 'Put it down',     cls: s?.holding ? 'glow' : '', act: () => say('drop it') },
    { k: 'sort',  icon: '🧺', label: 'Sort them all',   cls: 'emerald',  act: () => say('sort everything') },
    { k: 'wave',  icon: '👋', label: 'Wave',            act: () => say('wave hello') },
    { k: 'bow',   icon: '🙇', label: 'Take a bow',      act: () => say('take a bow') },
    { k: 'dance', icon: '💃', label: 'Dance',           act: () => say('dance') },
    { k: 'bell',  icon: '🔔', label: 'Ring the bell',   cls: 'gold',     act: () => say('ring the bell') },
    { k: 'tower', icon: '🧱', label: 'Build a tower',   cls: 'amethyst', act: () => say('build a tower') },
    { k: 'home',  icon: '🏠', label: 'Go home',         act: () => say('go home') },
  ]

  return (
    <div className="play">
      <div className={`bubble st-${st}`}>
        <span className="bubble-icon">{icon}</span>
        <span>{line}</span>
      </div>

      <WhoIsPlaying s={s} online={online} run={run} />

      <div className="tiles">
        {TILES.map(t => (
          <button key={t.k} className={`tile ${t.cls || ''}`} disabled={!online || busy} onClick={t.act}>
            <span className="tile-icon">{t.icon}</span>{t.label}
          </button>
        ))}
      </div>

      <MyMoves s={s} online={online} busy={busy} run={run} />
      <Simon online={online} busy={busy} kid={kid} />
      <p className="hint center">You can also just talk to me: “pick up the {names[0]}”, “dance”, “stop”.</p>
    </div>
  )
}

// ---------------------------------------------------------------- record your own trick
function MyMoves({ s, online, busy, run }) {
  const list = s?.moves?.list || []
  const rec = !!s?.moves?.recording
  const [step, setStep] = useState(null)       // null | 'ready' | 'recording' | 'name'
  const [name, setName] = useState('')
  const frames = s?.moves?.frames || 0

  const start = async () => { await run(api.moves('record')); setStep('recording') }
  const done = () => setStep('name')
  const save = async () => { await run(api.moves('save', { name: name.trim() })); setStep(null); setName('') }
  const cancel = async () => { if (rec) await run(api.moves('cancel')); setStep(null); setName('') }

  return (
    <section className="card">
      <h2>⭐ My moves</h2>
      <div className="tiles small">
        {list.map(m => (
          <button key={m.i} className="tile star" disabled={!online || busy} onClick={() => run(api.moves('play', { i: m.i }))}>
            <span className="tile-icon">⭐</span>{m.name}
          </button>
        ))}
        {!step && (
          <button className="tile add" disabled={!online || busy || list.length >= 8} onClick={() => setStep('ready')}>
            <span className="tile-icon">＋</span>Make a move
          </button>
        )}
      </div>

      {step && (
        <div className="recorder">
          {step === 'ready' && <>
            <p>When you press <b>Record</b>, the arm goes floppy. Gently move it with your hands — I'll remember!</p>
            <div className="btns"><button className="big-btn ruby" onClick={start}>● Record</button><button onClick={cancel}>Cancel</button></div>
          </>}
          {step === 'recording' && <>
            <p className="rec-dot">● Recording… move me around!</p>
            <progress value={frames} max="150" />
            <div className="btns"><button className="big-btn emerald" onClick={done}>✓ Done</button><button onClick={cancel}>Cancel</button></div>
          </>}
          {step === 'name' && <>
            <p>What's this move called?</p>
            <input className="big-input" value={name} maxLength={15} autoFocus placeholder="the robot"
              onChange={e => setName(e.target.value.replace(/[^a-zA-Z ]/g, ''))} />
            <div className="btns"><button className="big-btn gold" disabled={!name.trim()} onClick={save}>Save my move</button><button onClick={cancel}>Cancel</button></div>
            <p className="hint">Then say “do the {name.trim() || '…'}”!</p>
          </>}
        </div>
      )}
    </section>
  )
}

// ---------------------------------------------------------------- Copy the Bells (Simon Says)
const wait = ms => new Promise(r => setTimeout(r, ms))

function Simon({ online, busy, kid }) {
  const [seq, setSeq] = useState([])
  const [pos, setPos] = useState(0)
  const [phase, setPhase] = useState('off')    // off | show | you | lost
  const [lit, setLit] = useState(-1)
  const [best, setBest] = useState(() => { try { return +localStorage.getItem('simonBest') || 0 } catch { return 0 } })
  const alive = useRef(true)
  useEffect(() => () => { alive.current = false }, [])

  const flash = async (j, ms = 450) => {
    setLit(j); api.fx({ show: j }).catch(() => {})
    await wait(ms); setLit(-1)
  }

  const showSeq = async q => {
    setPhase('show'); await wait(600)
    for (const j of q) { if (!alive.current) return; await flash(j); await wait(200) }
    setPos(0); setPhase('you')
  }

  const start = () => { const q = [rnd()]; setSeq(q); showSeq(q) }

  const tap = async j => {
    if (phase !== 'you') return
    flash(j, 300)
    if (j !== seq[pos]) {
      setPhase('lost'); api.fx({ event: 'sad' }).catch(() => {})
      const score = seq.length - 1
      if (kid) api.kids('score', { game: 'bells', score }).catch(() => {})
      if (score > best) { setBest(score); try { localStorage.setItem('simonBest', score) } catch {} }
      return
    }
    if (pos + 1 === seq.length) {
      api.fx({ event: 'happy' }).catch(() => {})
      const q = [...seq, rnd()]; setSeq(q); await wait(900); showSeq(q)
    } else setPos(pos + 1)
  }

  const msg = { off: 'Watch the lights, then copy me!', show: 'Watch…', you: 'Your turn!',
                lost: `Oops! You got ${seq.length - 1} in a row.` }[phase]

  return (
    <section className="card">
      <h2>🔔 Copy the Bells <small>{kid ? `${cap(kid.name)}'s best ${kid.best_bells}` : `best ${best}`}</small></h2>
      <p className="simon-msg">{msg}{phase === 'you' || phase === 'show' ? ` · round ${seq.length}` : ''}</p>
      <div className="simon">
        {JEWEL.map((c, j) => (
          <button key={c} className={`pad ${c} ${lit === j ? 'lit' : ''}`} disabled={phase !== 'you'} onClick={() => tap(j)} aria-label={c} />
        ))}
      </div>
      {(phase === 'off' || phase === 'lost') && (
        <button className="big-btn gold full" disabled={!online || busy} onClick={start}>{phase === 'lost' ? 'Play again' : 'Start'}</button>
      )}
    </section>
  )
}

function rnd() { return Math.floor(Math.random() * 4) }
function cap(t) { return t ? t[0].toUpperCase() + t.slice(1) : t }

// ---------------------------------------------------------------- who's playing (name + memory)
const EMOJI = ['🦊', '🐸', '🦖', '🐱', '🐶', '🦄', '🐙', '🚀', '🌟', '🎸', '⚽', '🌈']

function WhoIsPlaying({ s, online, run }) {
  const kids = s?.kids?.list || []
  const cur = s?.kids?.cur
  const me = kids.find(k => k.i === cur)
  const [adding, setAdding] = useState(false)
  const [name, setName] = useState('')
  const [emoji, setEmoji] = useState(EMOJI[0])
  const [jewel, setJewel] = useState(0)

  const add = async () => {
    await run(api.kids('add', { name: name.trim(), emoji, jewel }))
    setAdding(false); setName('')
  }

  return (
    <section className="card who">
      <h2>Who's playing?</h2>
      <div className="kids">
        {kids.map(k => (
          <button key={k.i} className={`kid j${k.jewel} ${k.i === cur ? 'on' : ''}`} disabled={!online}
            onClick={() => run(api.kids('select', { i: k.i }))}>
            <span className="kid-emoji">{k.emoji}</span>{cap(k.name)}
          </button>
        ))}
        {!adding && kids.length < 12 && (
          <button className="kid add" disabled={!online} onClick={() => setAdding(true)}><span className="kid-emoji">＋</span>Add me</button>
        )}
      </div>
      {me && !adding && (
        <p className="kid-stats">{me.emoji} {cap(me.name)} · 🔔 best {me.best_bells} · 🧱 best {me.best_tower} · visit #{me.visits}</p>
      )}
      {adding && (
        <div className="recorder">
          <p>What's your name?</p>
          <input className="big-input" value={name} maxLength={15} autoFocus placeholder="your name"
            onChange={e => setName(e.target.value.replace(/[^a-zA-Z ]/g, ''))} />
          <p>Pick your picture:</p>
          <div className="emoji-pick">
            {EMOJI.map(e => <button key={e} className={e === emoji ? 'on' : ''} onClick={() => setEmoji(e)}>{e}</button>)}
          </div>
          <p>Pick your color:</p>
          <div className="jewel-pick">
            {JEWEL.map((c, j) => <button key={c} className={`pad ${c} ${j === jewel ? 'lit' : ''}`} onClick={() => setJewel(j)} aria-label={c} />)}
          </div>
          <div className="btns">
            <button className="big-btn gold" disabled={!name.trim()} onClick={add}>That's me!</button>
            <button onClick={() => setAdding(false)}>Cancel</button>
          </div>
          <p className="hint">Next time just say “hi, I'm {name.trim().toLowerCase() || '…'}”.</p>
        </div>
      )}
    </section>
  )
}
