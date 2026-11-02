// Solenoid tester: one MOSFET, one solenoid, any ESP32-S3 board.
// Flashed from the Ragamuffin Flasher (board "Solenoid"). Then join WiFi "Solenoid" (pw ragamuffin) -> 192.168.4.1
//
// Drive: GPIO -> logic-level MOSFET module gate (or a bare logic-level N-FET: 100 R series, 10k gate->GND).
//        Flyback diode across the coil (cathode to +V) is required: the hold mode PWMs the coil.
// Modes: strike = full-power pulse, length set by velocity (min..max ms), then off
//        hold   = full-power kick, then PWM hold at hold% until released (auto-off after maxHold)
// Built for 12 V coils run at 24 V: hold% is capped at 50, full-power on-time is rate-limited (heat bucket).
// Also: BOOT button = strike, USB serial commands (type ? for help), WiFi firmware update at /update.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_timer.h"

#if __has_include("version.h")
#include "version.h"                // CI writes FW_VERSION = the short commit SHA (same as sol/manifest.json)
#endif
#ifndef FW_VERSION
#define FW_VERSION  "dev"
#endif
#define LATEST_URL  "https://jobot101.github.io/xarm/sol/"
#define AP_SSID     "Solenoid"
#define AP_PASS     "ragamuffin"
#define BOOT_BTN    0
#define RES_BITS    10
#define FULL        1023          // ledcWrite turns max into 100 % on
#define BUCKET_MS   400.0f        // full-power ms that can go out back to back (24 V on a 12 V coil)
#define REFILL_MS_S 300.0f        // full-power ms per second on average (30 % duty at 24 V ~ 1.2x a 12 V coil's rating)
                                  // at 12 V (= coil rating) the limiter is off and hold can go to 100 %

struct Cfg {
  int pin = 4;
  bool inv = false;               // active-low driver
  int minMs = 3, maxMs = 25;      // strike pulse at velocity 1 / 127
  int kickMs = 30;                // hold: full-power pull-in
  int holdPct = 35;               // hold: PWM duty after the kick (35 % of 24 V ~ 8.4 V avg)
  int maxHoldS = 8;               // hold auto-release
  int hz = 20000;                 // PWM freq (opto-isolated modules: 1000)
  int volts = 24;                 // supply: 12 = coil rating (no limits), 24 = 2x (heat-limited, hold <= 50 %)
} cfg;

enum State { IDLE, STRIKE, KICK, HOLD };
volatile State state = IDLE;
esp_timer_handle_t tmr;
Preferences prefs;
WebServer server(80);
DNSServer dns;

float bucket = BUCKET_MS;
uint32_t lastRefill = 0, strikes = 0, clipped = 0;
int repLeft = 0, repV = 100;
uint32_t repNext = 0, repGap = 250;

// ---------- output ----------
static inline void out(uint32_t d) { ledcWrite(cfg.pin, cfg.inv ? FULL - d : d); }
static inline void off() { out(0); }
static inline int holdMax() { return cfg.volts <= 12 ? 100 : 50; }
static inline uint32_t holdDuty() { return (uint32_t)FULL * constrain(cfg.holdPct, 5, holdMax()) / 100; }

void IRAM_ATTR onTimer(void*) {
  if (state == KICK) {            // pull-in done -> hold at reduced duty, arm the auto-release
    out(holdDuty()); state = HOLD;
    esp_timer_start_once(tmr, (uint64_t)cfg.maxHoldS * 1000000ULL);
  } else { off(); state = IDLE; } // strike over, or hold timed out
}

void refill() {
  uint32_t now = millis();
  if (cfg.volts <= 12) { bucket = BUCKET_MS; lastRefill = now; return; }   // rated voltage: no heat limit
  bucket = min(BUCKET_MS, bucket + (now - lastRefill) * REFILL_MS_S / 1000.0f);
  lastRefill = now;
}

int msForVel(int v) {
  v = constrain(v, 1, 127);
  return cfg.minMs + (cfg.maxMs - cfg.minMs) * (v - 1) / 126;
}

// returns ms actually fired (0 = skipped: too hot)
int strike(int vel) {
  refill();
  int ms = msForVel(vel);
  if (bucket < ms) { clipped++; ms = (int)bucket; if (ms < cfg.minMs) return 0; }
  bucket -= ms;
  esp_timer_stop(tmr);
  state = STRIKE; out(FULL); strikes++;
  esp_timer_start_once(tmr, (uint64_t)ms * 1000ULL);
  return ms;
}

