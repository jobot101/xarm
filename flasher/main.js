// One-tap ESP32-S3 flasher for phones.
// Android Chrome can't reach the S3's built-in USB (USB-Serial-JTAG, 303A:1001) through Web Serial,
// so we talk to it over WebUSB with Google's web-serial-polyfill and hand that port to esptool-js.
import { ESPLoader, Transport } from 'esptool-js'
import { serial as usbSerial } from 'web-serial-polyfill'
import './style.css'

const ESPRESSIF_VID = 0x303a
const FW = './fw/'                 // CI copies the release files here
const APP_OFFSET = 0x10000

const $ = (id) => document.getElementById(id)
const logEl = $('log'), statusEl = $('status'), barEl = $('bar'), flashBtn = $('flash'), monBtn = $('monitor')

const log = (s = '') => { logEl.textContent += s; logEl.scrollTop = logEl.scrollHeight }
const status = (s, cls = '') => { statusEl.textContent = s; statusEl.className = 'status ' + cls }
const progress = (f) => { barEl.style.width = `${Math.round(f * 100)}%` }
const terminal = { clean() { logEl.textContent = '' }, writeLine: (s) => log(s + '\n'), write: (s) => log(s) }

let manifest = null
let busy = false

async function loadManifest() {
  try {
    manifest = await (await fetch(FW + 'manifest.json', { cache: 'no-store' })).json()
    const parts = manifest.builds[0].parts
    $('fw').textContent = `build ${manifest.version} · ${parts.length} files`
    flashBtn.disabled = false
  } catch (e) {
    $('fw').textContent = 'firmware files missing'
    status('Could not load firmware from this site: ' + e.message, 'bad')
  }
}

function serialApi() {
  if ('usb' in navigator) return usbSerial          // Android Chrome: WebUSB path
  if ('serial' in navigator) return navigator.serial // desktop Chrome fallback
  return null
}

async function pickPort() {
  const api = serialApi()
  if (!api) throw new Error('This browser has no WebUSB. Use Chrome on Android.')
  // reuse an already-permitted board without showing the picker again
  const known = (await api.getPorts?.()) || []
  const mine = known.find((p) => p.getInfo?.().usbVendorId === ESPRESSIF_VID)
  return mine || api.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] })
}

async function fetchBin(path) {
  const r = await fetch(FW + path, { cache: 'no-store' })
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`)
  return new Uint8Array(await r.arrayBuffer())
}

async function flash() {
  if (busy) return
  busy = true; flashBtn.disabled = true; monBtn.disabled = true
  const full = document.querySelector('input[name=mode]:checked').value === 'full'
  let transport
  try {
    terminal.clean(); progress(0)
    status('Pick “USB JTAG/serial debug unit” and allow access…')
    const port = await pickPort()

    status('Downloading firmware…')
    const parts = full
      ? manifest.builds[0].parts
      : manifest.builds[0].parts.filter((p) => p.offset === APP_OFFSET)
    const files = []
    for (const p of parts) {
      files.push({ data: await fetchBin(p.path), address: p.offset })
      log(`fetched ${p.path} (${files.at(-1).data.length} bytes) -> 0x${p.offset.toString(16)}\n`)
    }
    const total = files.reduce((n, f) => n + f.data.length, 0)
    const before = files.map((_, i) => files.slice(0, i).reduce((n, f) => n + f.data.length, 0))

    status('Connecting to the ESP32-S3…')
    transport = new Transport(port, false)
    // USB-Serial-JTAG ignores baud rate, so stay at 115200: no mid-session port reopen
    const loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal })
    const chip = await loader.main()
    log(`\nconnected: ${chip}\n`)

    status(full ? 'Erasing + writing (about 1–2 min)…' : 'Writing app…')
    await loader.writeFlash({
      fileArray: files,
      flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
      eraseAll: full,
      compress: true,
      reportProgress: (i, written, size) => progress((before[i] + (written / size) * files[i].data.length) / total),
    })
    progress(1)
    status('Done. Rebooting the board…', 'ok')
    await loader.after('hard_reset')
    status('Done ✓ — join WiFi “xarm-voice” (pw ragamuffin) → open 192.168.4.1. If nothing happens, tap RESET.', 'ok')
  } catch (e) {
    console.error(e)
    log(`\nERROR: ${e.message || e}\n`)
    const msg = String(e.message || e)
    if (/No device selected|requestDevice/.test(msg)) status('No board picked. Check the cable, or do BOOT+RESET and try again.', 'bad')
    else status('Failed: ' + msg + ' — try BOOT+RESET, then Flash again.', 'bad')
  } finally {
    try { await transport?.disconnect() } catch {}
    busy = false; flashBtn.disabled = !manifest; monBtn.disabled = false
  }
}

// read the board's boot log (115200) — shows what the firmware is doing after flashing
let monitorPort = null
async function monitor() {
  if (monitorPort) { try { await monitorPort.close() } catch {} monitorPort = null; monBtn.textContent = 'Boot log'; return }
  try {
    const port = await pickPort()
    await port.open({ baudRate: 115200 })
    monitorPort = port; monBtn.textContent = 'Stop log'
    terminal.clean(); log('--- boot log (tap RESET on the board to see it from the start) ---\n')
    const reader = port.readable.getReader(), dec = new TextDecoder()
    while (monitorPort) {
      const { value, done } = await reader.read()
      if (done) break
      log(dec.decode(value, { stream: true }))
    }
    reader.releaseLock()
  } catch (e) {
    log(`\nlog stopped: ${e.message || e}\n`)
    monitorPort = null; monBtn.textContent = 'Boot log'
  }
}

flashBtn.addEventListener('click', flash)
monBtn.addEventListener('click', monitor)
if (!serialApi()) status('This browser has no WebUSB. Open this page in Chrome on Android.', 'bad')
loadManifest()
