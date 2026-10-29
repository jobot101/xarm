import { useEffect, useRef, useState } from 'react'
import { api } from '../api.js'
import Joints from './Joints.jsx'

// plain status words for each firmware state
const STATUS = {
  idle: 'Ready', searching: 'Looking', moving: 'Moving', grasping: 'Grabbing', holding: 'Holding',
  dropping: 'Dropping', giving: 'Handing over', sorting: 'Sorting', waving: 'Waving', bowing: 'Bowing',
  dancing: 'Dancing', ringing: 'Ringing the bell', playing: 'Playing move', not_found: "Can't see it",
  need_calib: 'Needs calibration', stopped: 'Stopped', updating: 'Updating', stacking: 'Stacking', helping: 'Helping',
}
const SPEEDS = [['Fine', 4], ['Normal', 10], ['Fast', 22]]
const TICK_MS = 100
const JEWEL = ['ruby', 'sapphire', 'emerald', 'gold']

export default function Play({ s, online, run }) {
  const st = online ? s?.state || 'idle' : 'offline'
  const busy = !!s?.busy
  const names = s?.cam?.slots?.map(x => x.name) || ['red', 'blue']
  const cam = !!s?.cam?.ok          // camera parts only show when one is plugged in
  const say = t => run(api.cmd(t))
  const kid = (s?.kids?.list || []).find(k => k.i === s?.kids?.cur)

  return (
    <div className="ctl">
      <div className={`ctl-status st-${st}`}>
        <span className="dot" />
        <b>{online ? STATUS[st] || st : 'Offline'}</b>
        <span className="ctl-detail">{online ? s?.detail : 'no connection to the arm'}</span>
        {kid && <span className="ctl-kid">{kid.emoji} {cap(kid.name)}</span>}
      </div>

      <Drive online={online} busy={busy} run={run} />

      <Joints s={s} run={run} disabled={!online || busy} />

      <section className="card">
        <h2>Actions</h2>
        <div className="acts">
          {cam && <button disabled={!online || busy} onClick={() => say(`pick up the ${names[0]}`)}><i className="sw ruby" />Pick {names[0]}</button>}
          {cam && <button disabled={!online || busy} onClick={() => say(`pick up the ${names[1]}`)}><i className="sw sapphire" />Pick {names[1]}</button>}
          <button disabled={!online || busy} className={s?.holding ? 'hl' : ''} onClick={() => say('give it to me')}>Give to me</button>
          <button disabled={!online || busy} className={s?.holding ? 'hl' : ''} onClick={() => say('drop it')}>Drop</button>
          {cam && <button disabled={!online || busy} onClick={() => say('sort everything')}>Sort by color</button>}
          {cam && <button disabled={!online || busy} onClick={() => say('sort by size')}>Sort by size</button>}
          {cam && <button disabled={!online || busy} onClick={() => say('build a tower')}>Stack</button>}
          <button disabled={!online || busy} onClick={() => say('hold this')}>Hold this</button>
          <button disabled={!online || busy} onClick={() => say('go home')}>Home</button>
        </div>
      </section>

      <section className="card">
        <h2>Moves</h2>
        <div className="acts">
          <button disabled={!online || busy} onClick={() => say('wave hello')}>Wave</button>
          <button disabled={!online || busy} onClick={() => say('take a bow')}>Bow</button>
          <button disabled={!online || busy} onClick={() => say('dance')}>Dance</button>
          <button disabled={!online || busy} onClick={() => say('ring the bell')}>Ring bell</button>
          {(s?.moves?.list || []).map(m => (
            <button key={m.i} className="mine" disabled={!online || busy} onClick={() => run(api.moves('play', { i: m.i }))}>{cap(m.name)}</button>
          ))}
        </div>
        <Recorder s={s} online={online} busy={busy} run={run} />
      </section>

      <details className="card more">
        <summary>Players &amp; game</summary>
        <WhoIsPlaying s={s} online={online} run={run} />
        <Simon online={online} busy={busy} kid={kid} />
      </details>
    </div>
  )
}