bool holdOn() {
  refill();
  if (bucket < cfg.kickMs) { clipped++; return false; }
  bucket -= cfg.kickMs;
  esp_timer_stop(tmr);
  state = KICK; out(FULL);
  esp_timer_start_once(tmr, (uint64_t)cfg.kickMs * 1000ULL);
  return true;
}

void release() { esp_timer_stop(tmr); off(); state = IDLE; }
void stopAll() { repLeft = 0; release(); }

bool pinOk(int p) {   // S3: no strapping (0,3,45,46), USB (19,20), flash/PSRAM (26-37)
  return (p >= 1 && p <= 18 && p != 3) || p == 21 || (p >= 38 && p <= 44) || p == 47 || p == 48;
}

void attachPin(int oldPin) {
  if (oldPin >= 0) { ledcDetach(oldPin); pinMode(oldPin, INPUT_PULLDOWN); }
  ledcAttach(cfg.pin, cfg.hz, RES_BITS);
  off();
}

// ---------- config ----------
void loadCfg() {
  prefs.begin("sol", true);
  cfg.pin = prefs.getInt("pin", cfg.pin);       cfg.inv = prefs.getBool("inv", cfg.inv);
  cfg.minMs = prefs.getInt("min", cfg.minMs);   cfg.maxMs = prefs.getInt("max", cfg.maxMs);
  cfg.kickMs = prefs.getInt("kick", cfg.kickMs); cfg.holdPct = prefs.getInt("hold", cfg.holdPct);
  cfg.maxHoldS = prefs.getInt("mhold", cfg.maxHoldS); cfg.hz = prefs.getInt("hz", cfg.hz);
  cfg.volts = prefs.getInt("volts", cfg.volts);
  prefs.end();
  if (!pinOk(cfg.pin)) cfg.pin = 4;
}

void saveCfg() {
  prefs.begin("sol", false);
  prefs.putInt("pin", cfg.pin);     prefs.putBool("inv", cfg.inv);
  prefs.putInt("min", cfg.minMs);   prefs.putInt("max", cfg.maxMs);
  prefs.putInt("kick", cfg.kickMs); prefs.putInt("hold", cfg.holdPct);
  prefs.putInt("mhold", cfg.maxHoldS); prefs.putInt("hz", cfg.hz);
  prefs.putInt("volts", cfg.volts);
  prefs.end();
}

String cfgJson() {
  refill();
  static const char* names[] = {"idle", "strike", "kick", "hold"};
  char b[400];
  snprintf(b, sizeof b,
    "{\"fw\":\"%s\",\"pin\":%d,\"inv\":%d,\"min\":%d,\"max\":%d,\"kick\":%d,\"hold\":%d,\"mhold\":%d,\"hz\":%d,\"volts\":%d,"
    "\"state\":\"%s\",\"heat\":%d,\"strikes\":%lu,\"clipped\":%lu,\"rep\":%d}",
    FW_VERSION, cfg.pin, cfg.inv, cfg.minMs, cfg.maxMs, cfg.kickMs, cfg.holdPct, cfg.maxHoldS, cfg.hz, cfg.volts,
    names[state], (int)(100 - bucket * 100 / BUCKET_MS), (unsigned long)strikes, (unsigned long)clipped, repLeft);
  return b;
}

String applyCfg(std::function<bool(const char*)> has, std::function<int(const char*)> get) {
  int oldPin = cfg.pin, oldHz = cfg.hz; bool oldInv = cfg.inv;
  if (has("pin")) { int p = get("pin"); if (!pinOk(p)) return "pin not allowed"; cfg.pin = p; }
  if (has("inv"))   cfg.inv = get("inv");
  if (has("min"))   cfg.minMs = constrain(get("min"), 1, 100);
  if (has("max"))   cfg.maxMs = constrain(get("max"), 1, 100);
  if (has("kick"))  cfg.kickMs = constrain(get("kick"), 5, 100);
  if (has("volts")) cfg.volts = get("volts") <= 12 ? 12 : 24;
  if (has("hold"))  cfg.holdPct = get("hold");
  cfg.holdPct = constrain(cfg.holdPct, 5, holdMax());
  if (has("mhold")) cfg.maxHoldS = constrain(get("mhold"), 1, 60);
  if (has("hz"))    cfg.hz = constrain(get("hz"), 100, 40000);
  if (cfg.maxMs < cfg.minMs) cfg.maxMs = cfg.minMs;
  if (cfg.pin != oldPin || cfg.hz != oldHz || cfg.inv != oldInv) { stopAll(); attachPin(oldPin); }
  saveCfg();
  return "";
}

