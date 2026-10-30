// fake firmware API so the UI can be developed with no hardware:
//   terminal 1: node scripts/mock.js      terminal 2: ARM=http://localhost:8787 npm run dev
// or with no server at all: npm run preview:build -> dist-preview/index.html (mock runs in the page)
import { createServer } from 'node:http'
import { st, handle } from './mock-api.js'

createServer((req, res) => {
  let b = ''
  req.on('data', d => (b += d))
  req.on('end', () => {
    const raw = req.url === '/api/ota' || req.url.startsWith('/api/sounds')
    if (req.method === 'POST' && req.url.startsWith('/api/sounds')) st.fw.sounds = new URL(req.url, 'http://x').searchParams.get('v')
    const body = b && !raw ? JSON.parse(b) : {}   // OTA / voice bodies are raw .bin, not json
    const r = req.method === 'POST' && req.url.startsWith('/api/sounds') ? { ok: true, msg: 'voice updated' } : handle(req.method, req.url, body)
    if (r === undefined) { res.statusCode = 404; return res.end() }
    res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(r))
  })
}).listen(8787, () => console.log('mock firmware on http://localhost:8787'))
