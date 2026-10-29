// The Ragamuffin Hall rose window, as drawn on ragamuffinstudios.com: a cream medallion, charcoal rings
// around a red/blue beaded band, 16 rounded petals cycling red · yellow · green · blue, and a colored hub
// under a white five-pointed star. Flat folk-art lines.
const INK = '#2b2b2b', CREAM = '#fbf8f1'
const PETALS = ['#c4544a', '#d9b347', '#7aa356', '#5b93b8']        // red, yellow, green, blue (from the top, clockwise)

const P = (r, deg) => {
  const a = (deg - 90) * Math.PI / 180
  return `${(50 + r * Math.cos(a)).toFixed(2)} ${(50 + r * Math.sin(a)).toFixed(2)}`
}

// narrow, rounded end at the hub; wide, round end at the rim
const petal = (d) =>
  `M${P(15.5, d - 5.2)} L${P(31.5, d - 9.4)} A5.2 5.2 0 0 1 ${P(31.5, d + 9.4)} L${P(15.5, d + 5.2)} ` +
  `A1.45 1.45 0 0 1 ${P(15.5, d - 5.2)}Z`

const star = (() => {
  const pts = []
  for (let k = 0; k < 10; k++) pts.push(P(k % 2 ? 3.3 : 8.2, k * 36))
  return `M${pts.join(' L')}Z`
})()

// hub: five wedges between the star's arms
const wedge = (k) => `M50 50 L${P(11.2, k * 72 - 36)} A11.2 11.2 0 0 1 ${P(11.2, k * 72 + 36)}Z`
const HUB = ['#c4544a', '#d9b347', '#7aa356', '#5b93b8', '#d9b347']

const BAND_R = 41.6, SEG = (2 * Math.PI * BAND_R) / 48      // 48 beads, alternating red / blue

export default function RoseWindow({ size = 44, spin = false, progress = 0 }) {
  return (
    <svg width={size} height={size} viewBox="0 0 100 100" className={spin ? 'rose spin' : 'rose'} aria-hidden>
      <circle cx="50" cy="50" r="49" fill={CREAM} />
      <circle cx="50" cy="50" r="44.3" fill="none" stroke={INK} strokeWidth="2.2" />
      <circle cx="50" cy="50" r={BAND_R} fill="none" stroke="#c4544a" strokeWidth="3"
        strokeDasharray={`${SEG * 0.62} ${SEG * 1.38}`} />
      <circle cx="50" cy="50" r={BAND_R} fill="none" stroke="#5b93b8" strokeWidth="3"
        strokeDasharray={`${SEG * 0.62} ${SEG * 1.38}`} strokeDashoffset={-SEG} />
      <circle cx="50" cy="50" r="38.8" fill="none" stroke={INK} strokeWidth="1.6" />
      <g stroke={INK} strokeWidth="1.3" strokeLinejoin="round">
        {Array.from({ length: 16 }, (_, i) => <path key={i} d={petal(i * 22.5)} fill={PETALS[i % 4]} />)}
      </g>
      <g>{HUB.map((c, k) => <path key={k} d={wedge(k)} fill={c} />)}</g>
      <circle cx="50" cy="50" r="12" fill="none" stroke={INK} strokeWidth="1.8" />
      <path d={star} fill="#fff" stroke={INK} strokeWidth="1" strokeLinejoin="round" />
      {progress > 0 && <circle cx="50" cy="50" r="47" fill="none" stroke={INK} strokeWidth="3"
        strokeDasharray={`${progress * 295} 400`} transform="rotate(-90 50 50)" />}
    </svg>
  )
}