// ---------------------------------------------------------------- hold-to-move drive pad
function Drive({ online, busy, run }) {
  const [speed, setSpeed] = useState(1)
  const [grip, setGrip] = useState(false)         // gripper buttons hold-to-move too
  const held = useRef(null)
  const timer = useRef(null)
  const dis = !online || busy

  const send = () => {
    if (!held.current) return
    api.drive({ ...held.current, step: SPEEDS[speed][1], ms: TICK_MS + 20 }).catch(() => {})
  }
  const press = (dir) => (e) => {
    e.preventDefault()
    if (dis) return
    e.currentTarget.setPointerCapture?.(e.pointerId)
    held.current = dir
    send()
    clearInterval(timer.current)
    timer.current = setInterval(send, TICK_MS)
    navigator.vibrate?.(8)
  }
  const release = () => { held.current = null; clearInterval(timer.current) }
  useEffect(() => () => clearInterval(timer.current), [])

  // keyboard: arrows = base / up-down, W/S = reach, Q/E = twist, R/F = tilt, O/C = gripper
  useEffect(() => {
    const keys = { ArrowLeft: { base: 1 }, ArrowRight: { base: -1 }, ArrowUp: { lift: 1 }, ArrowDown: { lift: -1 },
      w: { reach: 1 }, s: { reach: -1 }, q: { twist: 1 }, e: { twist: -1 }, r: { tilt: 1 }, f: { tilt: -1 }, o: { grip: -1 }, c: { grip: 1 } }
    const down = ev => { const d = keys[ev.key]; if (!d || ev.repeat || dis || ev.target.tagName === 'INPUT') return; ev.preventDefault(); held.current = d; send(); clearInterval(timer.current); timer.current = setInterval(send, TICK_MS) }
    const up = ev => { if (keys[ev.key]) release() }
    window.addEventListener('keydown', down); window.addEventListener('keyup', up)
    return () => { window.removeEventListener('keydown', down); window.removeEventListener('keyup', up) }
  })

  const B = ({ dir, children, cls = '' }) => (
    <button className={`pad-btn ${cls}`} disabled={dis}
      onPointerDown={press(dir)} onPointerUp={release} onPointerCancel={release} onLostPointerCapture={release}
      onContextMenu={e => e.preventDefault()}>{children}</button>
  )

  return (
    <section className="card drive">
      <div className="drive-head">
        <h2>Drive</h2>
        <div className="seg">
          {SPEEDS.map(([l], i) => <button key={l} className={i === speed ? 'on' : ''} onClick={() => setSpeed(i)}>{l}</button>)}
        </div>
      </div>

      <div className="drive-body">
        <div className="dpad">
          <span className="lbl top">up</span>
          <B dir={{ lift: 1 }} cls="n">▲</B>
          <B dir={{ base: 1 }} cls="w">◀</B>
          <div className="hub">arm</div>
          <B dir={{ base: -1 }} cls="e">▶</B>
          <B dir={{ lift: -1 }} cls="s">▼</B>
          <span className="lbl bot">down</span>
        </div>

        <div className="side">
          <div className="pair">
            <span className="lbl">reach</span>
            <B dir={{ reach: 1 }}>Out</B>
            <B dir={{ reach: -1 }}>In</B>
          </div>
          <div className="pair">
            <span className="lbl">wrist</span>
            <B dir={{ tilt: 1 }}>Tilt ▲</B>
            <B dir={{ tilt: -1 }}>Tilt ▼</B>
          </div>
          <div className="pair">
            <span className="lbl">twist</span>
            <B dir={{ twist: 1 }}>⟲</B>
            <B dir={{ twist: -1 }}>⟳</B>
          </div>
          <div className="pair">
            <span className="lbl">hand</span>
            <B dir={{ grip: -1 }}>Open</B>
            <B dir={{ grip: 1 }}>Close</B>
          </div>
        </div>
      </div>
      <p className="hint">Hold a button to move; let go to stop. Keyboard: arrows, W/S reach, Q/E twist, R/F tilt, O/C hand.</p>
    </section>
  )
}

// ---------------------------------------------------------------- record a move by hand
function Recorder({ s, online, busy, run }) {
  const rec = !!s?.moves?.recording
  const [step, setStep] = useState(null)       // null | 'recording' | 'name'
  const [name, setName] = useState('')
  const frames = s?.moves?.frames || 0
  const full = (s?.moves?.list || []).length >= 8

  const start = async () => { await run(api.moves('record')); setStep('recording') }
  const save = async () => { await run(api.moves('save', { name: name.trim() })); setStep(null); setName('') }
  const cancel = async () => { if (rec) await run(api.moves('cancel')); setStep(null); setName('') }

  if (!step) return (
    <button className="rec-btn" disabled={!online || busy || full} onClick={start}>● Record a move</button>
  )
  return (
    <div className="recorder">
      {step === 'recording' && <>
        <p><span className="rec-dot">●</span> Recording — the arm is limp; move it by hand. {Math.round(frames / 5)}s / 30s</p>
        <div className="btns"><button className="primary" onClick={() => setStep('name')}>Done</button><button onClick={cancel}>Cancel</button></div>
      </>}
      {step === 'name' && <>
        <input value={name} maxLength={15} autoFocus placeholder="name it" onChange={e => setName(e.target.value.replace(/[^a-zA-Z ]/g, ''))} />
        <div className="btns"><button className="primary" disabled={!name.trim()} onClick={save}>Save</button><button onClick={cancel}>Cancel</button></div>
        <p className="hint">Voice: “do the {name.trim().toLowerCase() || '…'}”.</p>
      </>}
    </div>
  )
}

