# Handoff — solenoid tester (Ragamuffin Hall bell machine)

Repo `jobot101/xarm`, folder `solenoid/`. It's a one-MOSFET, one-solenoid test rig on any ESP32-S3. It's the first step toward the hall bell machine: 24 tubes, one push-pull solenoid per tube that both strikes and damps, played from a MIDI keyboard. It lives in the xarm repo only to share the phone flasher. **It is not part of the arm (Muffin).**

## Status (Nov 1 2026)
- Firmware written, compiles clean in CI, published. Latest build: `5360c13`.
- **Hardware:**
  - ESP32-S3 board with a MOSFET module on **GPIO 4**, grounds tied.
  - The test coil is the small **12 V gong solenoid from the Oracle phone**, on a **12 V supply**.
  - The real coils are **JF-0530B 12 V push-pull**, still in the mail. They'll run at **24 V**.
- Flash / bring-up on hardware is in progress. **Nothing has been verified on hardware yet.**
- Settings to use on the 12 V test rig: supply 12 V, strike min 5 / max 40 ms, hold kick 50 ms, hold 50 %, PWM 20 kHz (1 kHz if the module has an optocoupler).

## What the firmware does (`solenoid.ino`, Arduino core 3.2.0)
- **Strike:** a full-power pulse; velocity 1–127 maps linearly to min–max ms; one-shot `esp_timer` ends it.
- **Hold:** a full-power kick for kick ms, then PWM at hold % until released. It auto-releases after `mhold` seconds.
- **Repeat:** n strikes at a given bpm.
- **BOOT button (GPIO 0):** strike at velocity 100.
- **USB serial (CDC):** `s<vel>`, `h`, `r`, `x`, `p<gpio>`, `?`.
- **Supply setting:** at 24 V (2× coil rating) hold is capped at 50 % and a heat bucket limits full-power time to 400 ms burst and 300 ms/s average. At 12 V there are no limits and hold can go to 100 %.
- **Output:** LEDC at 10-bit resolution. `ledcWrite(pin, 1023)` means 100 % on. Active-low is done in software as `FULL - d`.
- **Allowed pins:** 1, 2, 4–18, 21, 38–44, 47, 48. Strapping pins, USB and flash/PSRAM pins are blocked.
- **Network:**
  - Its own AP **"Solenoid" / ragamuffin** at 192.168.4.1, with a captive DNS. This AP is always on.
  - Plus a station join to the hall WiFi. There's a **ClarkShop** preset button. **The password is NVS-only; never put it in the repo — the repo and the Pages site are public.**
  - Hostname `solenoid`, mDNS `solenoid.local`.
  - Station retries are paced: a 15 s attempt, then 45 s off. Without that, a nonstop retry hops channels and knocks phones off the AP.
- **Web page:** embedded in the firmware (`PAGE`, a raw string).
  - Endpoints: `/state`, `/s?v=`, `/h?on=`, `/stop`, `/r?n&bpm&v`, `/cfg?…`, `/scan`, `/wifi?ssid&pass|forget=1`, `POST /update` (multipart OTA).
  - **Install latest** fetches `https://jobot101.github.io/xarm/sol/manifest.json` + `solenoid.bin` through the phone's mobile data, then posts it to `/update`.
- **NVS namespace `sol`:** `pin inv min max kick hold mhold hz volts ssid wpass`.

## Build / ship
- **`.github/workflows/solenoid.yml`** runs on pushes to `solenoid/**`, or by hand with `gh workflow run solenoid.yml`.
  - It compiles with arduino-cli (FQBN `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=disabled,FlashSize=4M,PartitionScheme=default,FlashMode=dio`).
  - It writes `solenoid/version.h` (FW_VERSION = short SHA; the file is gitignored).
  - It publishes **only `gh-pages/sol/`** (`keep_files`). A run takes about 1.5 min.
- **`build.yml` (the arm) ignores `solenoid/**`.** Its pages job carries `sol/` over from gh-pages when it force-republishes.
  - **Never make solenoid changes trigger build.yml:** every arm build changes the version and Muffin auto-updates and reboots.
  - If a commit must touch build.yml or the flasher without rebuilding the arm, use `[skip ci]` and dispatch solenoid.yml by hand.
- **Flasher:** `flasher/` (`main.js` `BOARDS.sol`, `expectMB: 0` = any flash size; full erase + write of all 4 parts). The board button is "🔔 Solenoid". Flasher changes ride on build.yml's pages job.
- **Local compile:** not possible in the Claude cloud workspace, because the toolchain downloads are blocked. Use CI.
- **The owner works from an Android phone.**
  - First flash: https://jobot101.github.io/xarm/ → Solenoid.
  - After that: on the solenoid page, Firmware → **Install latest** (mobile data on, opened in Chrome, not the captive pop-up).

## Wiring rules (tell the owner if asked)
- **Grounds:** ESP GND goes to the module's signal-side GND. **Only ground is shared;** 12/24 V never touches the ESP.
- **Flyback diode across every coil** (cathode to +V). This is required because hold mode PWMs the coil.
- **MOSFET modules:** logic-level ones (D4184 / AOD4184) work. Red IRF520 boards don't fully turn on at 3.3 V.

## Unverified — check on hardware
1. Strike and hold actually fire; whether the module needs `inv` (active low).
2. Hold PWM at 20 kHz on this module. Buzzing or heat means try 1 kHz or check the flyback diode.
3. AP + station together: the phone stays on the "Solenoid" AP while the station joins ClarkShop, and the AP follows the station's channel.
4. `/scan` blocks about 2 s in AP_STA mode; it should be OK for a test page.
5. **Install latest** from the board's own AP over mobile data, and OTA `/update` ending in a reboot.
6. Whether `solenoid.local` resolves on Android; the fallbacks are `http://solenoid` or the IP shown in the Hall WiFi card.

## Next (the owner's direction for the bell machine)
- **MIDI in:** a 5-pin DIN keyboard through a 6N138 optocoupler into a UART at 31250 baud. Note-on velocity drives the strike; note held means hold/damp; note-off releases. As low latency as possible: "like a piano key".
- **Scale to 24 channels:** the plan is I2C solenoid drivers from the ESP32-S3 and MQTT into the hall's ecosystem. The owner already has MOSFET modules.
- **Coils:** JF-0530B at 24 V. Keep the 24 V heat limits; tune strike/kick/hold per tube.
- **Later:** his own key console that also outputs MIDI.
