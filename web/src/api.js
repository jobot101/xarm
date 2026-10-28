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
  torque: (on)          => call('/api/torque', { on }),
  teach:  (what)        => call('/api/teach', { what }),
  getConfig: ()         => call('/api/config'),
  setConfig: (cfg)      => call('/api/config', cfg),
}

// servo index 0..5 = bus id 1..6 on the xArm 1S
export const JOINTS = ['Gripper', 'Wrist rot', 'Wrist', 'Elbow', 'Shoulder', 'Base']
