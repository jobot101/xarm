# CLAUDE.md — xarm-voice

Voice and web control for a Hiwonder xArm 1S. The brain is a Waveshare ESP32-S3-AUDIO-Board running ESP-IDF; a Hiwonder ESP32-S3 vision module (color mode) gives it eyes. The goal is "pick up the red" → the arm finds the red object, grabs it, and holds it at Home. README.md covers wiring and user setup; read it first.

## Status

- `core.c` passes its host tests. The frames match the byte examples in Hiwonder's protocol docs.
- The web app builds and has been checked against the mock API.
- **The firmware compiles clean in CI** (ESP-IDF v5.5.1, first try) and the `latest` release is published. It has **not been run on hardware yet**.
- The partition table already has two OTA app slots (`ota_0`/`ota_1`), and rollback is on in `sdkconfig.defaults`. The OTA code itself is task 3 below.

## The job, in order

The owner wants to **flash and update this board from an Android phone**. A PC is only used for building.

**1. Build.** Run the host tests, then `idf.py set-target esp32s3 && idf.py build`. Fix API drift (ESP-IDF 5.5, esp-sr 2.4.7, esp_codec_dev 1.6) until the build is clean, without restructuring anything.

**2. Phone flash kit (CI).** The owner works from a phone, so **GitHub Actions is the build machine** (`.github/workflows/build.yml`). Every push to `main` builds and publishes the GitHub release `latest` with the files below. Do the compile-fix loop by pushing and reading the Actions log (`gh run watch`, `gh run view --log-failed`); don't install ESP-IDF on the phone. The release contains:

| file | offset | used for |
|---|---|---|
| `bootloader.bin` | 0x0 | first USB flash |
| `partition-table.bin` | 0x8000 | first USB flash |
| `ota_data_initial.bin` | 0xd000 | first USB flash |
| `xarm-voice.bin` | 0x10000 | first USB flash, **and** every OTA update after that |
| `srmodels.bin` | 0x810000 | first USB flash (speech models; only reflash if the model changes) |

CI also writes `OFFSETS.txt` and an ESP Web Tools `manifest.json`, taking the offsets from `build/flasher_args.json`. Files land at `https://github.com/jobot101/xarm/releases/download/latest/<file>`.

The first flash happens from the phone over USB-C using **`flasher/`**, a one-tap page that CI deploys to https://jobot101.github.io/xarm/ (gh-pages branch) with that build's files baked in. Android Chrome can't open the S3's built-in USB-Serial-JTAG (303A:1001) through Web Serial, and esptool-js's own WebUSB mode only supports CH340. So the flasher hands esptool-js a port from Google's `web-serial-polyfill`, which drives the board's CDC-ACM interfaces over WebUSB. Keep baud at 115200 (USB-JTAG ignores it, and a baud change would reopen the port). Every flash after the first is OTA.

**3. OTA from the phone.** Add this, following the existing patterns:
- `ota.c`: `POST /api/ota` takes the raw `.bin` body and streams it with `esp_ota_begin` / `esp_ota_write` (4 KB chunks, heap buffer) / `esp_ota_end`, then `esp_ota_set_boot_partition`, replies `{ok}`, and restarts after 500 ms. Refuse while `pick_busy()`, and call `pick_stop()` first. Raise httpd `recv_wait_timeout` for the upload.
- In `app_main`, after `web_start()` succeeds, call `esp_ota_mark_app_valid_cancel_rollback()`. A build that can't reach the web server rolls itself back.
- Add `fw: {version, partition}` to `/api/state`. `version` comes from `esp_app_get_description()`; set `PROJECT_VER` in the top-level CMakeLists.
- Web app: a **Firmware** panel with the current version, a `.bin` file picker, an upload progress bar (XHR `upload.onprogress`; fetch has no upload progress), then "rebooting…" and a poll of `/api/state` until it answers with the new version. Add a matching handler to `scripts/mock.js`.
- The web app is embedded in the firmware, so one `.bin` updates both the UI and the firmware.

