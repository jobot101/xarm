// what the vision module reports: color blobs (center x,y + size) in its own pixel space
const COLORS = ['#ff5a4e', '#4ea4ff']

export default function Vision({ s }) {
  const slots = s?.cam?.slots || []
  const calib = s?.calib || []
  // module color mode is ~160x120; grow the frame if it reports bigger coords
  const W = Math.max(160, ...slots.map(b => b.x + b.w / 2), ...calib.map(c => c.px))
  const H = Math.max(120, ...slots.map(b => b.y + b.h / 2), ...calib.map(c => c.py))

  return (
    <section className="card">
      <h2>Camera <small>{s?.cam?.ok ? `${calib.length} calib pts` : 'not responding'}</small></h2>
      <svg className="cam" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="xMidYMid meet">
        <rect width={W} height={H} className="cam-bg" />
        {calib.map((c, i) => (
          <g key={i} className="calpt">
            <circle cx={c.px} cy={c.py} r="2.5" />
            <text x={c.px + 4} y={c.py - 3}>{i + 1}</text>
          </g>
        ))}
        {slots.map((b, i) => b.seen && (
          <g key={i} stroke={COLORS[i]} fill="none">
            <rect x={b.x - b.w / 2} y={b.y - b.h / 2} width={b.w} height={b.h} strokeWidth="1.5" />
            <text x={b.x - b.w / 2} y={b.y - b.h / 2 - 3} fill={COLORS[i]} stroke="none">{b.name}</text>
          </g>
        ))}
      </svg>
      <div className="legend">
        {slots.map((b, i) => (
          <span key={i} style={{ color: COLORS[i] }}>
            ■ {b.name}: {b.seen ? `${b.x}, ${b.y}` : 'not seen'}
          </span>
        ))}
      </div>
      <details><summary>raw I2C</summary><code>{s?.cam?.raw}</code></details>
    </section>
  )
}
