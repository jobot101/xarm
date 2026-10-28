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
}
const JEWEL = ['ruby', 'sapphire', 'emerald', 'gold']

export default function Play({ s, online, run }) {
  const st = online ? s?.state || 'idle' : 'offline'
  const [icon, line] = online ? SAY[st] || ['🤖', st] : ['😴', "I'm asleep — is my power on?"]
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
    { k: 'home',  icon: '🏠', label: 'Go home',         act: () => say('go home') },
  ]

  return (
    <div className="play">
      <div className={`bubble st-${st}`}>
        <span className="bubble-icon">{icon}</span>
        <span>{line}</span>
      </div>

      <div className="tiles">
        {TILES.map(t => (
          <button key={t.k} className={`tile ${t.cls || ''}`} disabled={!online || busy} onClick={t.act}>
            <span className="tile-icon">{t.icon}</span>{t.label}
          </button>
        ))}
      </div>

      <MyMoves s={s} online={online} busy={busy} run={run} />
      <Simon online={online} busy={busy} />
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

function Simon({ online, busy }) {
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
      <h2>🔔 Copy the Bells <small>best {best}</small></h2>
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