**Auto-update loop (done):** with home WiFi set (web app → WiFi), `update.c` polls `https://jobot101.github.io/xarm/fw/manifest.json` every 60 s. When the `version` there (the short commit SHA; CI stamps the same value into `firmware/version.txt` → `PROJECT_VER`) differs from the running build, it installs `xarm-voice.bin` over HTTPS and reboots. Each version is tried once, so a build that rolls back isn't reflashed in a loop. **So shipping a change is just: push to `main`.** About 3 min of CI plus up to 1 min for the board's check. Don't break this path. If a change needs `srmodels.bin` or the partition table reflashed, say so, because the phone flasher has to do those.

**Play / Workshop (done):** the web app opens in **Play** mode for kids: big picture tiles, plain-words status, recording their own moves, and the Copy the Bells game. Hold the rose window for 3 s to reach **Workshop** (all the setup panels). Keep Play jargon-free. New tricks need a router phrase (voice), a Play tile, and an `OP_` in `pick.c`.

**Head-mounted camera (done, default):** `cfg.cam_on_head`. Picks start from the taught **Look** pose, then sweep `scan[]` base offsets. Calibration is taught from Look, and a base offset just rotates the picture, so it gets added to the grasp base.

**Ragamuffin Hall theme:** rose-window LED ring in stained-glass jewels, bell and organ sounds (`fx.c`), and oak/brass/jewel UI. Keep new features in that voice.

**Camera firmware (done):** `cam/xarm_color` is Hiwonder's color sketch (originals in `cam/hiwonder`, fetched by `fetch-cam.yml`), patched with:
- the two arm colors set in `colors.h` (0 red, 1 yellow, 2 green, 3 blue, 4 purple), on regs 0x00/0x01, plus all five colors on 0x10–0x14;
- a fix for their max-area bug.

The CI `camera` job builds it with arduino-esp32 2.0.17 (bundled esp-dl), 4 MB flash, huge_app, for both quad and octal PSRAM. The flasher's **Camera** button handles it. HSV thresholds live in `color_detection.cpp`.

**4. Hardware bring-up:** work through the unverified list below with the owner.

The phone gets its files from the `latest` release.

## Build / test

```
cd firmware/test && cc -I../main test_core.c ../main/core.c -lm -o t && ./t     # pure logic
cd web && npm i && npm run build      # also writes firmware/main/www/index.html.gz
cd web && npm run mock & ARM=http://localhost:8787 npm run dev                    # UI with no hardware
cd firmware && idf.py set-target esp32s3 && idf.py build flash monitor          # IDF >= 5.5, PC only
git push                              # CI builds + publishes release "latest"
```

## Architecture rules

- **One command vocabulary.** `router.c` turns text into actions. Voice phrases come from `router_phrases()`, and the web app posts plain text to `/api/cmd`. Add a command in the router once and both inputs get it. Don't add a voice-only or web-only action path.
- **Pure logic goes in `core.c`** (protocol, parsing, math) with a test in `firmware/test`. No ESP-IDF includes there.
- **Motion is streamed, never sent whole.** `arm.c` runs a 25 Hz task that walks each joint along a curve (min-jerk for `arm_move`, constant speed for `arm_move_lin` path segments, accel-limited chase for `arm_follow_one`, velocity for `arm_drive`) and sends each setpoint with a 60 ms move time, so servos never stop between frames. Don't write servo move frames anywhere else. `arm_hold()` brakes in place. The curve math lives in `core.c` (tested).
- **`pick.c` owns motion sequences.** One job at a time (queue length 1). `pick_stop()` must stay callable from any task and preempt within ~20 ms. Every new wait inside a sequence goes through `wait_ms()`.
- **Config lives in `cfg_t` (store.h).** If you change the struct layout, bump `MAGIC` in store.c, otherwise old NVS blobs load as garbage.
- **One shared I2C bus** (`i2c_master` driver, created in main.c) carries the codecs, TCA9555, RTC and the cam. Don't call the legacy `driver/i2c.h` API, because mixing the two drivers aborts at boot.
- **Web app:** keep it one gzipped file served from flash. Rebuild the web app after UI changes, since the firmware embeds `www/index.html.gz`. Home and Drop poses are taught on the Teach panel; Settings must never post them back.
- Servo index `i` = bus ID `i+1`. On the 1S: 1 gripper, 2 wrist rotate, 3 wrist, 4 elbow, 5 shoulder, 6 base. Positions run 0–1000.

