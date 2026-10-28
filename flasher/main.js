// One-tap ESP32-S3 flasher for phones, for both boards:
//   arm brain = Waveshare ESP32-S3-AUDIO-Board  (native USB-Serial-JTAG, 303A:1001, 16 MB flash)
//   camera    = Hiwonder ESP32-S3 vision module (4 MB flash; its USB may be native or a CH340 bridge)
// Android Chrome can't reach these through Web Serial, so we open them over WebUSB:
//   303A (native)  -> Google's web-serial-polyfill (CDC-ACM)
//   1A86 (CH340)   -> esptool-js's own WebUSBSerialPort
import { ESPLoader, Transport, WebUSBSerialPort } from 'esptool-js'
import { SerialPort as CdcPort } from 'web-serial-polyfill'
import './style.css'

const ESPRESSIF = 0x303a, WCH = 0x1a86
const BOARDS = {
  arm: { base: './fw/', label: 'arm brain', expectMB: 16, appOffset: 0x10000 },
  cam: { base: './cam/', label: 'camera', expectMB: 4 },
}

const $ = (id) => document.getElementById(id)
const logEl = $('log'), statusEl = $('status'), barEl = $('bar'), flashBtn = $('flash'), monBtn = $('monitor')
const log = (s = '') => { logEl.textContent += s; logEl.scrollTop = logEl.scrollHeight }
const status = (s, cls = '') => { statusEl.textContent = s; statusEl.className = 'status ' + cls }
const progress = (f) => { barEl.style.width = `${Math.round(f * 100)}%` }
const terminal = { clean() { logEl.textContent = '' }, writeLine: (s) => log(s + '\n'), write: (s) => log(s) }

let board = 'arm'
let manifests = {}          // arm: manifest, cam: { qspi, opi }
let busy = false

const getJSON = async (u) => (await fetch(u, { cache: 'no-store' })).json()

async function loadManifests() {
  try { manifests.arm = await getJSON(BOARDS.arm.base + 'manifest.json') } catch {}
  try {
    manifests.cam = { qspi: await getJSON(BOARDS.cam.base + 'qspi/manifest.json'), opi: await getJSON(BOARDS.cam.base + 'opi/manifest.json') }
  } catch {}
  render(true)
}

function render(hint = false) {
  document.querySelectorAll('.board').forEach(b => b.classList.toggle('on', b.dataset.board === board))
  $('modes').style.display = board === 'arm' ? '' : 'none'
  const m = board === 'arm' ? manifests.arm : manifests.cam?.qspi
  $('fw').textContent = m ? `build ${m.version}` : 'files missing'
  flashBtn.disabled = !m || busy
  flashBtn.textContent = board === 'arm' ? 'Flash arm brain' : 'Flash camera'
  if (hint) status(board === 'arm'
    ? 'Plug the Waveshare board (round, with the speaker) into this phone.'
    : 'Plug the camera module into this phone by its own USB-C. Unplug it from the arm first.')
}

document.querySelectorAll('.board').forEach(b => b.addEventListener('click', () => { if (!busy) { board = b.dataset.board; render(true) } }))

// pick a board over WebUSB and wrap it as a Web-Serial-style port esptool-js understands
async function pickPort() {
  if (!('usb' in navigator)) throw new Error('This browser has no WebUSB. Use Chrome on Android.')
  const known = (await navigator.usb.getDevices()).filter(d => d.vendorId === ESPRESSIF || d.vendorId === WCH)
  const dev = known[0] || await navigator.usb.requestDevice({ filters: [{ vendorId: ESPRESSIF }, { vendorId: WCH }] })
  if (dev.vendorId === WCH) return new WebUSBSerialPort(dev).asSerialPort()
  return new CdcPort(dev)
}

async function fetchBin(base, path) {
  const r = await fetch(base + path, { cache: 'no-store' })
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`)
  return new Uint8Array(await r.arrayBuffer())
}

async function flash() {
  if (busy) return
  busy = true; render(); monBtn.disabled = true
  const B = BOARDS[board]
  const full = board === 'cam' || document.querySelector('input[name=mode]:checked').value === 'full'
  let transport
  try {
    terminal.clean(); progress(0)
    status('Pick the board in the list and allow access…')
    const port = await pickPort()

    status('Connecting…')
    transport = new Transport(port, false)
    // USB-Serial-JTAG ignores baud rate, so stay at 115200: no mid-session port reopen
    const loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal })
    log(`connected: ${await loader.main()}\n`)

    // sanity: is this the board we think it is? (wrong firmware on the wrong board won't boot)
    const features = (await loader.chip.getChipFeatures(loader)).join(', ')
    const flashMB = parseInt(loader.DETECTED_FLASH_SIZES[(await loader.readFlashId()) >> 16 & 0xff] || '0')
    log(`features: ${features}\nflash: ${flashMB} MB\n`)
    if (flashMB && flashMB !== B.expectMB &&
        !confirm(`This board has ${flashMB} MB flash — the ${B.label} should have ${B.expectMB} MB. Is it the right board? Flash anyway?`))
      throw new Error('cancelled — wrong board?')

    let man = manifests.arm, base = B.base
    if (board === 'cam') {   // octal PSRAM (8 MB, 1.8 V) vs quad (2 MB) — pick the matching build
      const opi = /PSRAM 8MB|Octal|1v8/i.test(features)
      man = opi ? manifests.cam.opi : manifests.cam.qspi
      base = B.base + (opi ? 'opi/' : 'qspi/')
      log(`camera build: ${opi ? 'octal' : 'quad'} PSRAM\n`)
    }

    status('Downloading firmware…')
    const parts = full ? man.builds[0].parts : man.builds[0].parts.filter(p => p.offset === B.appOffset)
    const files = []
    for (const p of parts) {
      files.push({ data: await fetchBin(base, p.path), address: p.offset })
      log(`fetched ${p.path} (${files.at(-1).data.length} bytes) -> 0x${p.offset.toString(16)}\n`)
    }
    const total = files.reduce((n, f) => n + f.data.length, 0)
    const before = files.map((_, i) => files.slice(0, i).reduce((n, f) => n + f.data.length, 0))

    status(full ? 'Erasing + writing (about 1–2 min)…' : 'Writing app…')
    await loader.writeFlash({
      fileArray: files, flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
      eraseAll: full, compress: true,
      reportProgress: (i, written, size) => progress((before[i] + (written / size) * files[i].data.length) / total),
    })
    progress(1)
    status('Done. Rebooting…', 'ok')
    await loader.after('hard_reset')
    status(board === 'arm'
      ? 'Done ✓ — listen for “Ready to rumble!”, then join WiFi “xarm-voice” (pw ragamuffin) → 192.168.4.1'
      : 'Camera done ✓ — plug it back onto the arm (5V, GND, SDA, SCL). If nothing happens, tap its reset.', 'ok')
  } catch (e) {
    console.error(e)
    log(`\nERROR: ${e.message || e}\n`)
    const msg = String(e.message || e)
    if (/No device selected|requestDevice/.test(msg)) status('No board picked. Check the cable, or do BOOT+RESET and try again.', 'bad')
    else status('Failed: ' + msg + ' — try BOOT+RESET, then Flash again.', 'bad')
  } finally {
    try { await transport?.disconnect() } catch {}
    busy = false; render(); monBtn.disabled = false
  }
}

// read the board's boot log (115200)
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
if (!('usb' in navigator)) status('This browser has no WebUSB. Open this page in Chrome on Android.', 'bad')
loadManifests()
