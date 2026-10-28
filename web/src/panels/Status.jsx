export default function Status({ s, online }) {
  const v = s?.arm?.battery_mv ? (s.arm.battery_mv / 1000).toFixed(2) + ' V' : '— V'
  return (
    <header className="status">
      <div className="brand">xArm<span>voice</span></div>
      <div className={`state st-${s?.state || 'offline'}`}>
        <b>{online ? s?.state : 'offline'}</b>
        <small>{online ? s?.detail : 'no reply from xarm.local'}</small>
      </div>
      <div className="lights">
        <Light ok={online && s?.arm?.link} label="arm" />
        <Light ok={online && s?.voice?.ok} label="mic" />
        <span className="volt">{v}</span>
      </div>
    </header>
  )
}

const Light = ({ ok, label }) => <span className={`light ${ok ? 'on' : ''}`}>{label}</span>
