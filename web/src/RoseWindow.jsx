// the Ragamuffin Hall rose window: 8 stained-glass petals in a brass ring
const JEWELS = ['#b3203a', '#e0b84c', '#2952a3', '#1f8a5b', '#b3203a', '#6b2c91', '#2952a3', '#e0b84c']

export default function RoseWindow({ size = 44, spin = false, progress = 0 }) {
  const petals = JEWELS.map((c, i) => {
    const a = (i * Math.PI) / 4
    return <ellipse key={i} cx={50 + 24 * Math.cos(a)} cy={50 + 24 * Math.sin(a)} rx="14" ry="9"
      transform={`rotate(${(i * 45)} ${50 + 24 * Math.cos(a)} ${50 + 24 * Math.sin(a)})`} fill={c} opacity=".92" />
  })
  return (
    <svg width={size} height={size} viewBox="0 0 100 100" className={spin ? 'rose spin' : 'rose'} aria-hidden>
      <circle cx="50" cy="50" r="47" fill="#1a120c" stroke="#c9a24a" strokeWidth="4" />
      <g>{petals}</g>
      <circle cx="50" cy="50" r="11" fill="#e0b84c" stroke="#c9a24a" strokeWidth="3" />
      <circle cx="50" cy="50" r="47" fill="none" stroke="#c9a24a" strokeWidth="2" strokeDasharray="4 6" opacity=".5" />
      {progress > 0 && <circle cx="50" cy="50" r="47" fill="none" stroke="#fff" strokeWidth="6"
        strokeDasharray={`${progress * 295} 400`} transform="rotate(-90 50 50)" />}
    </svg>
  )
}