// ---------- web ----------
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Solenoid</title><style>
:root{--bg:#121212;--card:#1d1d1d;--fg:#eee;--dim:#999;--acc:#e8a33d;--bad:#e05a4f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 12px}h1 small{color:var(--dim);font-weight:400}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px}
button{font:inherit;border:0;border-radius:10px;padding:12px;background:#333;color:var(--fg);width:100%;touch-action:none;user-select:none;-webkit-user-select:none}
.big{font-size:22px;padding:26px;background:var(--acc);color:#111;font-weight:700}
.big:active,.held{filter:brightness(1.3)}.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;color:var(--dim)}
input[type=range]{width:100%}input[type=number],select{width:90px;font:inherit;padding:6px;border-radius:8px;border:0;background:#333;color:var(--fg)}
#st{font:13px ui-monospace,monospace;color:var(--dim)}.heat{height:6px;background:#333;border-radius:3px;margin-top:6px}.heat div{height:100%;background:var(--bad);border-radius:3px;width:0}
</style></head><body><main>
<h1>Solenoid <small id="fw"></small></h1>
<div class="card"><button class="big" id="strike">STRIKE</button>
<label>velocity <b id="vv">100</b></label><input type="range" id="vel" min="1" max="127" value="100">
<div class="row"><button id="hold">HOLD (press)</button><button id="stop">STOP</button></div>
<div class="heat"><div id="heat"></div></div><div id="st">…</div></div>
<div class="card"><b>Repeat</b>
<label>count <input type="number" id="n" value="16"></label><label>bpm <input type="number" id="bpm" value="240"></label>
<button id="rep">Go</button></div>
<div class="card"><b>Settings</b>
<label>supply <select id="volts"><option value="12">12 V (coil rating)</option><option value="24">24 V (2× coil)</option></select></label>
<label>GPIO <select id="pin"></select></label>
<label>active low <input type="checkbox" id="inv"></label>
<label>strike min ms <input type="number" id="min"></label><label>strike max ms <input type="number" id="max"></label>
<label>hold kick ms <input type="number" id="kick"></label><label>hold % <input type="number" id="hold_"></label>
<label>hold auto-off s <input type="number" id="mhold"></label><label>PWM Hz <input type="number" id="hz"></label>
<button id="save">Save</button></div>
<div class="card"><b>Firmware</b> <small id="fwv"></small>
<div class="row"><button id="latest">Install latest</button></div>
<p style="color:var(--dim);font-size:13px">Needs mobile data on, page open in Chrome (not the WiFi sign-in pop-up).</p>
<input type="file" id="bin" accept=".bin"><div class="row"><button id="up">Upload file</button></div></div>
<script>
const LATEST=')HTML" LATEST_URL R"HTML(';
const $=id=>document.getElementById(id),go=u=>fetch(u).then(r=>r.json()).then(show).catch(()=>{});
const PINS=[1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,43,44,47,48];
PINS.forEach(p=>$('pin').add(new Option('GPIO '+p,p)));
let c={};
function show(j){if(!j||j.err){$('st').textContent=j?j.err:'no reply';return}c=j;
 $('fw').textContent=j.fw;$('heat').style.width=j.heat+'%';
 $('st').textContent=`GPIO ${j.pin} · ${j.state} · ${Math.round(j.min+(j.max-j.min)*($('vel').value-1)/126)} ms · strikes ${j.strikes} · heat-clipped ${j.clipped}${j.rep?' · repeat '+j.rep:''}`;
 $('rep').textContent=j.rep?'Stop':'Go'}
function fill(j){show(j);$('fwv').textContent='running '+j.fw;$('volts').value=j.volts;$('pin').value=j.pin;$('inv').checked=!!j.inv;
 ['min','max','kick','mhold','hz'].forEach(k=>$(k).value=j[k]);$('hold_').value=j.hold}
$('vel').oninput=()=>{$('vv').textContent=$('vel').value;show(c)};
$('strike').addEventListener('pointerdown',e=>{e.preventDefault();go('/s?v='+$('vel').value)});
const h=$('hold');
h.addEventListener('pointerdown',e=>{e.preventDefault();h.setPointerCapture(e.pointerId);h.classList.add('held');go('/h?on=1')});
['pointerup','pointercancel','lostpointercapture'].forEach(t=>h.addEventListener(t,()=>{if(h.classList.contains('held')){h.classList.remove('held');go('/h?on=0')}}));
$('stop').onclick=()=>go('/stop');
$('rep').onclick=()=>go(c.rep?'/stop':`/r?n=${$('n').value}&bpm=${$('bpm').value}&v=${$('vel').value}`);
$('save').onclick=()=>go(`/cfg?volts=${$('volts').value}&pin=${$('pin').value}&inv=${$('inv').checked?1:0}&min=${$('min').value}&max=${$('max').value}&kick=${$('kick').value}&hold=${$('hold_').value}&mhold=${$('mhold').value}&hz=${$('hz').value}`).then(()=>fetch('/state').then(r=>r.json()).then(fill));
function send(blob,btn){const x=new XMLHttpRequest(),d=new FormData();d.append('f',blob,'solenoid.bin');
 x.upload.onprogress=e=>btn.textContent='writing '+Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{btn.textContent=x.responseText+' — rebooting';setTimeout(()=>location.reload(),5000)};
 x.onerror=()=>btn.textContent='upload failed';x.open('POST','/update');x.send(d)}
$('up').onclick=()=>{const f=$('bin').files[0];if(f)send(f,$('up'))};
$('latest').onclick=async()=>{const b=$('latest');try{
 b.textContent='checking…';const m=await(await fetch(LATEST+'manifest.json',{cache:'no-store'})).json();
 if(m.version===c.fw&&!confirm('Already on '+m.version+'. Reinstall?')){b.textContent='up to date ('+m.version+')';return}
 b.textContent='downloading '+m.version+'…';const r=await fetch(LATEST+'solenoid.bin',{cache:'no-store'});if(!r.ok)throw 0;
 send(await r.blob(),b)}catch(e){b.textContent='no internet — mobile data on, open in Chrome'}};
fetch('/state').then(r=>r.json()).then(fill);setInterval(()=>go('/state'),700);
</script></main></body></html>)HTML";

void sendState() { server.send(200, "application/json", cfgJson()); }
int arg(const char* k, int d) { return server.hasArg(k) ? server.arg(k).toInt() : d; }

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/state", sendState);
  server.on("/s", [] { strike(arg("v", 100)); sendState(); });
  server.on("/h", [] { if (arg("on", 0)) holdOn(); else release(); sendState(); });
  server.on("/stop", [] { stopAll(); sendState(); });
  server.on("/r", [] {
    repV = arg("v", 100); repGap = 60000 / constrain(arg("bpm", 240), 10, 1200);
    repLeft = constrain(arg("n", 16), 1, 10000); repNext = millis(); sendState();
  });
  server.on("/cfg", [] {
    String e = applyCfg([](const char* k) { return server.hasArg(k); }, [](const char* k) { return (int)server.arg(k).toInt(); });
    if (e.length()) server.send(400, "application/json", "{\"err\":\"" + e + "\"}"); else sendState();
  });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "ok" : "update failed");
    if (ok) { delay(300); ESP.restart(); }
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { stopAll(); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) Update.end(true);
  });
  server.onNotFound([] { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); });  // captive portal
  server.begin();
}