// ---------------------------------------------------------------- Copy the Bells
const wait = ms => new Promise(r => setTimeout(r, ms))

function Simon({ online, busy, kid }) {
  const [seq, setSeq] = useState([])
  const [pos, setPos] = useState(0)
  const [phase, setPhase] = useState('off')    // off | show | you | lost
  const [lit, setLit] = useState(-1)
  const alive = useRef(true)
  useEffect(() => () => { alive.current = false }, [])

  const flash = async (j, ms = 450) => { setLit(j); api.fx({ show: j }).catch(() => {}); await wait(ms); setLit(-1) }
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
      if (kid) api.kids('score', { game: 'bells', score: seq.length - 1 }).catch(() => {})
      return
    }
    if (pos + 1 === seq.length) { api.fx({ event: 'happy' }).catch(() => {}); const q = [...seq, rnd()]; setSeq(q); await wait(900); showSeq(q) }
    else setPos(pos + 1)
  }
  const msg = { off: 'Watch the ring, then repeat the pattern.', show: 'Watch…', you: 'Your turn', lost: `Score: ${seq.length - 1}` }[phase]

  return (
    <div className="sub">
      <h3>Copy the Bells <small>{kid ? `${cap(kid.name)} best ${kid.best_bells}` : ''}</small></h3>
      <p className="hint">{msg}{phase === 'you' || phase === 'show' ? ` · round ${seq.length}` : ''}</p>
      <div className="simon">
        {JEWEL.map((c, j) => <button key={c} className={`pad ${c} ${lit === j ? 'lit' : ''}`} disabled={phase !== 'you'} onClick={() => tap(j)} aria-label={c} />)}
      </div>
      {(phase === 'off' || phase === 'lost') && <button className="primary full" disabled={!online || busy} onClick={start}>{phase === 'lost' ? 'Play again' : 'Start'}</button>}
    </div>
  )
}

// ---------------------------------------------------------------- players
const EMOJI = ['🦊', '🐸', '🦖', '🐱', '🐶', '🦄', '🐙', '🚀', '🌟', '🎸', '⚽', '🌈']

function WhoIsPlaying({ s, online, run }) {
  const kids = s?.kids?.list || []
  const cur = s?.kids?.cur
  const [adding, setAdding] = useState(false)
  const [name, setName] = useState('')
  const [emoji, setEmoji] = useState(EMOJI[0])
  const [jewel, setJewel] = useState(0)
  const add = async () => { await run(api.kids('add', { name: name.trim(), emoji, jewel })); setAdding(false); setName('') }

  return (
    <div className="sub">
      <h3>Players</h3>
      <div className="kids">
        {kids.map(k => (
          <button key={k.i} className={`kid j${k.jewel} ${k.i === cur ? 'on' : ''}`} disabled={!online} onClick={() => run(api.kids('select', { i: k.i }))}>
            {k.emoji} {cap(k.name)}
          </button>
        ))}
        {!adding && kids.length < 12 && <button className="kid add" disabled={!online} onClick={() => setAdding(true)}>+ Add</button>}
      </div>
      {cur >= 0 && kids.find(k => k.i === cur) && !adding && (() => { const me = kids.find(k => k.i === cur); return (
        <p className="hint">Bells best {me.best_bells} · Tower best {me.best_tower} · {me.visits} visits</p>) })()}
      {adding && (
        <div className="recorder">
          <input value={name} maxLength={15} autoFocus placeholder="name" onChange={e => setName(e.target.value.replace(/[^a-zA-Z ]/g, ''))} />
          <div className="emoji-pick">{EMOJI.map(e => <button key={e} className={e === emoji ? 'on' : ''} onClick={() => setEmoji(e)}>{e}</button>)}</div>
          <div className="jewel-pick">{JEWEL.map((c, j) => <button key={c} className={`pad ${c} ${j === jewel ? 'lit' : ''}`} onClick={() => setJewel(j)} aria-label={c} />)}</div>
          <div className="btns"><button className="primary" disabled={!name.trim()} onClick={add}>Add</button><button onClick={() => setAdding(false)}>Cancel</button></div>
          <p className="hint">Voice: “hi, I'm {name.trim().toLowerCase() || '…'}”.</p>
        </div>
      )}
    </div>
  )
}

function rnd() { return Math.floor(Math.random() * 4) }
function cap(t) { return t ? t[0].toUpperCase() + t.slice(1) : t }
