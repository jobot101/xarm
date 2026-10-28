import { useState } from 'react'
import { api } from '../api.js'

// same vocabulary as voice (firmware/main/router.c)
export default function Commands({ s, run }) {
  const [text, setText] = useState('')
  const names = s?.cam?.slots?.map(x => x.name) || []
  const say = (t) => run(api.cmd(t))

  return (
    <section className="card">
      <h2>Command</h2>
      <div className="btns">
        {names.map(n => <button key={n} className="primary" onClick={() => say(`pick up the ${n}`)}>Pick {n}</button>)}
        <button onClick={() => say('drop it')}>Drop</button>
        <button onClick={() => say('go home')}>Home</button>
        <button onClick={() => say('open hand')}>Open</button>
        <button onClick={() => say('close hand')}>Close</button>
      </div>
      <form className="say" onSubmit={e => { e.preventDefault(); if (text.trim()) { say(text); setText('') } }}>
        <input value={text} onChange={e => setText(e.target.value)} placeholder="type a command, e.g. grab the red" />
        <button type="submit">Send</button>
      </form>
      <p className="hint">
        Voice: say <i>“pick up the {names[0] || 'red'}”</i>, <i>“drop it”</i>, <i>“go home”</i>, <i>“stop”</i>.
        {s?.voice?.last && <> Last heard: <b>“{s.voice.last}”</b></>}
      </p>
    </section>
  )
}
