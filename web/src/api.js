// thin client for the firmware JSON API (firmware/main/web.c)
async function call(path, body) {
  const r = await fetch(path, body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (!r.ok) throw new Error(`${path} ${r.status}`)
  return r.json()
}

export const api = {
  state:  ()            => call('/api/state'),
  cmd:    (text)        => call('/api/cmd', { text }),
  jog:    (id, pos, ms) => call('/api/jog', { id, pos, ms }),
  drive:  (d)           => call('/api/drive', d),          // hold-to-move: {base|lift|reach|tilt|twist|grip: ±1, step, ms}
  torque: (on)          => call('/api/torque', { on }),
  teach:  (what)        => call('/api/teach', { what }),
  getConfig: ()         => call('/api/config'),
  setConfig: (cfg)      => call('/api/config', cfg),
  wifi:   (ssid, pass)  => call('/api/wifi', { ssid, pass }),
  update: (body)        => call('/api/update', body),   // {check:true} | {auto:bool}
  moves:  (action, extra = {}) => call('/api/moves', { action, ...extra }),   // record | save {name} | cancel | play {i} | delete {i}
  kids:   (action, extra = {}) => call('/api/kids', { action, ...extra }),     // add {name,emoji,jewel} | select {i} | remove {i} | score {game,score}
  voice:  (body)        => call('/api/voice', body),     // {gain:0..42 dB, thresh:5..90 %}
  limits: (i, min, max)  => call('/api/limits', { i, min, max }),
  i2c:    ()            => call('/api/i2c'),            // bus scan {found:'0x18 0x20 ...', cam:bool}
  phrases: ()           => call('/api/phrases'),
  sys:    ()            => call('/api/sys'),            // CPU per task, memory (Workshop -> System)         // everything the voice listens for
  fx:     (body)        => call('/api/fx', body),        // {show:0..3} | {event:'sad'|'happy'}
  school: ()            => call('/api/school'),          // Robot School: settings + each kid's progress
  setSchool: (body)     => call('/api/school', body),    // {on, minutes, pass, more:i, reset_day:i, restart:i}
}

// servo index 0..5 = bus id 1..6 on the xArm 1S
export const JOINTS = ['Gripper', 'Wrist rot', 'Wrist', 'Elbow', 'Shoulder', 'Base']
