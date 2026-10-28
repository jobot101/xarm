import { api } from '../api.js'

export default function Teach({ s, run }) {
  const t = (what) => run(api.teach(what))
  const n = s?.calib?.length || 0
  const names = s?.cam?.slots?.map(x => x.name) || ['red', 'blue']
  const moves = s?.moves?.list || []

  return (
    <section className="card">
      <h2>Teach</h2>
      <p className="hint">Pose the arm (jog, or go limp and move it by hand), then save it as:</p>
      <div className="btns">
        <button onClick={() => t('home')}>🏠 Home</button>
        <button onClick={() => t('drop')}>⬇️ Drop</button>
        <button onClick={() => t('give')}>🎁 Hand-off</button>
        <button onClick={() => t('bell')}>🔔 Bell strike</button>
        <button onClick={() => t('bin0')}>🧺 {names[0]} bin</button>
        <button onClick={() => t('bin1')}>🧺 {names[1]} bin</button>
        <button onClick={() => t('look')}>👁️ Look (head cam)</button>
      </div>

      <h3>Calibrate camera → arm <small>{n} point{n === 1 ? '' : 's'} {n < 3 ? '(need 3+)' : ''}</small></h3>
      <ol className="steps">
        <li>Put a target on the mat. (Head cam: the arm goes to its Look pose. Overhead cam: keep the arm out of view.) <button onClick={() => t('calib_mark')}>1 · Mark target</button></li>
        <li>Jog or hand-pose the open gripper around it. <button className="primary" disabled={!s?.marked} onClick={() => t('calib_add')}>2 · Save point</button></li>
        <li>Repeat across the reachable area — corners + center, 5–9 points.</li>
      </ol>
      <div className="btns">
        <button onClick={() => t('calib_undo')} disabled={!n}>Undo last</button>
        <button className="danger" onClick={() => confirm('Clear all calibration points?') && t('calib_clear')} disabled={!n}>Clear all</button>
      </div>

      <h3>Recorded moves <small>{moves.length}/8 · kids make these in Play</small></h3>
      <div className="btns">
        {moves.length === 0 && <span className="hint">none yet</span>}
        {moves.map(m => (
          <button key={m.i} className="danger" onClick={() => confirm(`Delete “${m.name}”?`) && run(api.moves('delete', { i: m.i }))}>✕ {m.name}</button>
        ))}
      </div>
    </section>
  )
}