// ---------- serial ----------
void serialCmd() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim(); if (!line.length()) continue;
    char c = line[0]; int n = line.length() > 1 ? line.substring(1).toInt() : -1;
    if (c == 's') Serial.printf("strike %d ms\n", strike(n > 0 ? n : 100));
    else if (c == 'h') Serial.println(holdOn() ? "hold" : "too hot");
    else if (c == 'r') { release(); Serial.println("released"); }
    else if (c == 'x') { stopAll(); Serial.println("stopped"); }
    else if (c == 'p') {
      String e = applyCfg([](const char* k) { return !strcmp(k, "pin"); }, [n](const char*) { return n; });
      Serial.println(e.length() ? e : "pin " + String(cfg.pin));
    } else Serial.println(cfgJson() + "\ns<vel> strike · h hold · r release · x stop · p<gpio> set pin · ? state");
    line = "";
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  loadCfg();
  attachPin(-1);
  esp_timer_create_args_t ta = {}; ta.callback = onTimer; ta.name = "sol";
  esp_timer_create(&ta, &tmr);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  lastRefill = millis();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  dns.start(53, "*", WiFi.softAPIP());
  setupWeb();
  Serial.printf("\n%s  solenoid on GPIO %d  WiFi \"%s\" -> http://%s\n", FW_VERSION, cfg.pin, AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  serialCmd();

  static bool was = true; static uint32_t tDown = 0;      // BOOT button: strike at velocity 100
  bool up = digitalRead(BOOT_BTN);
  if (!up && was && millis() - tDown > 120) { tDown = millis(); strike(100); }
  was = up;

  if (repLeft > 0 && (int32_t)(millis() - repNext) >= 0) { strike(repV); repLeft--; repNext += repGap; }
  delay(1);
}
