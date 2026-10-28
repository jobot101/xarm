import { api } from '../api.js'

// third hand: present the open gripper, grip the part, then nudge it into place (same words work by voice)
export default function Helper({ s, run }) {
  const say = t => run(api.cmd(t))
  const busy = !!s?.busy
  const pad = (label, text) => <button disabled={busy} onClick={() => say(text)}>{label}</button>
  return (
    <section className="card">
      <h2>🤝 Third hand <small>{s?.state === 'helping' ? s.detail : 'say “hold this”'}</small></h2>
      <div className="btns">
        <button className="primary" disabled={busy} onClick={() => say('hold this')}>Hold this</button>
        {pad('Grip', 'hold it')}
        {pad('Let go', 'let go')}
      </div>
      <div className="nudge">
        <span />{pad('▲', 'a little up')}<span />
        {pad('◀', 'a little left')}{pad('⏹', 'stop')}{pad('▶', 'a little right')}
        <span />{pad('▼', 'a little down')}<span />
      </div>
      <div className="btns">
        {pad('⟲ turn', 'turn left')}{pad('turn ⟳', 'turn right')}{pad('tilt up', 'tilt up')}{pad('tilt down', 'tilt down')}
      </div>
      <p className="hint">Voice: “hold this” → “hold it” → “a little left / up …” → “let go”. Teach the helper spot in Teach.</p>
    </section>
  )
}
