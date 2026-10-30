// Preview build only (npm run preview:build): the fake firmware runs inside the page, plus a small tuning
// panel to try the Learning page in different states. Never part of the firmware's web app.
import { school, handle, ASKS } from '../scripts/mock-api.js'

let offline = false
const realFetch = window.fetch.bind(window)
window.fetch = async (path, opt = {}) => {
  const url = typeof path === 'string' ? path : path.url
  if (!url.startsWith('/api/')) return realFetch(path, opt)
  await new Promise(r => setTimeout(r, 40))
  if (offline) throw new Error('offline')
  let body = {}
  try { body = opt.body ? JSON.parse(opt.body) : {} } catch {}
  const r = handle(opt.method || 'GET', url, body)
  return new Response(r === undefined ? '' : JSON.stringify(r), { status: r === undefined ? 404 : 200, headers: { 'Content-Type': 'application/json' } })
}

const kid = school.kids[0]
const css = `
#pv{position:fixed;z-index:50;right:10px;top:10px;font:13px/1.3 system-ui,sans-serif;color:#f4f1ea;background:#22262c;border:1px solid #000;
 border-radius:12px;box-shadow:0 6px 24px rgba(0,0,0,.35);width:230px;max-width:calc(100vw - 20px)}
#pv summary{cursor:pointer;padding:9px 12px;font-weight:700;list-style:none}
#pv summary::-webkit-details-marker{display:none}
#pv .b{padding:0 12px 12px;display:grid;gap:9px}
#pv label{display:grid;gap:3px;color:#b9bfc8}
#pv label b{color:#f4f1ea;font-weight:600}
#pv input[type=range]{width:100%;accent-color:#7aa356}
#pv select,#pv input[type=text]{font:inherit;padding:5px 6px;border-radius:7px;border:1px solid #555;background:#2f343b;color:#f4f1ea}
#pv .row{display:flex;gap:6px;flex-wrap:wrap}
#pv button{font:inherit;padding:5px 9px;border-radius:7px;border:1px solid #555;background:#2f343b;color:#f4f1ea;cursor:pointer}
`
const el = document.createElement('details')
el.id = 'pv'
el.innerHTML = `<summary>Preview ▾</summary><div class="b">
 <label>Kid's name <input type="text" id="pv-name" value="${kid.name}"></label>
 <label>Lessons done: <b id="pv-ln">${kid.lesson}</b><input type="range" id="pv-lesson" min="0" max="${school.lesson_count}" value="${kid.lesson}"></label>
 <label>Stars: <b id="pv-sn">${kid.stars}</b><input type="range" id="pv-stars" min="0" max="160" value="${kid.stars}"></label>
 <label>Days in a row: <b id="pv-kn">${kid.streak}</b><input type="range" id="pv-streak" min="0" max="30" value="${kid.streak}"></label>
 <label>Minutes used today: <b id="pv-un">${Math.round(kid.used_s / 60)}</b><input type="range" id="pv-used" min="0" max="20" value="${Math.round(kid.used_s / 60)}"></label>
 <label>Muffin is <select id="pv-state"><option>waiting</option><option>in class</option><option>paused</option><option>asleep</option><option value="offline">offline</option></select></label>
 <label>Muffin asks <select id="pv-ask"><option value="">nothing</option><option value="tf">a true/false question</option><option value="yn">another lesson?</option><option value="turn">your turn: turn</option><option value="nudge">your turn: any nudge</option><option value="stop">shout stop!</option><option value="free">free play</option><option value="fetch">hand me something</option><option value="giveback">take it back</option><option value="start">waiting: start class</option></select></label>
 <div class="row"><button id="pv-mode">Open Workshop</button></div>
</div>`
const style = document.createElement('style'); style.textContent = css
document.head.appendChild(style)
const mount = () => document.body.appendChild(el)
document.body ? mount() : addEventListener('DOMContentLoaded', mount)

const $ = id => el.querySelector('#' + id)
const bind = (id, out, set) => $(id).addEventListener('input', e => { set(+e.target.value); $(out).textContent = e.target.value })
bind('pv-lesson', 'pv-ln', v => (kid.lesson = v))
bind('pv-stars', 'pv-sn', v => (kid.stars = v))
bind('pv-streak', 'pv-kn', v => (kid.streak = v))
bind('pv-used', 'pv-un', v => (kid.used_s = v * 60))
$('pv-name').addEventListener('input', e => (kid.name = e.target.value.trim().toLowerCase() || 'sam'))
$('pv-state').addEventListener('change', e => { offline = e.target.value === 'offline'; if (!offline) school.state = e.target.value })
$('pv-ask').addEventListener('change', e => {
  const a = ASKS[e.target.value]
  if (a) { school.ask = JSON.parse(JSON.stringify(a)); school.state = a.kind === 'start' ? 'waiting' : 'in class'; $('pv-state').value = school.state; offline = false }
  else delete school.ask
})
setInterval(() => { if (!school.ask && $('pv-ask').value) $('pv-ask').value = '' }, 500)   // answered: back to nothing
$('pv-mode').addEventListener('click', () => {
  const ws = document.querySelector('.app.mode-workshop')
  try { localStorage.setItem('mode', ws ? 'play' : 'workshop') } catch {}
  location.reload()
})
const label = () => { const ws = document.querySelector('.app.mode-workshop'); $('pv-mode').textContent = ws ? 'Back to lessons' : 'Open Workshop' }
setInterval(label, 500)