## Unverified: check on hardware

1. **esp-sr / esp_codec_dev API and sdkconfig names.** `voice.c` follows xiaozhi-esp32 (esp-sr 2.4.7). Check that `CONFIG_SR_MN_EN_MULTINET7_QUANT` and friends exist under menuconfig → ESP Speech Recognition, and that the `model` partition gets flashed.
2. ~~Vision module data format~~ **Resolved from Hiwonder's source:** 4 × uint8 x,y,w,h on a 240×240 frame.
3. **Camera PSRAM type.** The flasher picks the octal or quad build from the chip features. Check on first flash that the cam boots (Boot log).
4. **Mic channels + AFE.** `audio.c` reads ES7210 TDM slots 0 (MIC1) and 1 (MIC3 = speaker reference, per xiaozhi's `AUDIO_INPUT_REFERENCE`) as AFE format `"MR"`; `XARM_MIC_DUAL` adds slot 2 (MIC2) as `"MRM"`. Boot log prints the AFE pipeline. Check that AEC actually cancels the bells (say "stop" during a trick) and that the level meter peaks around −12 to −6 dBFS when talking at arm's length (gain 24 dB default, AGC on).
5. **Gripper direction and lift sign.** Defaults are open 200 / close 650 and shoulder lift −120. Users fix these in Settings.
6. **Controller TX logic level** (3.3 V expected), and that the vision module doesn't pull SDA/SCL up to 5 V.
7. **MultiNet phrase rejection.** `load_commands()` logs rejected phrases. Keep phrases short and plain English.

## Pin facts (from xiaozhi-esp32 `boards/waveshare/esp32-s3-audio-board`)

- I2C: SDA 11, SCL 10.
- I2S: MCLK 12, BCLK 13, WS 14, DIN 15, DOUT 16.
- WS2812 ring: GPIO38 (6 LEDs).
- TCA9555: EXIO8 = speaker PA enable, EXIO0/1 = LCD/touch reset, EXIO5 = DVP camera reset.
- Header IO3/4/5/7/8/9 = LCD SPI pins, free with no LCD. Arm UART is TX 5 / RX 6. IO19/20 = USB, don't use.

## Next features (natural extension points)

- **Feedback:** LED ring states (listening/heard/moving/fail) via `led_strip` on GPIO38. Spoken or beep replies via ES8311: init per xiaozhi `box_audio_codec.cc`, PA on TCA9555 EXIO8. Hook both in `pick.c set()` and in the voice detect handler.
- **Wrist alignment:** use the blob's w/h to set wrist rotate (id 2) for long objects.
- **Hiwonder action groups:** `xf_action()` exists. Wire router phrases like "wave" to `XCMD_ACTION_RUN`.
- **Better recognition (done):** ESP-SR AFE (AEC, AGC, WebRTC VAD; BSS with `XARM_MIC_DUAL`) sits in front of MultiNet. A command only fires if the VAD heard speech within `XARM_VOICE_VAD_HOLD_MS`; while the speaker plays, only stop phrases pass. Voice phrases are 3+ syllables and acoustically distinct; keep new ones that way.
- **LLM layer:** anything that can produce router text (xiaozhi MCP, a server) can drive the arm through `router_exec()`.
- **Push instead of poll:** a websocket for `/api/state` if 4 Hz polling gets in the way.
