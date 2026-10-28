// gzip dist/index.html into the firmware so the ESP32 serves it
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { gzipSync } from 'node:zlib'

const html = readFileSync(new URL('../dist/index.html', import.meta.url))
const gz = gzipSync(html, { level: 9 })
const out = new URL('../../firmware/main/www/', import.meta.url)
mkdirSync(out, { recursive: true })
writeFileSync(new URL('index.html.gz', out), gz)
console.log(`embedded ${html.length} -> ${gz.length} bytes gz`)
