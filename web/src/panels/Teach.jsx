import { api } from '../api.js'

export default function Teach({ s, run }) {
  const t = (what) => run(api.teach(what))
  const n = s?.calib?.length || 0

  return (
    <section className="card">
      <h2>Teach</h2>
      <div className="btns">
        <button onClick={() => t('home')}>Save current as Home</button>
        <button onClick={() => t('drop')}>Save current as Drop</button>
      </div>

      <h3>Calibrate camera → arm <small>{n} point{n === 1 ? '' : 's'} {n < 3 ? '(need 3+)' : ''}</small></h3>
      <ol className="steps">
        <li>Put a target on the mat, arm out of the camera's view. <button onClick={() => t('calib_mark')}>1 · Mark target</button></li>
        <li>Jog or hand-pose the open gripper around it. <button className="primary" disabled={!s?.marked} onClick={() => t('calib_add')}>2 · Save point</button></li>
        <li>Repeat across the reachable area — corners + center, 5–9 points.</li>
      </ol>
      <div className="btns">
        <button onClick={() => t('calib_undo')} disabled={!n}>Undo last</button>
        <button className="danger" onClick={() => confirm('Clear all calibration points?') && t('calib_clear')} disabled={!n}>Clear all</button>
      </div>
    </section>
  )
}
